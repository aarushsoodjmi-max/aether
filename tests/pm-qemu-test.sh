#!/bin/bash
# Boot the compat image in QEMU and drive aether-pm on the running Aether system (signed on-image repo).
# Exit 77 (skip) without QEMU or a kernel.
set -uo pipefail
P="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL="${KERNEL:-$P/build/kernel/bzImage}"; INITRD="$P/build/aether-compat-initramfs.cpio.gz"
command -v qemu-system-x86_64 >/dev/null || { echo "SKIP: no qemu-system-x86_64"; exit 77; }
[[ -f "$KERNEL" ]] || { echo "SKIP: no kernel at $KERNEL"; exit 77; }
[[ -f "$INITRD" ]] || { echo "FAIL: build first: make compat-initramfs"; exit 1; }
LOG="$(mktemp /tmp/aether-pm-qemu-XXXXXX.log)"; FIFO="$(mktemp -u /tmp/aether-pm-fifo-XXXXXX)"
mkfifo "$FIFO"; trap 'rm -f "$LOG" "$FIFO"; kill $QP 2>/dev/null' EXIT
INITRD="$INITRD" MEM="${MEM:-512}" timeout 300 "$P/tools/run-qemu.sh" <"$FIFO" >"$LOG" 2>&1 & QP=$!
exec 3>"$FIFO"
waitfor() { local i=0; while ! grep -qF "$1" "$LOG"; do sleep 1; i=$((i+1)); [[ $i -ge $2 ]] && return 1; done; return 0; }
waitfor "Aether: all services started" 120 || { echo "FAIL: no boot"; tail -20 "$LOG"; exit 1; }
sleep 3
# Tags are assembled with "" so the console's echo of the typed command never matches the expected output.
send() { printf '%s\n' "$1" >&3; sleep 0.3; }
send 'aether-pm list | grep -c . | sed "s/^/PM_""INSTALLED=/"'
send 'aether-pm check | tail -1 | sed "s/^/PM_""CHECK0=/"'
send 'aether-pm update && echo PM_""UPDATE_OK'
send 'aether-pm install aether-hello=1.0-1 && echo PM_""INSTALL_OK'
send 'aether-hello | sed "s/^/PM_""RUN=/"'
send 'aether-pm upgrade && echo PM_""UPGRADE_OK'
send 'aether-hello | sed "s/^/PM_""RUN2=/"'
send 'aether-pm check | tail -1 | sed "s/^/PM_""CHECK1=/"'
send 'aether-pm owner /usr/bin/aether-hello | sed "s/^/PM_""OWNER=/"'
send 'aether-pm remove aether-hello && aether-pm remove aether-hello-data && echo PM_""REMOVE_OK'
send 'test -e /usr/bin/aether-hello || echo PM_""GONE'
send 'for f in /var/lib/aether/repo /var/cache/aether/pm/aether/pkgs; do busybox dd if=/dev/zero of=$f/aether-hello_1.1-1_x86_64.aether bs=1 count=1 seek=150 conv=notrunc 2>/dev/null; done'
send 'aether-pm install aether-hello 2>&1 | grep -c "signed index" | sed "s/^/PM_""TAMPER=/"'
send 'test -e /usr/bin/aether-hello || echo PM_""NOTHING_INSTALLED'
send 'aether-pm check | tail -1 | sed "s/^/PM_""CHECK2=/"'
send 'echo PM_""DONE'
waitfor "PM_DONE" 240 || { echo "FAIL: did not finish"; tail -40 "$LOG"; exit 1; }
sleep 1; echo "poweroff" >&3; waitfor "Aether: powering off" 30; wait $QP 2>/dev/null; exec 3>&-
L="$(tr -d '\r' < "$LOG")"; pass=0; fail=0
chk() { if grep -q "^$2" <<<"$L"; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1 (expected line starting '$2')"; fail=$((fail+1)); fi; }
chk "boot: compat packages present in the image database" "PM_INSTALLED=6"
chk "check: image files verify (0 problems)" "PM_CHECK0=.* 0 problems"
chk "update: signed on-image index verified" "PM_UPDATE_OK"
chk "install: aether-hello 1.0 installed" "PM_INSTALL_OK"
chk "run: installed program executes" "PM_RUN=hello from package 1.0"
chk "upgrade: resolves new dependency and upgrades" "PM_UPGRADE_OK"
chk "run: upgraded program executes" "PM_RUN2=hello from package 1.1"
chk "check: system consistent after upgrade" "PM_CHECK1=.* 0 problems"
chk "owner: file ownership answered" "PM_OWNER=.*owned by aether-hello 1.1-1"
chk "remove: dependent then dependency" "PM_REMOVE_OK"
chk "remove: files gone" "PM_GONE"
chk "tamper: corrupted package rejected against signed index" "PM_TAMPER=1"
chk "tamper: nothing installed" "PM_NOTHING_INSTALLED"
chk "check: system still consistent after rejected install" "PM_CHECK2=.* 0 problems"
echo "PM-QEMU SUMMARY pass=$pass fail=$fail"; [[ $fail -eq 0 ]]
