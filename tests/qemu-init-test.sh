#!/bin/bash
# QEMU boot test of Aether init (the part unit/integration tests cannot cover:
# real devtmpfs/proc/sysfs mounts by PID 1, /dev/console, the real shell).
# Not a unit test; needs qemu-system-x86_64 and a kernel, exits 77 (skip) without them.
#
#   tests/qemu-init-test.sh            # new C init, then the legacy fallback
#   KERNEL=... INITRD=... tests/qemu-init-test.sh
set -uo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL="${KERNEL:-$PROJECT/build/kernel/bzImage}"
INITRD="${INITRD:-$PROJECT/build/aether-initramfs.cpio.gz}"
command -v qemu-system-x86_64 >/dev/null || { echo "SKIP: qemu-system-x86_64 not installed"; exit 77; }
[[ -f "$KERNEL" ]] || { echo "SKIP: no kernel at $KERNEL"; exit 77; }
[[ -f "$INITRD" ]] || { echo "FAIL: no initramfs at $INITRD (run 'make initramfs')"; exit 1; }
export KERNEL INITRD

LOG="$(mktemp /tmp/aether-qemu-init-XXXXXX.log)"
LOG2="$(mktemp /tmp/aether-qemu-legacy-XXXXXX.log)"
trap 'rm -f "$LOG" "$LOG2"' EXIT
fail=0
expect() {   # expect LOGFILE PATTERN
    grep -qF -- "$2" "$1" && echo "PASS: saw '$2'" || { echo "FAIL: missing '$2'"; fail=1; }
}

echo "== new C init (INIT=aether) =="
# Type into the console: inspect mounts, kill the console shell (init must restart it), then power off.
( sleep 15
  echo "grep -E ' /(dev|proc|sys|run) ' /proc/mounts | cut -d' ' -f2,3"; sleep 2
  echo "exit"; sleep 6                       # console shell dies -> restart policy
  echo "echo shell-restarted-ok"; sleep 2
  echo "poweroff"; sleep 20 ) |
    INIT=aether timeout 120 "$PROJECT/tools/run-qemu.sh" >"$LOG" 2>&1
rc=$?
for pat in "Aether: init: mounted /proc (proc)" "Aether: init: mounted /sys (sysfs)" "Aether: init: mounted /run (tmpfs)" \
           "AETHER OS" "Aether: all services started" "/run tmpfs" "/proc proc" "/sys sysfs" \
           "Aether: console exited with status 0." "Aether: restarting console (1/5)." "shell-restarted-ok" \
           "Aether: powering off"; do
    expect "$LOG" "$pat"
done
# /dev may already be mounted by the kernel (CONFIG_DEVTMPFS_MOUNT): either message is correct.
grep -qE "Aether: init: (mounted /dev \(devtmpfs\)|/dev already mounted)" "$LOG" \
    && echo "PASS: /dev handled" || { echo "FAIL: no /dev message"; fail=1; }
grep -q "CRITICAL" "$LOG" && { echo "FAIL: unexpected CRITICAL message"; fail=1; }
[[ $rc -ne 124 ]] && echo "PASS: QEMU exited after poweroff" || { echo "FAIL: timed out"; fail=1; }

echo "== legacy fallback (INIT=legacy) =="
( sleep 15; echo "echo legacy-shell-ok"; sleep 3 ) |
    INIT=legacy timeout 40 "$PROJECT/tools/run-qemu.sh" >"$LOG2" 2>&1 || true   # legacy has no poweroff path: the timeout ends it
for pat in "Core v0.2" "Aether system ready." "legacy-shell-ok"; do
    expect "$LOG2" "$pat"
done

[[ $fail -eq 0 ]] || { echo "---- new init log tail"; tail -n 40 "$LOG"; echo "---- legacy log tail"; tail -n 20 "$LOG2"; }
exit $fail
