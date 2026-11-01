#!/bin/sh
# Status readout displayed on :1 whenever the squint mirrors aren't covering it.
#
# Launched inside an xterm by igpu-mirrors-up.sh. That xterm is also :1's
# *keepalive* client, which matters for a non-obvious reason: X performs a full
# server reset when its last client disconnects, repainting the root black and
# restoring screensaver/DPMS defaults. With squint as :1's only client, the
# moment squint dies the server wipes the root colour that would have told you
# it died. Holding one long-lived connection open prevents that reset.
#
# The mirrors are fullscreen and mapped after this xterm, so they stack above it
# and it stays invisible while they're running.

TOOLS="$(cd "$(dirname "$0")" && pwd)"
BIN="$TOOLS/../builddir/squint"
# Mirror stdout/stderr goes to the journal (tags squint-mirror1/2), not to a
# /tmp file -- see igpu-mirrors-up.sh. journalctl -t is the source of truth
# for "last line" below; a stale /tmp path here would silently show nothing.
TAG1="squint-mirror1"
TAG2="squint-mirror2"

while :; do
	n=$(pgrep -cf "$BIN --src-display" 2>/dev/null)
	printf '\033[H\033[2J'
	echo "squint iGPU mirror host  --  X :1 is ALIVE"
	echo "-----------------------------------------"
	echo "clock:   $(date '+%Y-%m-%d %H:%M:%S')  (ticks once a second;"
	echo "                              frozen clock => X or the GPU is stuck)"
	echo "mirrors: ${n:-0} of 2 running"
	echo ""
	echo "last line, mirror 1 ($TAG1):"
	echo "  $(journalctl -t "$TAG1" -n1 --no-pager -o cat 2>/dev/null)"
	echo "last line, mirror 2 ($TAG2):"
	echo "  $(journalctl -t "$TAG2" -n1 --no-pager -o cat 2>/dev/null)"
	echo ""
	echo "restart the mirrors with: $TOOLS/igpu-mirrors-up.sh"
	sleep 1
done
