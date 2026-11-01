#!/bin/sh
# igpu-mirrors-up.sh — configure the :1 (Intel iGPU) outputs and start the
# squint mirrors that duplicate regions of the :0 (AMD) desktop onto them.
#
# Prerequisite: the standalone :1 Xorg must already be running, i.e. run
#   tools/igpu-server-up.sh
# first (it detaches the iGPU outputs from :0 and starts Xorg :1).
#
# Layout produced on :1:
#   DP-3   (4K)    primary, at 0,0
#   HDMI-1 (1440p) to the right of DP-3, at 3840,0
#
# Mirrors started (both fullscreen; the EGL path scales source->dest on the GPU):
#   1) :0 monitor 1  (4K)               -> DP-3    (1:1)
#   2) :0 monitors 2,3,4 (the other 3)  -> HDMI-1  (cursor-following; squint
#                                                    shows whichever of the three
#                                                    the pointer is currently on)
#
# Edit the SRC/DST/mode variables below if the monitor numbering changes
# (see: DISPLAY=:0 builddir/squint --list-monitors
#   and DISPLAY=:0 builddir/squint --list-monitors --dst-display :1).

set -e

TOOLS="$(cd "$(dirname "$0")" && pwd)"
BIN="$TOOLS/../builddir/squint"
IGPU_XAUTH="/tmp/xauth-igpu"
COMBINED_AUTH="/tmp/xauth-squint-combined"
# Mirror output is streamed to the journal (see step 5) rather than /tmp files:
# /tmp is tmpfs, so file-based logs die with the reboot that a mirror-caused
# GPU hang usually forces -- taking the only evidence with them.
FIFO1="/tmp/squint-mirror1.fifo"
FIFO2="/tmp/squint-mirror2.fifo"
TAG1="squint-mirror1"
TAG2="squint-mirror2"

# Root colour for :1, visible only when the mirrors aren't covering it. Bright
# and unmistakable on purpose: black means X itself is down or the panels are
# asleep, this colour means X is fine and squint is what's missing.
ROOT_COLOR="#ff0000"
STATUS_TITLE="squint-igpu-status"

# --- :1 output layout ---
DST_4K="DP-3";     DST_4K_MODE="3840x2160";  DST_4K_POS="0x0"
DST_HD="HDMI-1";   DST_HD_MODE="2560x1440";  DST_HD_POS="3840x0"

# --- source(:0) -> dest(:1 output name) mapping ---
# Use output names, not squint's monitor numbers: squint numbers :1's monitors
# by RandR output-detection order, not by which one is --primary, so the
# number assigned to DP-3 vs HDMI-1 is not guaranteed stable across restarts.
M1_SRC="1";      M1_DST="$DST_4K"   # :0 4K monitor          -> DP-3
M2_SRC="2,3,4";  M2_DST="$DST_HD"   # :0 other three (roam)  -> HDMI-1

# --- 1. sanity checks -------------------------------------------------------
if [ ! -S /tmp/.X11-unix/X1 ]; then
	echo "error: :1 is not running. Start it first with tools/igpu-server-up.sh" >&2
	exit 1
fi
if [ ! -f "$IGPU_XAUTH" ]; then
	echo "error: $IGPU_XAUTH not found. Start :1 first with tools/igpu-server-up.sh" >&2
	exit 1
fi
if [ ! -x "$BIN" ]; then
	echo "error: $BIN not built. Run: ninja -C builddir" >&2
	exit 1
fi

# --- 2. combined X auth (:0 cookie + :1 cookie) -----------------------------
# Same approach as run-squint.sh: take the real :0 cookie from the user's
# Xauthority plus :1's freshly-generated cookie. We deliberately do NOT merge
# ~/.Xauthority wholesale -- it may carry a stale, unrelated :1 entry that
# would collide with our cookie and break auth.
rm -f "$COMBINED_AUTH"
touch "$COMBINED_AUTH"
chmod 600 "$COMBINED_AUTH"
xauth -f "$HOME/.Xauthority" nlist :0 | xauth -f "$COMBINED_AUTH" nmerge -
xauth -f "$IGPU_XAUTH"        nlist :1 | xauth -f "$COMBINED_AUTH" nmerge -
export XAUTHORITY="$COMBINED_AUTH"

# --- 3. stop any mirrors from a previous run --------------------------------
# Match the full argv, not just the binary: a bare "$BIN" pattern would also
# match the status window's helper processes.
pkill -f "$BIN --src-display" 2>/dev/null || true
sleep 0.3

# --- 3b. keepalive + status window on :1 ------------------------------------
# :1 must keep at least one client connected at all times. X resets the server
# when the last client disconnects -- repainting the root black and restoring
# the screensaver/DPMS defaults from xorg.conf -- so with squint as the only
# client, the root colour set here would vanish exactly when squint died. The
# status xterm holds that connection open and reports :1's health; being mapped
# before the mirrors, it stays stacked underneath them and never obstructs them.
if pgrep -f "$STATUS_TITLE" >/dev/null 2>&1; then
	echo "Status window already running on :1"
else
	echo "Starting :1 keepalive/status window"
	DISPLAY=:1 xterm -title "$STATUS_TITLE" -geometry 64x14+120+120 \
		-fa Monospace -fs 18 -bg '#101010' -fg '#00ff66' \
		-e "$TOOLS/igpu-status-window.sh" >/dev/null 2>&1 &
	sleep 0.5
fi
DISPLAY=:1 xsetroot -solid "$ROOT_COLOR"

# --- 4. configure :1 outputs (HDMI-1 to the right of DP-3) -------------------
echo "Configuring :1 outputs: $DST_4K ($DST_4K_MODE) + $DST_HD ($DST_HD_MODE, right of it)"
DISPLAY=:1 xrandr \
	--output "$DST_4K" --mode "$DST_4K_MODE" --pos "$DST_4K_POS" --primary \
	--output "$DST_HD" --mode "$DST_HD_MODE" --pos "$DST_HD_POS"

# Give the RandR change a moment to propagate before launching.
sleep 1

# --- 5. start the mirrors (fullscreen, passive) -----------------------------
# -p (passive) keeps each mirror pinned and always-on. In passive fullscreen
# mode squint shows the window immediately (on its destination monitor, which it
# positions itself) with its first listed source. Mirror 2 lists three sources
# and switches between them as the :0 cursor moves onto each -- cross-server
# cursor-follow tracks the SOURCE (:0) pointer.
#
# Output goes to the journal via a FIFO rather than straight to /dev/null or a
# /tmp file: a mirror that dies on its own leaves no other trace, and its last
# lines are the only evidence of why. Each mirror gets its own FIFO with a
# systemd-cat reader started FIRST -- a FIFO reader exits on EOF (all writers
# closed) and will NOT wait for a new writer to reconnect, so the reader must
# already be attached before the mirror opens its end, and each FIFO must have
# exactly one writer for its whole lifetime (the mirror process itself) or an
# earlier writer closing would kill the reader before the mirror even starts.
# That also means the "started"/"exited" banners below go to this script's own
# stdout/stderr -- already captured under the squint-igpu-mirrors unit's own
# journal stream -- rather than through the FIFOs.
rm -f "$FIFO1" "$FIFO2"
mkfifo "$FIFO1" "$FIFO2"
systemd-cat -t "$TAG1" <"$FIFO1" &
systemd-cat -t "$TAG2" <"$FIFO2" &

echo "Starting mirror 1: :0 [$M1_SRC] -> :1 monitor $M1_DST ($DST_4K)"
DISPLAY=:1 "$BIN" --src-display :0 --dst-display :1 -p -S "$M1_SRC" -D "$M1_DST" \
	>"$FIFO1" 2>&1 &
M1_PID=$!

echo "Starting mirror 2: :0 [$M2_SRC] -> :1 monitor $M2_DST ($DST_HD)"
DISPLAY=:1 "$BIN" --src-display :0 --dst-display :1 -p -S "$M2_SRC" -D "$M2_DST" \
	>"$FIFO2" 2>&1 &
M2_PID=$!

echo ""
echo "Mirrors running (pids $M1_PID, $M2_PID). Ctrl+C to stop both."
echo "Logs: journalctl -u squint-igpu-mirrors -t $TAG1 -t $TAG2 (or add -f to follow)"
echo "(Both mirrors show immediately. Mirror 2 follows the :0 cursor across its"
echo " source monitors ($M2_SRC), switching once the cursor moves ~75px into one.)"

# Stop both mirrors together on exit. The status window is deliberately left
# running: it is :1's keepalive, and it is what makes a dead mirror visible.
# STOPPING distinguishes "we were asked to stop" from "the mirrors died on us",
# which are the same code path but must not report the same exit status: only
# the latter is a failure worth restarting and flagging in systemctl status.
STOPPING=0
trap 'STOPPING=1; kill "$M1_PID" "$M2_PID" 2>/dev/null' INT TERM

# Report deaths as they happen instead of silently outliving them. Goes to
# this script's own stderr (journal, under squint-igpu-mirrors), not the
# mirror's FIFO: that FIFO's systemd-cat reader has already seen EOF and
# exited by the time we get here, since the mirror process was its only writer.
reap() {	# reap <pid> <label>
	rc=0
	wait "$1" || rc=$?
	echo "=== $2 exited rc=$rc at $(date '+%F %T') ===" >&2
}

# A mirror can be alive but useless: squint_enable() only fails and disables
# itself if the destination monitor lookup races :1's RandR layout at startup
# (e.g. "Source and destination both map the same screen area"), and nothing
# in squint retries that afterwards except a RandR screen-change event, which
# may never come again. That leaves it heartbeating "enabled=0" forever --
# alive, so `kill -0` above never sees a failure to restart on. Poll the
# heartbeat's enabled= field instead, and treat several stuck in a row as a
# death: kill both so the reap logic above restarts them the normal way, by
# which point :1's layout has long since settled and select_monitors() should
# succeed again.
HEARTBEAT_CHECK_SECS=65	# > the 60s heartbeat period, so each check sees a fresh line
STUCK_THRESHOLD=3		# consecutive stuck checks (~3.5min) before restarting
elapsed=0
m1_stuck=0
m2_stuck=0

heartbeat_enabled() {	# heartbeat_enabled <tag> -> prints last enabled= value, empty if none yet
	journalctl -t "$1" -n1 --no-pager -o cat 2>/dev/null \
		| sed -n 's/.*enabled=\([0-9]*\).*/\1/p'
}

while [ -n "$M1_PID" ] || [ -n "$M2_PID" ]; do
	if [ -n "$M1_PID" ] && ! kill -0 "$M1_PID" 2>/dev/null; then
		reap "$M1_PID" "mirror 1"
		M1_PID=""
	fi
	if [ -n "$M2_PID" ] && ! kill -0 "$M2_PID" 2>/dev/null; then
		reap "$M2_PID" "mirror 2"
		M2_PID=""
	fi
	# One dead mirror is already a broken setup -- half the iGPU panels are
	# showing the status screen instead of the desktop. Don't wait for the
	# survivor to die too; take it down and let systemd restart the pair, so
	# recovery doesn't depend on both failing.
	if [ "$STOPPING" = 0 ] && { [ -z "$M1_PID" ] || [ -z "$M2_PID" ]; }; then
		echo "A mirror died on its own -- stopping the other so both restart." >&2
		kill "$M1_PID" "$M2_PID" 2>/dev/null || :
		break
	fi

	elapsed=$((elapsed + 2))
	if [ "$STOPPING" = 0 ] && [ "$elapsed" -ge "$HEARTBEAT_CHECK_SECS" ]; then
		elapsed=0
		if [ -n "$M1_PID" ]; then
			v=$(heartbeat_enabled "$TAG1")
			if [ "$v" = "0" ]; then m1_stuck=$((m1_stuck + 1)); else m1_stuck=0; fi
		fi
		if [ -n "$M2_PID" ]; then
			v=$(heartbeat_enabled "$TAG2")
			if [ "$v" = "0" ]; then m2_stuck=$((m2_stuck + 1)); else m2_stuck=0; fi
		fi
		if [ "$m1_stuck" -ge "$STUCK_THRESHOLD" ] || [ "$m2_stuck" -ge "$STUCK_THRESHOLD" ]; then
			echo "A mirror is alive but stuck disabled (no re-enable after $((STUCK_THRESHOLD * HEARTBEAT_CHECK_SECS))s) -- restarting both." >&2
			kill "$M1_PID" "$M2_PID" 2>/dev/null || :
		fi
	fi

	# "|| :" matters: on SIGTERM this sleep is interrupted and returns non-zero,
	# and set -e would abort the script right here -- exiting 143 before the
	# STOPPING check below could turn a clean stop into a reported failure.
	sleep 2 || :
done

echo "Mirrors are down. :1 shows the status window on $ROOT_COLOR."

if [ "$STOPPING" = 1 ]; then
	exit 0	# asked to stop (Ctrl+C, systemctl stop) -- a clean shutdown
fi
exit 1		# died on their own -- let systemd restart and record it
