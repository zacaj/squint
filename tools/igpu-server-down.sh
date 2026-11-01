#!/bin/sh
# Stop the watcher, kill Xorg :1, and restore :0 via zrandr.sh.

# Stop the systemd-managed path first. If :1 was started by the unit, killing
# its pid directly is pointless -- Restart=always brings it straight back.
# Stopping the server unit also takes down the status/keepalive unit (PartOf=).
if systemctl --user is-active --quiet squint-igpu-server.service; then
	echo "Stopping squint-igpu-* user units..."
	systemctl --user stop squint-igpu-mirrors.service squint-igpu-server.service
	echo "Done."
	exit 0
fi

echo "Stopping auto-restart watcher..."
if [ -f /tmp/xorg-igpu-watcher.pid ]; then
	kill "$(cat /tmp/xorg-igpu-watcher.pid)" 2>/dev/null || true
	rm /tmp/xorg-igpu-watcher.pid
fi

echo "Stopping Xorg :1..."
if [ -f /tmp/xorg-igpu.pid ]; then
	sudo kill "$(cat /tmp/xorg-igpu.pid)" 2>/dev/null || true
	rm /tmp/xorg-igpu.pid
else
	sudo pkill -f "Xorg :1" 2>/dev/null || true
fi

sleep 0.5

# echo "Restoring :0 display layout via zrandr.sh..."
# ~/bin/zrandr.sh

echo "Done."
