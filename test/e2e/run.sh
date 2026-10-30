#!/bin/sh
# Build owe for another architecture in Docker, run the test suites, and play
# video end to end on a headless sway session.
#
# Usage: test/e2e/run.sh [platform]
#
# The platform defaults to linux/arm64. On an x86_64 host, Docker runs the
# image through QEMU, so the host needs binfmt rules for the target.
set -eu

platform=${1:-linux/arm64}
root=$(cd "$(dirname "$0")/../.." && pwd)
tag="owe-e2e:$(printf '%s' "$platform" | tr / -)"
target=${platform#linux/}

# Map the Docker architecture to the uname and QEMU name.
case "$target" in
    arm64) qemu=aarch64 ;;
    amd64) qemu=x86_64 ;;
    arm/*) qemu=arm ;;
    *) qemu=$target ;;
esac
emulated=0
if [ "$(uname -m)" != "$qemu" ]; then
    emulated=1
    if [ ! -e "/proc/sys/fs/binfmt_misc/qemu-$qemu" ]; then
        echo "No binfmt rule for $qemu. Install one, then run this script again:" >&2
        echo "  Arch Linux:   sudo pacman -S qemu-user-static-binfmt" >&2
        echo "  Other hosts:  docker run --privileged --rm tonistiigi/binfmt --install $target" >&2
        exit 1
    fi
fi

docker build --platform "$platform" -t "$tag" "$root/test/e2e"
exec docker run --rm --init --platform "$platform" \
    -e OWE_E2E_EMULATED="$emulated" -e OWE_E2E_TIMEOUT_MULTIPLIER -e OWE_E2E_TIMEOUT \
    -v "$root:/src:ro" "$tag"
