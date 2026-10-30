#!/bin/sh
# PID 1 of the virtual machine that test/e2e/run-gpu.sh boots. It mounts the
# source tree and the arm64 binaries from the host, runs the playback check on
# the virtio GPU, prints the result for the host and powers off.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

mountpoint -q /proc || mount -t proc proc /proc
mountpoint -q /sys || mount -t sysfs sysfs /sys
mountpoint -q /dev || mount -t devtmpfs devtmpfs /dev
mountpoint -q /run || mount -t tmpfs tmpfs /run
mkdir -p /dev/pts /dev/shm
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /dev/shm
mount -t tmpfs tmpfs /tmp

modprobe -a virtio_gpu 9pnet_virtio 9p
mkdir -p /src /opt/owe
mount -t 9p -o trans=virtio,version=9p2000.L,ro src /src
mount -t 9p -o trans=virtio,version=9p2000.L,ro bin /opt/owe

# Without udev, devtmpfs creates the DRM nodes for root only.
i=0
while [ ! -e /dev/dri/renderD128 ] && [ "$i" -lt 100 ]; do
    sleep 0.1
    i=$((i + 1))
done
chmod 0666 /dev/dri/*

timeout=$(sed -n 's/.*owe_timeout=\([0-9]*\).*/\1/p' /proc/cmdline)
hwdec=$(sed -n 's/.*owe_hwdec=\([a-z0-9-]*\).*/\1/p' /proc/cmdline)
if [ -n "$hwdec" ]; then
    echo "== VA-API in the guest"
    setpriv --reuid=1000 --regid=1000 --init-groups \
        env HOME=/home/owe vainfo --display drm --device /dev/dri/renderD128 2>&1
fi
status=0
for api in opengl gles; do
    echo "== Playback with $api on the virtio GPU, hwdec ${hwdec:-default}"
    setpriv --reuid=1000 --regid=1000 --init-groups \
        env -i HOME=/home/owe PATH="$PATH" OWE_E2E_TIMEOUT="${timeout:-300}" \
        python3 -u /src/test/e2e/e2e.py /opt/owe --api "$api" --gpu \
        ${hwdec:+--hwdec "$hwdec"} || status=1
done
echo "OWE_VM_RESULT=$status"

sync
echo o > /proc/sysrq-trigger
# PID 1 must not exit, or the kernel panics before the power off ends.
while :; do sleep 60; done
