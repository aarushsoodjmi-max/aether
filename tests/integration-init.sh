#!/bin/bash
# Integration tests for the real Aether init, supervisor and tooling.
#
# Runs the *test build* of aether-init (-DAETHER_TEST: no mounts, reboot(2)
# replaced by a message) as PID 1 of a fresh PID+mount namespace chrooted
# into a generated rootfs, so nothing on the host can be affected.
# Requires root (unshare/chroot/mknod). Skips with exit 77 otherwise.
set -uo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$PROJECT/build/bin"
INIT_TEST="$PROJECT/build/test/aether-init-test"
BUSYBOX="${BUSYBOX:-$BIN/busybox}"

[[ $EUID -eq 0 ]] || { echo "SKIP: needs root for unshare/chroot"; exit 77; }
unshare --pid --fork --mount true 2>/dev/null || { echo "SKIP: no namespace support"; exit 77; }
[[ -x "$BUSYBOX" ]] || { echo "SKIP: no busybox ($BUSYBOX); run 'make busybox'"; exit 77; }

# The test rootfs needs working device nodes (/dev/null, /dev/console) and executables. /tmp is often
# mounted nodev and/or noexec, which makes every non-console service die with status 126. Use the first
# candidate directory where a probe device node can be opened and a binary can run.
# Override with AETHER_TEST_TMP=/some/dir.
pick_tmp() {
    local base d
    for base in "${AETHER_TEST_TMP:-}" "${TMPDIR:-}" /tmp "$PROJECT/build" /var/tmp; do
        [[ -n $base && -d $base && -w $base ]] || continue
        d="$(mktemp -d "$base/aether-it-XXXXXX" 2>/dev/null)" || continue
        if mknod -m 666 "$d/probe" c 1 3 2>/dev/null && { : < "$d/probe"; } 2>/dev/null \
           && cp "$BUSYBOX" "$d/busybox" 2>/dev/null && "$d/busybox" true 2>/dev/null; then
            echo "$d"; return 0
        fi
        rm -rf -- "$d"
    done
    return 1
}
TMP="$(pick_tmp)" || { echo "SKIP: no writable directory allows device nodes and executables (nodev/noexec?); set AETHER_TEST_TMP"; exit 77; }
trap 'rm -rf -- "$TMP"' EXIT
fail=0
ok()  { echo "PASS: $1"; }
bad() { echo "FAIL: $1"; fail=1; }

mkdir -p "$TMP/base"
printf 'NAME=console\nEXEC=/bin/sh\n' > "$TMP/base/console.conf"

# build_root DIR : assemble a rootfs with the test init and test helpers
build_root() {
    ROOT="$1" SERVICES_DIR="$TMP/base" INIT_BIN="$INIT_TEST" BUSYBOX="$BUSYBOX" \
        NO_IMAGE=1 "$PROJECT/tools/build-rootfs.sh" >/dev/null || return 1
    mknod -m 666 "$1/dev/null" c 1 3
    mknod -m 600 "$1/dev/console" c 5 1
    local b="$1/bin"
    cat > "$b/console-poweroff" <<'S'
#!/bin/sh
echo console-up
/bin/service-status heartbeat
/bin/service-status nosuchservice
/bin/aetherctl check /etc/aether/services
sleep "${WAIT:-1}"
/bin/aetherctl poweroff
sleep 30
S
    sed 's/poweroff$/reboot/' "$b/console-poweroff" > "$b/console-reboot"
    cat > "$b/console-ctl" <<'S'
#!/bin/sh
sleep 1
echo "T:list"; /bin/aetherctl list
echo "T:status"; /bin/aetherctl status termlog
echo "T:stop"; /bin/aetherctl stop termlog
echo "T:status-stopped"; /bin/aetherctl status termlog
echo "T:stop-again"; /bin/aetherctl stop termlog 2>&1; echo "RC=$?"
sleep 2
echo "T:status-after-wait"; /bin/aetherctl status termlog
echo "T:start"; /bin/aetherctl start termlog
echo "T:start-again"; /bin/aetherctl start termlog 2>&1
echo "T:restart"; /bin/aetherctl restart termlog
echo "T:unknown"; /bin/aetherctl start nosuch 2>&1; echo "RC=$?"
echo "T:console"; /bin/aetherctl stop console 2>&1
echo "T:badcmd"; /bin/aetherctl status 2>&1; echo "RC=$?"
echo "T:done"
/bin/aetherctl poweroff
sleep 30
S
    cat > "$b/console-mnt" <<'S'
#!/bin/sh
echo hello > /mnt/data/f && echo "WROTE-DATA"
grep ' /mnt/data ' /proc/mounts | grep -q nosuid && echo "OPTS-OK"
echo x > /mnt/ro/f 2>&1
cat /proc/mounts | grep -c '/mnt/' | sed 's/^/MOUNTS=/'
/bin/aetherctl poweroff
sleep 30
S
    printf '#!/bin/sh\nsleep 1\nkill -TERM 1\nsleep 30\n' > "$b/console-term"
    printf '#!/bin/sh\nsleep 6\n/bin/aetherctl poweroff\nsleep 30\n' > "$b/console-slow"
    printf '#!/bin/sh\necho crasher-run\nexit 1\n' > "$b/crasher"
    printf '#!/bin/sh\ntrap "" TERM\necho stubborn-up\nwhile :; do sleep 1; done\n' > "$b/stubborn"
    printf '#!/bin/sh\ntrap '"'"'echo "$0-got-term"; exit 0'"'"' TERM\necho "$0-up"\nwhile :; do sleep 1 & wait $!; done\n' > "$b/termlog"
    chmod +x "$b"/console-* "$b"/crasher "$b"/stubborn "$b"/termlog
}

# svc ROOT NAME EXEC [extra lines...]
svc() {
    local root="$1" name="$2" exe="$3"; shift 3
    { printf 'NAME=%s\nEXEC=%s\n' "$name" "$exe"; for l in "$@"; do echo "$l"; done; } \
        > "$root/etc/aether/services/$name.conf"
}

# run ROOT : boot init in a PID namespace; sets RC, OUT, SECS
run() {
    local root="$1" start end
    start=$(date +%s)
    : > "$root/var/log/aether.log"
    timeout 40 unshare --pid --fork --kill-child --mount --uts --net \
        --mount-proc="$root/proc" chroot "$root" /init >"$root/out.txt" 2>&1 </dev/null
    RC=$?
    end=$(date +%s); SECS=$((end - start))
    OUT="$(cat "$root/out.txt")"
    LOG="$(cat "$root/var/log/aether.log" 2>/dev/null || true)"
}
has()  { grep -qF -- "$2" <<<"$1"; }
count(){ grep -cF -- "$2" <<<"$1" || true; }
reset_services() { rm -f "$1"/etc/aether/services/*.conf; }

R="$TMP/r"; build_root "$R" || { echo "FAIL: rootfs build"; exit 1; }

# ---- 1. clean boot, dependency order, status, check, clean shutdown ----
reset_services "$R"
svc "$R" console /bin/console-poweroff
svc "$R" heartbeat /bin/aether-heartbeat
svc "$R" termlog /bin/termlog
svc "$R" app /bin/termlog REQUIRES=termlog,heartbeat
echo 'garbage' > "$R/etc/aether/services/broken.conf"
run "$R"
[[ $RC -eq 0 ]] && has "$OUT" "AETHER-TEST: poweroff" && ok "poweroff via aetherctl -> init -> poweroff" || { bad "poweroff via aetherctl (rc=$RC)"; echo "$OUT" | tail -20; }
has "$OUT" "Core v0.9" && ok "banner printed" || bad "banner"
has "$OUT" "1 service file(s) rejected" && ok "invalid service file rejected, boot continues" || bad "invalid file handling"
t=$(grep -n 'started termlog' <<<"$OUT" | head -1 | cut -d: -f1); a=$(grep -n 'started app' <<<"$OUT" | head -1 | cut -d: -f1)
c=$(grep -n 'started console' <<<"$OUT" | head -1 | cut -d: -f1)
[[ -n $t && -n $a && -n $c && $t -lt $a && $a -lt $c ]] && ok "start order: dependency before dependent, console last" || bad "start order ($t/$a/$c)"
has "$OUT" "heartbeat: running" && ok "service-status: running service" || bad "service-status running"
has "$OUT" "nosuchservice: not running" && ok "service-status: unknown service" || bad "service-status unknown"
has "$OUT" "check: 4 service(s) valid" && ok "aetherctl check inside the system" || bad "aetherctl check"
has "$LOG" "/bin/termlog-got-term" && ok "services receive SIGTERM at shutdown" || bad "SIGTERM delivery"
[[ $(count "$LOG" "-got-term") -eq 2 ]] && ok "both dependent and dependency stopped" || bad "stop count"
compgen -G "$R/run/aether-*.pid" >/dev/null && bad "pid files removed at shutdown" || ok "pid files removed at shutdown"

# ---- 2. reboot ----
reset_services "$R"; svc "$R" console /bin/console-reboot
run "$R"
[[ $RC -eq 3 ]] && has "$OUT" "AETHER-TEST: reboot" && ok "reboot path (aetherctl reboot)" || { bad "reboot (rc=$RC)"; echo "$OUT" | tail; }

# ---- 2b. runtime service control over the control socket ----
reset_services "$R"; svc "$R" console /bin/console-ctl; svc "$R" termlog /bin/termlog
run "$R"
has "$OUT" "termlog running pid=" && has "$OUT" "console running pid=" && ok "aetherctl list / status" || { bad "list/status"; echo "$OUT" | tail -30; }
has "$OUT" "termlog stopped pid=0" && ok "aetherctl stop" || bad "stop"
has "$OUT" "error: termlog is not running" && ok "stop of stopped service is an error" || bad "stop twice"
sec="$(sed -n '/T:status-after-wait/,/T:start$/p' <<<"$OUT")"
[[ "$sec" == *"termlog stopped"* ]] && ok "manually stopped service is not auto-restarted" || bad "manual stop restarted"
has "$OUT" "error: termlog is already running" && ok "start of running service is an error" || bad "start twice"
has "$OUT" "error: unknown service nosuch" && ok "unknown service reported" || bad "unknown service"
has "$OUT" "error: refusing to stop the console service" && ok "console cannot be stopped" || bad "console stop"
[[ $(grep -c '^RC=1$' <<<"$OUT") -eq 2 && $(grep -c '^RC=2$' <<<"$OUT") -eq 1 ]] \
    && ok "errors exit 1, usage errors exit 2" || bad "error exit codes"
[[ $(count "$LOG" "-up") -eq 3 && $(count "$LOG" "-got-term") -eq 3 ]] && ok "start/stop/restart really run and signal the service" \
    || bad "service lifecycle (up=$(count "$LOG" -up) term=$(count "$LOG" -got-term))"
[[ $RC -eq 0 ]] && has "$OUT" "AETHER-TEST: poweroff" && ok "clean poweroff after control session" || bad "poweroff after control"

# ---- 2c. fstab: mount at boot, options, failures tolerated, unmount at shutdown ----
reset_services "$R"; svc "$R" console /bin/console-mnt
cp "$R/etc/aether/fstab" "$TMP/fstab.orig"
cat > "$R/etc/aether/fstab" <<'F'
# comment
tmpfs /mnt/data tmpfs nosuid,nodev,size=1m
tmpfs /mnt/ro tmpfs ro
nosuchfs /mnt/bad nosuchfs
tmpfs relative tmpfs
too many fields here x y
F
run "$R"
echo "$OUT" > /tmp/mnt.out; has "$OUT" "Aether: mounted /mnt/data" && has "$OUT" "Aether: mounted /mnt/ro" && ! has "$OUT" "mounted /rel" && ok "fstab entries mounted (dirs auto-created)" || { bad "fstab mount"; echo "$OUT" | tail -15; }
has "$OUT" "WROTE-DATA" && has "$OUT" "OPTS-OK" && ok "mount works and options applied" || bad "mount options"
has "$OUT" "Read-only file system" && ok "ro option enforced" || bad "ro option"
has "$OUT" "mount nosuchfs on /mnt/bad failed" && ok "failed mount reported, boot continues" || bad "failed mount"
[[ $(grep -c 'fstab line .* invalid; skipped' <<<"$OUT") -eq 2 ]] && ok "invalid fstab lines skipped" || bad "invalid fstab lines"
um1=$(grep -n 'unmounted /mnt/ro' <<<"$OUT" | cut -d: -f1); um2=$(grep -n 'unmounted /mnt/data' <<<"$OUT" | cut -d: -f1)
[[ -n $um1 && -n $um2 && $um1 -lt $um2 ]] && ok "unmounted in reverse order at shutdown" || bad "unmount order ($um1/$um2)"
[[ $RC -eq 0 ]] && ok "clean poweroff with mounts" || bad "poweroff with mounts"
cp "$TMP/fstab.orig" "$R/etc/aether/fstab"

# ---- 2d. network service (isolated network namespace) ----
reset_services "$R"; svc "$R" console /bin/console-poweroff
svc "$R" network /bin/aether-net-up RESTART=never
mkdir -p "$R/etc/aether/network"; rm -f "$R"/etc/aether/network/*
run "$R"
has "$LOG" "lo: up 127.0.0.1/8" && has "$OUT" "network exited with status 0" && ok "network service brings up loopback" || { bad "network service"; echo "$LOG"; }

# ---- 2e. core configuration (/etc/aether/{system,boot,services,logging}.conf) ----
CFGD="$R/etc/aether"
printf '#!/bin/sh\necho "HOST=$(hostname)"\n/bin/aetherctl poweroff\nsleep 30\n' > "$R/bin/console-host"
chmod 755 "$R/bin/console-host"
cfg_reset() { rm -rf "$CFGD"/{system,boot,services,logging}.conf; }
reset_services "$R"; svc "$R" console /bin/console-host

# defaults (files written by aether-config init): same behaviour as before configuration existed
run "$R"
[[ $RC -eq 0 ]] && has "$OUT" "HOST=aether" && has "$OUT" "Hostname: aether" && grep -qxF -- "         AETHER OS" <<<"$OUT" \
    && has "$OUT" "Aether: mounted /tmp" && ! has "$OUT" ".conf:" \
    && ok "config: default files -> hostname aether, unchanged banner, fstab mounted, no complaints" \
    || { bad "config defaults (rc=$RC)"; echo "$OUT" | head -20; }

# no config files at all
cfg_reset; run "$R"
[[ $RC -eq 0 ]] && has "$OUT" "HOST=aether" && grep -qxF -- "         AETHER OS" <<<"$OUT" && ! has "$OUT" ".conf:" \
    && ok "config: no files -> built-in defaults, silent, boots" || { bad "config missing files (rc=$RC)"; echo "$OUT" | head -20; }

# custom identity
printf 'HOSTNAME=lab-7\nSYSTEM_NAME=Aether Lab\n' > "$CFGD/system.conf"
run "$R"
[[ $RC -eq 0 ]] && has "$OUT" "HOST=lab-7" && has "$OUT" "Hostname: lab-7" && grep -qxF -- "         AETHER LAB" <<<"$OUT" \
    && ok "config: HOSTNAME applied to the kernel, SYSTEM_NAME shown in the banner" || { bad "config identity (rc=$RC)"; echo "$OUT" | head -20; }

# boot behaviour
cfg_reset; printf 'BANNER=no\nMOUNT_FSTAB=no\n' > "$CFGD/boot.conf"
run "$R"
[[ $RC -eq 0 ]] && ! has "$OUT" "Core v0.9" && ! has "$OUT" "Aether: mounted" && has "$OUT" "Aether: all services started" \
    && ok "config: BANNER=no and MOUNT_FSTAB=no honoured, boot still completes" || { bad "config boot.conf (rc=$RC)"; echo "$OUT" | head -20; }

# invalid values: reported, defaults kept for those settings, valid ones still used, boot unaffected
cfg_reset; printf 'HOSTNAME=-bad-\nSYSTEM_NAME=Still Valid\nMODE=x\n' > "$CFGD/system.conf"
printf 'BANNER=maybe\n' > "$CFGD/boot.conf"; printf 'LEVEL=loud\n' > "$CFGD/logging.conf"
run "$R"
[[ $RC -eq 0 ]] && has "$OUT" "HOST=aether" && has "$OUT" "Aether: system.conf:1: HOSTNAME" && has "$OUT" "Aether: boot.conf:1: BANNER" \
    && has "$OUT" "Aether: logging.conf:1: LEVEL" && has "$OUT" "unknown setting 'MODE'" && has "$OUT" "STILL VALID" \
    && ok "config: invalid values reported on the console; defaults kept; boot completes" || { bad "config invalid values (rc=$RC)"; echo "$OUT" | head -25; }

# unusable files: a directory, a binary file, an oversized file, a FIFO
cfg_reset; mkdir "$CFGD/system.conf"; printf 'BANNER=no\0\n' > "$CFGD/boot.conf"
head -c 70000 /dev/zero | tr '\0' '#' > "$CFGD/services.conf"; mkfifo "$CFGD/logging.conf"
run "$R"
[[ $RC -eq 0 && $SECS -lt 30 ]] && has "$OUT" "HOST=aether" && has "$OUT" "system.conf: not a regular file" && has "$OUT" "boot.conf: file contains a NUL byte" \
    && has "$OUT" "services.conf: file too large" && has "$OUT" "logging.conf: not a regular file" && has "$OUT" "Core v0.9" \
    && ok "config: directory / binary / oversized / FIFO config files -> defaults, no hang, boot completes" || { bad "config unusable files (rc=$RC)"; echo "$OUT" | head -25; }

# the user's files are never rewritten by booting
cfg_reset; printf 'HOSTNAME=keepme\n# user note\n' > "$CFGD/system.conf"; cp "$CFGD/system.conf" "$TMP/system.conf.before"
run "$R"
cmp -s "$CFGD/system.conf" "$TMP/system.conf.before" && ok "config: boot leaves the user's configuration untouched" || bad "config rewritten by boot"

# restore the default configuration for the remaining sections
cfg_reset; rm -rf "$CFGD/system.conf"; "$BIN/aether-config" init "$CFGD" >/dev/null

# ---- 3. direct SIGTERM to PID 1 ----
reset_services "$R"; svc "$R" console /bin/console-term; svc "$R" termlog /bin/termlog
run "$R"
[[ $RC -eq 0 ]] && has "$OUT" "AETHER-TEST: poweroff" && has "$LOG" "-got-term" && ok "SIGTERM to init powers off cleanly" || bad "SIGTERM to init (rc=$RC)"

# ---- 4. restart policy and limit ----
reset_services "$R"; svc "$R" console /bin/console-slow
svc "$R" crasher /bin/crasher RESTART=always MAX_RESTARTS=2
svc "$R" oneshot /bin/crasher RESTART=never
run "$R"
has "$OUT" "restarting crasher (1/2)" && has "$OUT" "restarting crasher (2/2)" && ok "restart counter" || bad "restart counter"
has "$OUT" "crasher stopped; restart limit reached" && ok "restart limit enforced" || bad "restart limit"
[[ $(count "$LOG" "crasher-run") -eq 4 ]] && ok "crasher ran initial + 2 restarts (3x) plus oneshot once" \
    || bad "run count $(count "$LOG" crasher-run)"
has "$OUT" "oneshot stopped; restart limit reached" && ! has "$OUT" "restarting oneshot" && ok "RESTART=never honoured" || bad "RESTART=never"

# ---- 5. shutdown escalates to SIGKILL for stubborn services ----
reset_services "$R"; svc "$R" console /bin/console-poweroff; svc "$R" stubborn /bin/stubborn
run "$R"
[[ $RC -eq 0 ]] && has "$OUT" "stubborn ignored SIGTERM; sending SIGKILL" && [[ $SECS -lt 15 ]] \
    && ok "stubborn service killed after grace period (${SECS}s)" || bad "SIGKILL escalation (rc=$RC, ${SECS}s)"

# ---- 6. missing console -> recovery shell, other services still run ----
reset_services "$R"; svc "$R" termlog /bin/termlog
run "$R"
has "$OUT" "no valid console service" && has "$OUT" "started recovery" && has "$OUT" "recovery shell exited" \
    && [[ $RC -eq 0 ]] && has "$LOG" "-got-term" && ok "no console config -> recovery shell, then clean shutdown" \
    || { bad "missing console (rc=$RC)"; echo "$OUT" | tail; }

# ---- 7. dependency cycle -> recovery only ----
reset_services "$R"; svc "$R" console /bin/console-poweroff
svc "$R" alpha /bin/termlog REQUIRES=beta; svc "$R" beta /bin/termlog REQUIRES=alpha
run "$R"
has "$OUT" "invalid service dependency graph" && has "$OUT" "started recovery" && ! has "$OUT" "started alpha" \
    && ok "dependency cycle -> recovery shell, cyclic services not started" || bad "cycle handling"

# ---- 8. console crash loop -> recovery shell ----
reset_services "$R"; svc "$R" console /bin/crasher RESTART=always MAX_RESTARTS=1
run "$R"
has "$OUT" "restarting console (1/1)" && has "$OUT" "entering recovery shell" && has "$OUT" "recovery shell exited" \
    && [[ $RC -eq 0 ]] && ok "failing console falls back to recovery shell" || { bad "console crash loop (rc=$RC)"; echo "$OUT" | tail -15; }

# ---- 9. aetherctl refuses when PID 1 is not the Aether init ----
"$BIN/aetherctl" poweroff 2>"$TMP/err"; rc=$?
[[ $rc -eq 1 ]] && grep -q "refusing" "$TMP/err" && ok "aetherctl refuses to signal a foreign PID 1" || bad "aetherctl safety (rc=$rc)"
"$BIN/aetherctl" bogus 2>/dev/null; [[ $? -eq 2 ]] && ok "aetherctl rejects unknown command" || bad "aetherctl usage"
"$BIN/aetherctl" list 2>/dev/null; [[ $? -eq 1 ]] && ok "aetherctl list without init socket fails cleanly" || bad "list without init"
"$BIN/aetherctl" check "$TMP/nonexistent" 2>/dev/null; [[ $? -ne 0 ]] && ok "aetherctl check fails on missing dir" || bad "check missing dir"
"$BIN/aetherctl" check "$PROJECT/config/services" >/dev/null && ok "shipped service configs validate" || bad "shipped configs"

exit $fail
