#!/bin/sh
# wayland-nested-test.sh — exercise squint's --wayland-source path without
# logging into a Wayland session: runs a nested kwin_wayland (a window on the
# current X11 display) on a PRIVATE D-Bus session bus, so the screen cast
# portal + xdg-desktop-portal-kde instances activated there talk to the nested
# KWin instead of the real desktop. squint then mirrors the nested compositor's
# output into a window on $DST_DISPLAY.
#
# Usage: tools/wayland-nested-test.sh [extra squint args...]
#   DST_DISPLAY  destination X display for the mirror window (default :0)
#   CLIENT       command run inside the nested compositor (default eglgears_wayland)
#
# The first run shows the portal's monitor picker inside the nested KWin window;
# pick its single output. The permission is remembered under --portal-id
# "nested-test" (~/.local/state/squint/portal-nested-test.token).

TOOLS="$(cd "$(dirname "$0")" && pwd)"
BIN="$TOOLS/../builddir/squint"
SOCK="squint-wl-test"
DST_DISPLAY="${DST_DISPLAY:-:0}"
CLIENT="${CLIENT:-eglgears_wayland}"

if [ -z "$INSIDE_PRIVATE_BUS" ]; then
	export INSIDE_PRIVATE_BUS=1
	# Set before the private bus starts: D-Bus-activated services (the portal
	# frontend and the KDE backend) inherit the bus daemon's environment.
	export WAYLAND_DISPLAY="$SOCK"
	export XDG_CURRENT_DESKTOP=KDE
	export XDG_SESSION_TYPE=wayland
	export DISPLAY="$DST_DISPLAY"
	exec dbus-run-session -- "$0" "$@"
fi

cleanup() {
	kill $SQUINT_PID $CLIENT_PID $KWIN_PID 2>/dev/null
}
trap cleanup EXIT INT TERM

# kwin_wayland's nested X11 backend opens a window on $DISPLAY.
kwin_wayland --x11-display "$DST_DISPLAY" --socket "$SOCK" \
	--width 1280 --height 720 --no-lockscreen &
KWIN_PID=$!

i=0
while [ ! -S "$XDG_RUNTIME_DIR/$SOCK" ]; do
	i=$((i+1))
	[ $i -gt 50 ] && { echo "nested kwin did not come up" >&2; exit 1; }
	sleep 0.2
done
echo "nested kwin up on $SOCK"

$CLIENT &
CLIENT_PID=$!

"$BIN" --wayland-source --portal-id nested-test --dst-display "$DST_DISPLAY" \
	-w "$@" &
SQUINT_PID=$!
wait $SQUINT_PID
