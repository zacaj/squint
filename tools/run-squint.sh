#!/bin/sh
# Launch squint mirroring a monitor from :0 (AMD) onto a monitor on :1
# (Intel iGPU, see igpu-server-up.sh). Assumes :1 is already running.
#
# Usage: tools/run-squint.sh [SRC_MONITOR] [DST_MONITOR]
#   SRC_MONITOR / DST_MONITOR: number from the listings this script prints
#   (defaults to 1 / 1 if omitted)

set -e

BIN="$(cd "$(dirname "$0")/.." && pwd)/builddir/squint"
IGPU_XAUTH="/tmp/xauth-igpu"
COMBINED_AUTH="/tmp/xauth-squint-combined"

if [ ! -S /tmp/.X11-unix/X1 ]; then
	echo "error: :1 is not running. Start it first with tools/igpu-server-up.sh" >&2
	exit 1
fi

if [ ! -f "$IGPU_XAUTH" ]; then
	echo "error: $IGPU_XAUTH not found. Start :1 first with tools/igpu-server-up.sh" >&2
	exit 1
fi

# Build a combined auth file: the real :0 cookie (from the user's own
# Xauthority) plus :1's cookie (from igpu-server-up.sh). We deliberately do
# NOT just merge ~/.Xauthority wholesale -- it already has a stale, unrelated
# entry for local display :1 (leftover from something else, e.g. xrdp) that
# would collide with our freshly-generated :1 cookie and break auth.
rm -f "$COMBINED_AUTH"
touch "$COMBINED_AUTH"
chmod 600 "$COMBINED_AUTH"
xauth -f "$HOME/.Xauthority" nlist :0 | xauth -f "$COMBINED_AUTH" nmerge -
xauth -f "$IGPU_XAUTH" nlist :1 | xauth -f "$COMBINED_AUTH" nmerge -

export XAUTHORITY="$COMBINED_AUTH"

echo "=== :0 (AMD) source monitors ==="
DISPLAY=:0 "$BIN" --list-monitors 2>/dev/null | grep -v '^$'

echo ""
echo "=== :1 (iGPU) destination monitors ==="
# Bring up any connected-but-unconfigured iGPU outputs (xrandr won't assign
# a mode/position automatically at Xorg startup).
DISPLAY=:1 xrandr --auto >/dev/null 2>&1 || true
DISPLAY=:0 "$BIN" --list-monitors --dst-display :1 2>/dev/null | grep -v '^$'

SRC="${1:-1}"
DST="${2:-1}"

echo ""
echo "Mirroring :0 monitor $SRC -> :1 monitor $DST (windowed; Ctrl+C to stop)"
echo ""

export DISPLAY=:1
exec "$BIN" --src-display :0 --dst-display :1 -S "$SRC" -D "$DST" -w
