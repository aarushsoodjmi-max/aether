#!/bin/bash
# Boot the Aether desktop in a QEMU window with a mouse that follows your real pointer.
#   POINTER=tablet (default; absolute usb-tablet: no grab, the guest pointer is exactly where your pointer is)
#           ps2    (relative PS/2 mouse: click the window to grab, Ctrl+Alt+G to release)
#           virtio (absolute virtio-tablet)
#   DISPLAY_BACKEND=gtk|sdl|vnc=:0 (default gtk)   MEM=512   KERNEL=...   INITRD=...
set -euo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL="${KERNEL:-$PROJECT/build/kernel/bzImage}"
INITRD="${INITRD:-$PROJECT/build/aether-gui-initramfs.cpio.gz}"
QEMU="${QEMU:-$(command -v qemu-system-x86_64 || echo "$PROJECT/build/hosttools/bin/qemu-system-x86_64")}"
[[ -x "$QEMU" ]] || { echo "qemu-system-x86_64 not found (tools/setup-test-env.sh installs a project-local one)" >&2; exit 1; }
[[ -f "$KERNEL" && -f "$INITRD" ]] || { echo "need $KERNEL and $INITRD (make gui-initramfs)" >&2; exit 1; }
ptr=()
case "${POINTER:-tablet}" in
    tablet) ptr=(-usb -device usb-tablet) ;;
    virtio) ptr=(-device virtio-tablet-pci) ;;
    ps2) ptr=() ;;
    *) echo "POINTER must be tablet, virtio or ps2" >&2; exit 1 ;;
esac
accel=(); [[ -r /dev/kvm && -w /dev/kvm ]] && accel=(-enable-kvm -cpu host)
exec "$QEMU" "${accel[@]}" -m "${MEM:-512}" -vga std -display "${DISPLAY_BACKEND:-gtk}" "${ptr[@]}" -serial stdio -no-reboot \
    -kernel "$KERNEL" -initrd "$INITRD" -append "console=ttyS0 rdinit=/init panic=1 ${EXTRA:-}"
