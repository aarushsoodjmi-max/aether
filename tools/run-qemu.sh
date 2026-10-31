#!/bin/bash
# Boot Aether in QEMU (serial console on this terminal; Ctrl-A X quits).
#   KERNEL=build/kernel/bzImage  INITRD=build/aether-initramfs.cpio.gz
#   MEM=256  EXTRA="extra kernel args"
set -euo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL="${KERNEL:-$PROJECT/build/kernel/bzImage}"
INITRD="${INITRD:-$PROJECT/build/aether-initramfs.cpio.gz}"

command -v qemu-system-x86_64 >/dev/null || { echo "qemu-system-x86_64 not found" >&2; exit 1; }
[[ -f "$KERNEL" ]] || { echo "kernel not found: $KERNEL (run 'make kernel' or set KERNEL=)" >&2; exit 1; }
[[ -f "$INITRD" ]] || { echo "initramfs not found: $INITRD (run 'make initramfs')" >&2; exit 1; }

accel=()
[[ -r /dev/kvm && -w /dev/kvm ]] && accel=(-enable-kvm -cpu host)

exec qemu-system-x86_64 "${accel[@]}" -m "${MEM:-256}" -nographic -no-reboot \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -append "console=ttyS0 rdinit=/init panic=1 ${EXTRA:-}"
