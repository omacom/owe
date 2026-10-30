#!/bin/sh
# Entry of the VM host container in test/e2e/run-gpu.sh. It reads the guest
# root file system as a tar stream on stdin, writes it to an ext4 image, and
# boots the image in QEMU with a virtio GPU that draws on the host GPU.
set -eu

render=${OWE_E2E_RENDER_NODE:-/dev/dri/renderD128}
timeout=${OWE_E2E_TIMEOUT:-300}
cpus=${OWE_E2E_VM_CPUS:-4}
hwdec=${OWE_E2E_HWDEC:-}

echo "== Guest disk"
mkdir /rootfs
tar -x -C /rootfs --numeric-owner
cp "$(ls /rootfs/boot/vmlinuz-* | tail -n 1)" /vmlinuz
cp "$(ls /rootfs/boot/initrd.img-* | tail -n 1)" /initrd.img
size=$(($(du -sm /rootfs | cut -f 1) + 512))
mkfs.ext4 -q -L owe-root -d /rootfs /rootfs.ext4 "${size}M"
rm -rf /rootfs

echo "== Boot arm64 virtual machine with a virtio GPU on $render"
qemu-system-aarch64 \
    -machine virt -accel tcg,thread=multi -cpu max,pauth-impdef=on -smp "$cpus" -m 4096 \
    -kernel /vmlinuz -initrd /initrd.img \
    -append "root=/dev/vda rw console=ttyAMA0 quiet loglevel=3 init=/usr/local/sbin/owe-vm-init owe_timeout=$timeout${hwdec:+ owe_hwdec=$hwdec}" \
    -drive if=virtio,format=raw,file=/rootfs.ext4 \
    -virtfs local,path=/src,mount_tag=src,security_model=none,readonly=on \
    -virtfs local,path=/owe-bin,mount_tag=bin,security_model=none,readonly=on \
    -device virtio-gpu-gl-pci -display egl-headless,rendernode="$render" \
    -nodefaults -serial stdio -no-reboot | tee /console.log

result=$(sed -n 's/.*OWE_VM_RESULT=\([0-9]*\).*/\1/p' /console.log | tail -n 1)
if [ -z "$result" ]; then
    echo "The virtual machine stopped before the playback check ended." >&2
    exit 1
fi
exit "$result"
