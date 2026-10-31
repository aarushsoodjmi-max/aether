#!/bin/bash
# End-to-end boot test: boot the kernel + initramfs in QEMU, wait for the
# Aether banner and service start, then type "poweroff" on the console and
# require a clean shutdown. Exits 77 (skip) when QEMU or a kernel is missing.
set -uo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL="${KERNEL:-$PROJECT/build/kernel/bzImage}"
INITRD="${INITRD:-$PROJECT/build/aether-initramfs.cpio.gz}"
command -v qemu-system-x86_64 >/dev/null || { echo "SKIP: qemu-system-x86_64 not installed"; exit 77; }
[[ -f "$KERNEL" ]] || { echo "SKIP: no kernel at $KERNEL"; exit 77; }
[[ -f "$INITRD" ]] || { echo "FAIL: no initramfs at $INITRD"; exit 1; }

LOG="$(mktemp /tmp/aether-qemu-XXXXXX.log)"
trap 'rm -f "$LOG"' EXIT
( sleep 15; echo "service-status heartbeat"; sleep 2; echo "poweroff"; sleep 20 ) |
    timeout 90 "$PROJECT/tools/run-qemu.sh" >"$LOG" 2>&1
rc=$?
fail=0
for pat in "AETHER OS" "Aether: started heartbeat" "Aether: all services started" \
           "heartbeat: running" "Aether: powering off"; do
    grep -qF "$pat" "$LOG" && echo "PASS: saw '$pat'" || { echo "FAIL: missing '$pat'"; fail=1; }
done
[[ $rc -ne 124 ]] && echo "PASS: QEMU exited after poweroff" || { echo "FAIL: timed out"; fail=1; }
[[ $fail -eq 0 ]] || tail -n 40 "$LOG"
exit $fail
