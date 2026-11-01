#!/bin/sh
# Generate a fresh X auth cookie for :1 at /tmp/xauth-igpu.
#
# Split out of igpu-server-up.sh so that script and squint-igpu-server.service
# share one definition: the cookie file path and mode are baked into the sudoers
# rule and into every :1 client, so the two must not drift.
#
# Mode 0644 is intentional -- squint, xterm and xrandr all read this as the
# user, and it is a display cookie for an output-only server, not a secret.

set -e

XAUTH_FILE="/tmp/xauth-igpu"

rm -f "$XAUTH_FILE"
xauth -f "$XAUTH_FILE" add :1 . "$(mcookie)"
chmod 644 "$XAUTH_FILE"
