#!/bin/sh
# Build squint from the host (no ninja there) using the devcontainer image.
# builddir/ was configured inside the devcontainer, so its paths are absolute
# under /workspaces/squint -- the repo must be mounted at exactly that path.
#
# Usage: tools/build.sh [extra ninja args]
set -e
repo="$(cd "$(dirname "$0")/.." && pwd)"
image="$(docker images --format '{{.Repository}}' | grep '^vsc-squint-.*-uid$' | head -1)"
if [ -z "$image" ]; then
	echo "build.sh: no vsc-squint-*-uid image; open the devcontainer once in VS Code to build it" >&2
	exit 1
fi
exec docker run --rm -v "$repo:/workspaces/squint:z" -w /workspaces/squint \
	--user "$(id -u):$(id -g)" "$image" ninja -C builddir "$@"
