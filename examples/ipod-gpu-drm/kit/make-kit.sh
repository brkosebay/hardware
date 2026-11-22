#!/usr/bin/env bash
# kit/make-kit.sh OUT: pack the A8 GPU tester kit (kit/README.md) as OUT/$KIT_NAME.tar.gz, from pieces built
# beforehand. The checkout must be committed: the kit carries `git archive HEAD` of it as orchard/.
#   KIT_NAME        e.g. a8-gpu-tester-kit-2
#   KIT_KERNEL      the apple-4k kernel bundle `orchard build examples/minimal-boot --device n102` staged
#                   (the directory of Image.gz, dtbs/, lib/, kernel.release; out/apple-4k/build.json names it)
#   KIT_TOOLS       orchard's static tools directory for this checkout (the recovery-tools cache entry)
#   KIT_M1N1        orchard's m1n1.bin;  KIT_PONGO: orchard's released pongo.bin
#   KIT_A8VK        the Vulkan bundle (vk/make-bundle.sh);  KIT_A8_GPU_DRM: src/a8-gpu-drm.c built (build.sh)
#   KIT_SOURCES     a text file naming the source commits of the prebuilt pieces (goes into SOURCES.txt)
#   KIT_BRANCH      optional: where that commit is published, e.g. "branch a8-gpu-tester-kit of github.com/OWNER/hardware"
# Nothing from Apple goes in: the GPU firmware is fetched by setup.sh on the tester's machine, and this
# script refuses a kit that contains the firmware files.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
orchard=$(cd "$here/../.." && pwd)
out=$(mkdir -p "${1:?usage: make-kit.sh OUT}" && cd "$1" && pwd)
: "${KIT_NAME:?}" "${KIT_KERNEL:?}" "${KIT_TOOLS:?}" "${KIT_M1N1:?}" "${KIT_PONGO:?}" "${KIT_A8VK:?}" \
  "${KIT_A8_GPU_DRM:?}" "${KIT_SOURCES:?}"
[[ -z $(git -C "$orchard" status --porcelain --untracked-files=no) ]] || { echo "make-kit: commit the checkout first" >&2; exit 1; }

kit=$out/$KIT_NAME
rm -rf "$kit" "$kit.tar.gz"
mkdir -p "$kit/orchard" "$kit/prebuilt"
git -C "$orchard" archive HEAD | tar -x -C "$kit/orchard"
cp "$here/kit/setup.sh" "$here/kit/run.sh" "$kit/"
cp "$here/kit/README.md" "$kit/README.md"
cp -R "$KIT_KERNEL" "$kit/prebuilt/kernel"
cp "$KIT_M1N1" "$kit/prebuilt/m1n1.bin"
cp "$KIT_PONGO" "$kit/prebuilt/pongo.bin"
cp "$KIT_A8_GPU_DRM" "$kit/prebuilt/a8-gpu-drm"
cp -R "$KIT_A8VK" "$kit/prebuilt/a8vk"

# the static tools as an orchard cache entry under the key the kit's checkout computes, so that
# `orchard build --cache prebuilt/cache` reuses them instead of building them. -B: no __pycache__ in the
# kit, because .pyc files record the packer's absolute paths
key=$(cd "$kit/orchard" && "$orchard/bin/orchard" python -I -B -c \
  'import sys; sys.path.insert(0, "src"); from pathlib import Path; from orchard.build import recipe_key, TOOL_INPUTS; print(recipe_key(Path("."), TOOL_INPUTS))')
entry=$kit/prebuilt/cache/components-v2/recovery-tools/$key
mkdir -p "$entry"
for tool in "$KIT_TOOLS"/*; do
  case ${tool##*/} in component.json|commands.json) continue ;; esac
  cp "$tool" "$entry/"
done
python3 -I - "$entry" <<'EOF'
import hashlib, json, sys
from pathlib import Path
entry = Path(sys.argv[1])
files = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(entry.iterdir()) if p.is_file()}
(entry / 'component.json').write_text(json.dumps(files, indent=2, sort_keys=True) + '\n')
EOF

{
  echo "$KIT_NAME"
  echo
  echo "orchard/: the orchard checkout this kit was packed from, commit $(git -C "$orchard" rev-parse HEAD)"
  echo "  (git archive; ${KIT_BRANCH:-its branch})"
  echo
  cat "$KIT_SOURCES"
} > "$kit/SOURCES.txt"

if find "$kit" -name 'gfx-t7000*' | grep -q . || [[ -d $kit/orchard/examples/ipod-gpu-drm/rootfs/lib/firmware ]]; then
  echo "make-kit: the kit contains GPU firmware; it must not" >&2
  exit 1
fi
if find "$kit" -name __pycache__ -o -name '*.pyc' | grep -q .; then
  echo "make-kit: the kit contains Python bytecode, which records local paths; it must not" >&2
  exit 1
fi
(cd "$kit" && find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256 > SHA256SUMS)
# root-owned entries without extended attributes: the archive does not carry the packer's user and group
# names, nor macOS metadata such as com.apple.provenance
if tar --version 2>/dev/null | grep -q 'GNU tar'; then
  flags=(--owner=0 --group=0 --numeric-owner --no-xattrs)
else
  flags=(--uid 0 --gid 0 --uname root --gname root --no-xattrs --no-mac-metadata --no-acls --no-fflags)
fi
COPYFILE_DISABLE=1 tar "${flags[@]}" -C "$out" -czf "$kit.tar.gz" "$KIT_NAME"
shasum -a 256 "$kit.tar.gz"
