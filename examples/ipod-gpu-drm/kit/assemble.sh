#!/usr/bin/env bash
# kit/assemble.sh PREBUILT BOARD: this payload from the A8 GPU tester kit's prebuilt pieces (kit/README.md),
# without building a kernel, Mesa or anything else. PREBUILT is the kit's prebuilt/ directory:
#   kernel/          the apple-4k kernel bundle (Image.gz, dtbs/, lib/modules/, kernel.release), as
#                    `orchard build` stages it; passed as --kernel-dir
#   m1n1.bin         orchard's m1n1 (--loader);  pongo.bin: orchard's released Pongo (--pongo)
#   cache/           an orchard cache with the static tools (BusyBox, Dropbear, ...) for this checkout's
#                    recipes, so nothing is built or fetched from the nightly (--cache, ORCHARD_PUBLISHED=off)
#   a8-gpu-drm       src/a8-gpu-drm.c, built                 -> rootfs/usr/bin/a8-gpu-drm
#   a8vk/            the Vulkan bundle (vk/make-bundle.sh)   -> rootfs/opt/a8vk
# The GPU firmware is not in the kit: tools/extract-fw.py fetches it from Apple's public IPSW, as build.sh does.
# Then: bin/orchard run examples/ipod-gpu-drm --device BOARD --no-build
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
orchard=$(cd "$here/../.." && pwd)
usage='usage: kit/assemble.sh PREBUILT BOARD (BOARD: n102 j96 j97 n61 n56)'
die() { echo "kit: $*" >&2; exit 1; }
prebuilt=$(cd "${1:?$usage}" && pwd)
board=${2:?$usage}
case $board in
n102|j96|j97|n61|n56) ;;
*) die "$board is not an A8 board; $usage" ;;
esac
for f in kernel/kernel.release kernel/Image.gz kernel/dtbs/t7000-$board.dtb m1n1.bin pongo.bin a8-gpu-drm \
         a8vk/a8-vk-smoke a8vk/lib/libvulkan_powervr_mesa.so; do
  [[ -e $prebuilt/$f ]] || die "$prebuilt has no $f"
done
command -v uv >/dev/null || die 'install uv first: https://docs.astral.sh/uv/getting-started/installation/'
python3 -c 'import sys; sys.exit(sys.version_info < (3, 11))' 2>/dev/null || die 'needs python3 3.11 or newer'

mkdir -p "$here/rootfs/usr/bin" "$here/rootfs/lib/firmware/apple" "$here/rootfs/opt"
python3 -I "$here/tools/extract-fw.py" "$here/rootfs/lib/firmware/apple"
cp "$prebuilt/a8-gpu-drm" "$here/rootfs/usr/bin/a8-gpu-drm"
chmod 755 "$here/rootfs/usr/bin/a8-gpu-drm"
rm -rf "$here/rootfs/opt/a8vk" "$here/rootfs/usr/share/a8-gpu-drm"
cp -R "$prebuilt/a8vk" "$here/rootfs/opt/a8vk"

ORCHARD_PUBLISHED=off "$orchard/bin/orchard" build "$here" --device "$board" --kernel-dir "$prebuilt/kernel" \
  --loader "$prebuilt/m1n1.bin" --pongo "$prebuilt/pongo.bin" --cache "$prebuilt/cache"
"$here/tools/verify-payload.sh"
cat <<EOF

kit: the payload is ready: $here/out/apple-4k
Put the $board in DFU mode, then run ./run.sh $board in the kit, which runs:
  $orchard/bin/orchard run $here --device $board --no-build
Keep this directory where it is: the payload records its paths.
EOF
