#!/bin/sh
# Play video end to end in an arm64 virtual machine that draws on the host GPU.
# QEMU emulates the arm64 CPU. Its virtio GPU sends the guest's OpenGL calls to
# the host GPU through virglrenderer, so Mesa's virgl driver draws the frames
# in the guest, not llvmpipe.
#
# Usage: test/e2e/run-gpu.sh
#
# The host needs Docker, a binfmt rule for arm64 and a GPU render node. Set
# OWE_E2E_RENDER_NODE to use a render node other than /dev/dri/renderD128.
# Set OWE_E2E_HWDEC to an mpv hwdec value, such as vaapi, to decode through
# virgl. The check then fails if mpv does not decode with that value. This
# needs a QEMU that turns on virglrenderer video decode. QEMU 11.1 does not,
# so the check fails with it.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
render=${OWE_E2E_RENDER_NODE:-/dev/dri/renderD128}
if [ ! -c "$render" ]; then
    echo "No GPU render node at $render. Set OWE_E2E_RENDER_NODE to a node in /dev/dri." >&2
    exit 1
fi
if [ "$(uname -m)" != aarch64 ] && [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
    echo "No binfmt rule for aarch64. Install one, then run this script again:" >&2
    echo "  Arch Linux:   sudo pacman -S qemu-user-static-binfmt" >&2
    echo "  Other hosts:  docker run --privileged --rm tonistiigi/binfmt --install arm64" >&2
    exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/owe-e2e-vm.XXXXXX")
guest=""
cleanup() {
    if [ -n "$guest" ]; then
        docker rm -f "$guest" >/dev/null
    fi
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
# The build container runs as the owe user of the image and writes the binaries here.
chmod 0777 "$work"

echo "== Build the arm64 binaries"
docker build --platform linux/arm64 -t owe-e2e:linux-arm64 "$root/test/e2e"
docker run --rm --init --platform linux/arm64 -e OWE_E2E_BIN=/out \
    -v "$root:/src:ro" -v "$work:/out" owe-e2e:linux-arm64 --build-only

echo "== Build the guest and VM host images"
docker build --platform linux/arm64 -t owe-e2e-guest:linux-arm64 \
    -f "$root/test/e2e/vm/Dockerfile.guest" "$root/test/e2e/vm"
docker build -t owe-e2e-vm-host -f "$root/test/e2e/vm/Dockerfile.host" "$root/test/e2e/vm"

guest=$(docker create --platform linux/arm64 owe-e2e-guest:linux-arm64)
docker export "$guest" | docker run --rm -i --device "$render" \
    -e OWE_E2E_RENDER_NODE="$render" -e OWE_E2E_TIMEOUT -e OWE_E2E_VM_CPUS -e OWE_E2E_HWDEC \
    -v "$root:/src:ro" -v "$work:/owe-bin:ro" owe-e2e-vm-host
