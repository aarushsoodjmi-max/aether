#!/bin/bash
# Stage the kernel modules the GUI session may load into build/gui-modules/*.ko (decompressed).
# Usage: MODDIR=/path/to/lib/modules/<kernel-version> [ZSTD=zstd] tools/stage-gui-modules.sh
# The modules MUST come from the exact kernel that boots (vermagic is checked by the kernel).
# Modules: psmouse (PS/2 mouse; Ubuntu builds it as a module, so without it there is no pointer),
#          hid + usbhid + hid-generic (USB mice/keyboards and QEMU's `usb-tablet`; the USB host controllers themselves are built in),
#          virtio_input (QEMU `virtio-tablet-pci` / `virtio-mouse-pci` / `virtio-keyboard-pci`),
#          bochs (QEMU std VGA), virtio-gpu + virtio_dma_buf (QEMU virtio-vga). No i915: it needs firmware + many deps (not done).
set -euo pipefail
P="$(cd "$(dirname "$0")/.." && pwd)"; OUT="$P/build/gui-modules"; ZSTD="${ZSTD:-zstd}"
[[ -d "${MODDIR:-}" ]] || { echo "set MODDIR to the kernel's lib/modules/<version> directory" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
for rel in kernel/drivers/input/mouse/psmouse kernel/drivers/hid/hid kernel/drivers/hid/usbhid/usbhid kernel/drivers/hid/hid-generic kernel/drivers/virtio/virtio_input kernel/drivers/gpu/drm/tiny/bochs kernel/drivers/gpu/drm/virtio/virtio-gpu kernel/drivers/virtio/virtio_dma_buf; do
    src="$MODDIR/$rel.ko.zst"; [[ -f "$src" ]] || { echo "missing $src" >&2; exit 1; }
    "$ZSTD" -d -q -o "$OUT/$(basename "$rel").ko" "$src"
done
ls -la "$OUT"
