#!/bin/bash
# Build the Linux kernel into build/kernel/bzImage.
#
# The repository does not ship a kernel tree. Provide ONE of:
#   KERNEL_SRC=/path/to/linux-X.Y.Z            an unpacked kernel tree
#   KERNEL_TARBALL=/path/to/linux-X.Y.Z.tar.xz a tarball (checksum is checked)
# For a tarball, its sha256 must be listed in checksums/sources.sha256
# (format: "<sha256>  <file name>") or passed as KERNEL_SHA256=<hash>.
# Out-of-tree build: the kernel source is never modified.
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
OBJ="$PROJECT/build/kernel-obj"
OUT="$PROJECT/build/kernel/bzImage"
JOBS="${JOBS:-$(nproc)}"
export KBUILD_BUILD_TIMESTAMP="${KBUILD_BUILD_TIMESTAMP:-Thu Jan  1 00:00:00 UTC 1970}"
export KBUILD_BUILD_USER=aether KBUILD_BUILD_HOST=aether

if [[ -n "${KERNEL_TARBALL:-}" ]]; then
    base="$(basename "$KERNEL_TARBALL")"
    want="${KERNEL_SHA256:-$(awk -v f="$base" '$2==f{print $1}' "$PROJECT/checksums/sources.sha256")}"
    [[ -n "$want" ]] || { echo "no checksum for $base: add it to checksums/sources.sha256 or set KERNEL_SHA256" >&2; exit 1; }
    got="$(sha256sum "$KERNEL_TARBALL" | cut -d' ' -f1)"
    [[ "$got" == "$want" ]] || { echo "kernel tarball checksum mismatch" >&2; exit 1; }
    mkdir -p "$PROJECT/build/kernel-src"
    tar -C "$PROJECT/build/kernel-src" -xf "$KERNEL_TARBALL"
    KERNEL_SRC="$(find "$PROJECT/build/kernel-src" -mindepth 1 -maxdepth 1 -type d | head -n1)"
fi
[[ -n "${KERNEL_SRC:-}" && -f "$KERNEL_SRC/Makefile" ]] \
    || { echo "set KERNEL_SRC or KERNEL_TARBALL (see header of $0)" >&2; exit 1; }

mkdir -p "$OBJ" "$(dirname "$OUT")" "$PROJECT/build/logs"
LOG="$PROJECT/build/logs/kernel.log"
{
    make -C "$KERNEL_SRC" O="$OBJ" x86_64_defconfig
    "$KERNEL_SRC/scripts/kconfig/merge_config.sh" -m -O "$OBJ" "$OBJ/.config" "$PROJECT/config/kernel.fragment"
    make -C "$KERNEL_SRC" O="$OBJ" olddefconfig
    make -C "$KERNEL_SRC" O="$OBJ" -j"$JOBS" bzImage
} >"$LOG" 2>&1 || { echo "kernel build failed; see $LOG" >&2; tail -n 20 "$LOG" >&2; exit 1; }

install -m 0644 "$OBJ/arch/x86/boot/bzImage" "$OUT"
echo "Kernel built: $OUT"
