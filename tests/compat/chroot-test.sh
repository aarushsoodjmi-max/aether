#!/bin/bash
# Run the ELF-compat suite in a chroot of build/rootfs-compat (fast; needs root).
# Real kernel, real glibc loader — but the HOST kernel, not Aether's. Use qemu-test.sh for that.
set -uo pipefail
P="$(cd "$(dirname "$0")/../.." && pwd)"; R="$P/build/rootfs-compat"
[[ $EUID -eq 0 ]] || { echo "SKIP: needs root"; exit 77; }
[[ -x "$R/opt/aether-compat/run-all.sh" ]] || { echo "build first: make compat-initramfs" >&2; exit 1; }
mkdir -p "$R/proc" "$R/dev" "$R/tmp"
mount -t proc proc "$R/proc" || exit 1
mount --bind /dev "$R/dev" || { umount "$R/proc"; exit 1; }
trap 'umount "$R/dev" "$R/proc" 2>/dev/null' EXIT
env -i PATH=/bin:/sbin HOME=/root chroot "$R" /bin/sh /opt/aether-compat/run-all.sh
