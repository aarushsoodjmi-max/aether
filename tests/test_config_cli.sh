#!/bin/bash
# aether-config tests: check / show / get / init on throw-away directories.
# No privileges needed. Never touches /etc.
set -uo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$PROJECT/build/bin/aether-config"
TMP="$(mktemp -d /tmp/aether-cfgcli-XXXXXX)"
trap 'rm -rf -- "$TMP"' EXIT
fail=0
ok()  { echo "PASS: $1"; }
bad() { echo "FAIL: $1"; fail=1; }
rc()  { "$@" >/dev/null 2>&1; echo $?; }

[[ -x "$CFG" ]] || { echo "FAIL: missing $CFG (run 'make')"; exit 1; }

# ---- usage ----
[[ $(rc "$CFG") -eq 2 ]] && ok "no arguments -> usage, exit 2" || bad "no arguments"
[[ $(rc "$CFG" bogus) -eq 2 ]] && ok "unknown command -> exit 2" || bad "unknown command"
[[ $(rc "$CFG" help) -eq 0 ]] && ok "help -> exit 0" || bad "help"
[[ $(rc "$CFG" get) -eq 2 ]] && ok "get without a name -> exit 2" || bad "get without name"

# ---- missing directory: defaults, not an error ----
[[ $(rc "$CFG" check "$TMP/none") -eq 0 ]] && ok "check: missing directory is valid (defaults)" || bad "check missing dir"
[[ "$("$CFG" get system.hostname "$TMP/none" 2>/dev/null)" == "aether" ]] && ok "get: default hostname" || bad "get default"
[[ "$("$CFG" show "$TMP/none" 2>/dev/null | head -2)" == $'system.hostname=aether\nsystem.system_name=Aether OS' ]] \
    && ok "show: effective defaults" || bad "show defaults"

# ---- init creates the files; they validate and equal the defaults ----
OUT="$("$CFG" init "$TMP/etc" 2>&1)"; R=$?
[[ $R -eq 0 && $(grep -c '^created ' <<<"$OUT") -eq 4 ]] && ok "init: creates the four files" || bad "init creates ($R)"
[[ -f "$TMP/etc/system.conf" && -f "$TMP/etc/boot.conf" && -f "$TMP/etc/services.conf" && -f "$TMP/etc/logging.conf" ]] \
    && ok "init: system/boot/services/logging .conf exist" || bad "init file names"
[[ $(rc "$CFG" check "$TMP/etc") -eq 0 ]] && ok "check: generated files are valid" || bad "check generated"
[[ "$("$CFG" show "$TMP/etc" 2>&1)" == "$("$CFG" show "$TMP/none" 2>&1)" ]] && ok "generated files == built-in defaults" || bad "generated != defaults"

# ---- init never overwrites ----
printf 'HOSTNAME=mine\n# keep me\n' > "$TMP/etc/system.conf"
cp -a "$TMP/etc" "$TMP/before"
rm "$TMP/etc/logging.conf"
OUT="$("$CFG" init "$TMP/etc" 2>&1)"; R=$?
[[ $R -eq 0 && $(grep -c '^kept existing ' <<<"$OUT") -eq 3 && $(grep -c '^created ' <<<"$OUT") -eq 1 ]] \
    && ok "init: kept 3 existing, created the missing one" || bad "init second run ($R): $OUT"
[[ "$(cat "$TMP/etc/system.conf")" == $'HOSTNAME=mine\n# keep me' ]] && ok "init: edited file untouched" || bad "edited file changed"
cmp -s "$TMP/before/boot.conf" "$TMP/etc/boot.conf" && cmp -s "$TMP/before/services.conf" "$TMP/etc/services.conf" \
    && ok "init: other existing files untouched" || bad "other files changed"
[[ "$("$CFG" get system.hostname "$TMP/etc" 2>/dev/null)" == "mine" ]] && ok "get: user value wins over default" || bad "user value"

# ---- invalid configuration: reported, defaults kept, exit codes ----
printf 'HOSTNAME=-bad-\nSYSTEM_NAME=Fine\nFOO=1\n' > "$TMP/etc/system.conf"
OUT="$("$CFG" check "$TMP/etc" 2>&1)"; R=$?
[[ $R -eq 1 ]] && ok "check: problems -> exit 1" || bad "check invalid exit ($R)"
grep -q 'system.conf:1: HOSTNAME' <<<"$OUT" && grep -q "system.conf:3: unknown setting 'FOO'" <<<"$OUT" \
    && ok "check: file:line messages" || bad "check messages: $OUT"
[[ "$("$CFG" get system.hostname "$TMP/etc" 2>/dev/null)" == "aether" && \
   "$("$CFG" get system.system_name "$TMP/etc" 2>/dev/null)" == "Fine" ]] \
    && ok "invalid value falls back to default; valid sibling applied" || bad "fallback"
[[ $(rc "$CFG" show "$TMP/etc") -eq 0 ]] && ok "show: warns but still succeeds" || bad "show with problems"

[[ $(rc "$CFG" get nosuch.thing "$TMP/etc") -eq 1 ]] && ok "get: unknown setting -> exit 1" || bad "get unknown"
[[ "$("$CFG" get logging.level "$TMP/etc" 2>/dev/null)" == "info" ]] && ok "get: logging.level" || bad "get level"

# ---- init failure modes ----
: > "$TMP/afile"
[[ $(rc "$CFG" init "$TMP/afile") -eq 1 ]] && ok "init: path that is a file fails (exit 1)" || bad "init on a file"
[[ $(rc "$CFG" init "$TMP/no/such/parent") -eq 1 ]] && ok "init: missing parent fails (exit 1)" || bad "init missing parent"

exit $fail
