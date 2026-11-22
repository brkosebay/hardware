#!/bin/sh
# run.sh BOARD: boot the payload setup.sh assembled on the board in DFU mode (checkm8, PongoOS, m1n1, Linux;
# RAM only, nothing is written to the device). make-kit.sh puts it at the top of the kit.
set -eu
cd "$(dirname "$0")"
board=${1:?usage: run.sh BOARD (n102 j96 j97 n61 n56)}
exec orchard/bin/orchard run orchard/examples/ipod-gpu-drm --device "$board" --no-build
