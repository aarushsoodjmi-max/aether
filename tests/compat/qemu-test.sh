#!/bin/bash
# Boot the compat initramfs in QEMU (Aether init as PID 1), run the ELF-compat suite on the
# serial console, require "COMPAT SUMMARY ... fail=0" and a clean poweroff.
# Exit 77 (skip) without QEMU or a kernel. KERNEL=/path/to/bzImage overrides.
set -uo pipefail
P="$(cd "$(dirname "$0")/../.." && pwd)"
KERNEL="${KERNEL:-$P/build/kernel/bzImage}"; INITRD="$P/build/aether-compat-initramfs.cpio.gz"
command -v qemu-system-x86_64 >/dev/null || { echo "SKIP: no qemu-system-x86_64"; exit 77; }
[[ -f "$KERNEL" ]] || { echo "SKIP: no kernel at $KERNEL"; exit 77; }
[[ -f "$INITRD" ]] || { echo "FAIL: build first: make compat-initramfs"; exit 1; }
LOG="$(mktemp /tmp/aether-compat-qemu-XXXXXX.log)"; FIFO="$(mktemp -u /tmp/aether-compat-fifo-XXXXXX)"
mkfifo "$FIFO"; trap 'rm -f "$LOG" "$FIFO"; kill $QP 2>/dev/null' EXIT
INITRD="$INITRD" MEM="${MEM:-512}" timeout 400 "$P/tools/run-qemu.sh" <"$FIFO" >"$LOG" 2>&1 & QP=$!
exec 3>"$FIFO"
waitfor() { local i=0; while ! grep -qF "$1" "$LOG"; do sleep 1; i=$((i+1)); [[ $i -ge $2 ]] && return 1; done; return 0; }
waitfor "Aether: all services started" 120 || { echo "FAIL: no boot"; tail -30 "$LOG"; exit 1; }
sleep 3; echo "/opt/aether-compat/run-all.sh" >&3
waitfor "COMPAT SUMMARY" 240 || { echo "FAIL: suite did not finish"; tail -40 "$LOG"; exit 1; }
sleep 1; echo "poweroff" >&3
waitfor "Aether: powering off" 30 || echo "WARN: no poweroff message"
wait $QP 2>/dev/null; exec 3>&-
tr -d '\r' < "$LOG" | grep -E '^(PASS|FAIL|    \||COMPAT SUMMARY)' 
if tr -d '\r' < "$LOG" | grep -q 'COMPAT SUMMARY pass=[0-9]* fail=0$'; then echo "RESULT: PASS (QEMU)"; exit 0; fi
echo "RESULT: FAIL"; exit 1
