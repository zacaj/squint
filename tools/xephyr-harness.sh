#!/bin/bash
set -e

# Phase 1 Xephyr harness: spawn two nested X servers and test Squint cross-server mirroring
# Usage: ./tools/xephyr-harness.sh [start|stop|run]

XEPHYR_SRC=${XEPHYR_SRC:-:10}
XEPHYR_DST=${XEPHYR_DST:-:11}
XEPHYR_GEOM=${XEPHYR_GEOM:-1920x1080}

# Find builddir (assume called from repo root)
BUILDDIR="${BUILDDIR:-builddir}"
SQUINT="${BUILDDIR}/squint"

if [ ! -x "$SQUINT" ]; then
    echo "Error: $SQUINT not found. Run 'ninja -C builddir' first."
    exit 1
fi

start() {
    echo "Starting Xephyr servers..."

    # Start source Xephyr (will display test content)
    Xephyr -screen "${XEPHYR_GEOM}" "$XEPHYR_SRC" &
    SRC_PID=$!

    # Start destination Xephyr
    Xephyr -screen "${XEPHYR_GEOM}" "$XEPHYR_DST" &
    DST_PID=$!

    sleep 2  # Wait for servers to initialize

    echo "Source server: $XEPHYR_SRC (PID $SRC_PID)"
    echo "Dest server:   $XEPHYR_DST (PID $DST_PID)"
    echo "Save these PIDs to kill manually: kill $SRC_PID $DST_PID"
}

stop() {
    echo "Stopping Xephyr servers..."
    pkill -f "Xephyr.*:10\|Xephyr.*:11" || true
    sleep 1
}

run() {
    echo "Running Squint in cross-server mode..."
    export DISPLAY=:11  # Mirror output goes to dest

    # Run with test content on source
    DISPLAY=$XEPHYR_SRC xwd -root -out /tmp/test.xwd &
    TEST_PID=$!
    sleep 1

    # Launch Squint
    "$SQUINT" --src-display "$XEPHYR_SRC" --dst-display "$XEPHYR_DST" --window

    kill $TEST_PID 2>/dev/null || true
}

case "${1:-start}" in
    start) start ;;
    stop) stop ;;
    run) run ;;
    *)
        echo "Usage: $0 {start|stop|run}"
        echo "  start: spawn Xephyr servers"
        echo "  stop:  kill Xephyr servers"
        echo "  run:   launch Squint in cross-server mode"
        exit 1
        ;;
esac
