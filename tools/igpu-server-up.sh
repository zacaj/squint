#!/bin/sh
# Detach the two iGPU outputs from :0 and start a standalone Xorg :1 on the
# Intel iGPU, with auto-restart on crash.
# Run igpu-server-down.sh to stop everything and restore :0.

IGPU_OUT1="DP-1-3"
IGPU_OUT2="HDMI-1-1"
CONFDIR="$(cd "$(dirname "$0")" && pwd)/xorg-igpu.conf.d"
# Next to :0's own Xorg log, not /tmp: /tmp is tmpfs and a GPU hang forcing a
# reboot used to take this log down with it, right when it was most needed.
XLOG="/var/home/zacaj/.local/share/xorg/Xorg.1.log"
XAUTH_FILE="/tmp/xauth-igpu"
WATCHER_PID_FILE="/tmp/xorg-igpu-watcher.pid"
XORG_PID_FILE="/tmp/xorg-igpu.pid"

# :1 is normally started by squint-igpu-server.service at login; this script is
# the manual path. Refuse to start a second server on top of that one -- two
# Xorgs on the same iGPU is a much more confusing failure than an early exit.
if [ -S /tmp/.X11-unix/X1 ]; then
	echo "error: :1 is already running." >&2
	echo "  managed by systemd? systemctl --user status squint-igpu-server" >&2
	echo "  stop it with:        tools/igpu-server-down.sh" >&2
	exit 1
fi

echo "Creating auth file for :1..."
"$(dirname "$0")/igpu-xauth-init.sh"
export XAUTHORITY="$XAUTH_FILE"

start_xorg() {
	echo "Starting Xorg :1 on iGPU (sharing current VT, no switch)..."
	sudo Xorg :1 -configdir "$CONFDIR" -layout iGPULayout -logfile "$XLOG" -auth "$XAUTH_FILE" -sharevts -novtswitch -isolateDevice PCI:0:2:0 &
	echo $! > "$XORG_PID_FILE"

	for i in $(seq 1 20); do
		DISPLAY=:1 XAUTHORITY="$XAUTH_FILE" xdpyinfo >/dev/null 2>&1 && return 0
		sleep 0.5
	done
	echo "WARNING: :1 did not become ready within 10s. Check $XLOG" >&2
	return 1
}

# Initial start
start_xorg || exit 1

echo ""
echo ":1 is up (PID $(cat $XORG_PID_FILE))."

# Background watcher: restart Xorg if it dies, until watcher itself is killed
(
	while true; do
		XPID=$(cat "$XORG_PID_FILE" 2>/dev/null)
		if [ -n "$XPID" ] && ! kill -0 "$XPID" 2>/dev/null; then
			echo "[watcher] Xorg :1 (pid $XPID) died — restarting..." >&2
			sleep 1
			start_xorg
		fi
		sleep 2
	done
) &
WATCHER_PID=$!
echo "$WATCHER_PID" > "$WATCHER_PID_FILE"
echo "Auto-restart watcher running (PID $WATCHER_PID)."
echo "  Kill watcher + Xorg: tools/igpu-server-down.sh"
echo "  Kill watcher only:   kill $WATCHER_PID"
echo ""

TOOLS="$(cd "$(dirname "$0")" && pwd)"
echo "XAUTHORITY=$XAUTH_FILE is needed for all :1 clients."
echo ""
echo "Benchmark commands (build first: gcc -O2 -o tools/xbench tools/xbench.c -lX11 -lXext):"
echo "  XAUTHORITY=$XAUTH_FILE $TOOLS/xbench :0 :1 3072 1728 10"
echo "  XAUTHORITY=$XAUTH_FILE $TOOLS/xbench :0 :1 1920 1080 10"
