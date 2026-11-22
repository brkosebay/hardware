#!/bin/sh
# setup.sh BOARD: fetch the GPU firmware from Apple's IPSW and assemble the payload for BOARD
# (n102, j96, j97, n61, n56) from the kit's prebuilt pieces. make-kit.sh puts it at the top of the kit.
set -eu
cd "$(dirname "$0")"
exec orchard/examples/ipod-gpu-drm/kit/assemble.sh prebuilt "$@"
