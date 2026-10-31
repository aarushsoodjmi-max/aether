#!/bin/bash
# Build the pinned BusyBox release out-of-tree into build/busybox-obj and
# install the resulting static binary at build/bin/busybox.
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$PROJECT/src/packages/busybox-1.37.0"
TARBALL="$PROJECT/src/packages/busybox-1.37.0.tar.bz2"
OBJ="$PROJECT/build/busybox-obj"
OUT="$PROJECT/build/bin/busybox"
JOBS="${JOBS:-$(nproc)}"

( cd "$PROJECT/src/packages" && sha256sum -c "$PROJECT/checksums/sources.sha256" >/dev/null ) \
    || { echo "BusyBox tarball checksum mismatch" >&2; exit 1; }

# Re-extract from the verified tarball if the source tree is missing.
if [[ ! -f "$SRC/Makefile" ]]; then
    tar -C "$PROJECT/src/packages" -xjf "$TARBALL"
fi

mkdir -p "$OBJ" "$PROJECT/build/logs" "$(dirname "$OUT")"
cp "$PROJECT/config/busybox.config" "$OBJ/.config"

LOG="$PROJECT/build/logs/busybox.log"
if ! { make -C "$SRC" O="$OBJ" oldconfig </dev/null &&
       make -C "$SRC" O="$OBJ" -j"$JOBS" busybox; } >"$LOG" 2>&1; then
    echo "BusyBox build failed; see $LOG" >&2
    tail -n 20 "$LOG" >&2
    exit 1
fi

install -m 0755 "$OBJ/busybox" "$OUT"
echo "BusyBox built: $OUT"
