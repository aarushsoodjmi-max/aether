#!/bin/bash
# End-to-end tests of aether-pm against temporary roots (never touches the host system).
set -uo pipefail
P="$(cd "$(dirname "$0")/.." && pwd)"; PM="$P/build/bin/aether-pm"
[[ -x "$PM" ]] || { echo "build first: make build/bin/aether-pm" >&2; exit 1; }
TMP="$(mktemp -d /tmp/aether-pmtest-XXXXXX)"; trap 'chmod -R u+rwX "$TMP" 2>/dev/null; rm -rf "$TMP"' EXIT
pass=0; fail=0
ok()  { echo "PASS $1"; pass=$((pass+1)); }
bad() { echo "FAIL $1"; [[ -n "${2:-}" ]] && echo "    $2" | head -5; fail=$((fail+1)); }
expect_ok()   { local n="$1"; shift; local o; if o=$("$@" 2>&1); then ok "$n"; else bad "$n" "$o"; fi; }
expect_fail() { local n="$1" pat="$2"; shift 2; local o; if o=$("$@" 2>&1); then bad "$n (unexpectedly succeeded)" "$o"; elif [[ -z "$pat" || "$o" == *"$pat"* ]]; then ok "$n"; else bad "$n (wrong error)" "$o"; fi; }

K="$TMP/keys"; mkdir -p "$K"
"$PM" keygen "$K/good" >/dev/null; "$PM" keygen "$K/evil" >/dev/null
[[ "$(stat -c %a "$K/good.sec")" == 600 ]] && ok "keygen: secret key is mode 0600" || bad "keygen: secret key mode"

# ---- build a small package universe -------------------------------------------------------
mk() { # mk NAME VER "extra info lines" files...   (files given as path=content)
    local n="$1" v="$2" extra="$3"; shift 3; local d="$TMP/src/$n-$v"; rm -rf "$d"; mkdir -p "$d/tree"
    printf 'format 1\nname %s\nversion %s\narch x86_64\nlibc %s\nsummary %s %s\n%b' "$n" "$v" "${LIBC:-none}" "$n" "$v" "$extra" > "$d/INFO"
    local f; for f in "$@"; do local path="${f%%=*}" c="${f#*=}"; mkdir -p "$d/tree/$(dirname "$path")"; printf '%s\n' "$c" > "$d/tree/$path"; done
    [[ -d "$d/tree/usr/bin" ]] && chmod 755 "$d/tree/usr/bin"/* 2>/dev/null
    mkdir -p "$TMP/repo"; "$PM" build "$d/INFO" "$d/tree" "$TMP/repo/$n-$v.aether" --key "${SIGNKEY:-$K/good.sec}" >/dev/null
}
mk libfoo 1.0 'provides-soname libfoo.so.1\n' usr/lib/libfoo.so.1=foo1 usr/share/foo/old.txt=old
mk libfoo 2.0 'provides-soname libfoo.so.2\n' usr/lib/libfoo.so.2=foo2
mk app 1.0 'depends libfoo >= 1.0\n' usr/bin/app=app1
mk app 2.0 'depends libfoo >= 2.0\nneeds-soname libfoo.so.2\n' usr/bin/app=app2 usr/share/app/data=data
mk badapp 1.0 'conflicts app\n' usr/bin/badapp=bad
LIBC=glibc-2.39 mk libc-a 1.0 '' usr/lib/libc-a=a
LIBC=glibc-2.35 mk libc-old 1.0 '' usr/lib/libc-old=old
"$PM" index "$TMP/repo" --name test --key "$K/good.sec" --timestamp 100 >/dev/null && ok "repo: signed index created"

newroot() { local r="$1"; rm -rf "$r"; mkdir -p "$r/etc/aether/keys"; cp "$K/good.pub" "$r/etc/aether/keys/"; printf 'repo test file://%s\n' "$TMP/repo" > "$r/etc/aether/pm.conf"; }
R1="$TMP/root1"; newroot "$R1"; PMR=("$PM" --root "$R1")

# ---- unprivileged inspection ---------------------------------------------------------------
expect_ok "inspect: shows metadata" "$PM" inspect "$TMP/repo/app-2.0.aether" --files
expect_ok "verify: valid package passes" "$PM" --keys "$K" verify "$TMP/repo/app-2.0.aether"
expect_fail "verify: untrusted key rejected" "untrusted" "$PM" --keys "$TMP/nokeys" verify "$TMP/repo/app-2.0.aether"

# ---- repository update, install, ownership -------------------------------------------------
expect_ok "update: fetches + verifies signed index" "${PMR[@]}" update
expect_ok "install: --dry-run changes nothing" "${PMR[@]}" --dry-run install app
[[ ! -e "$R1/usr/bin/app" ]] && ok "dry-run left filesystem untouched" || bad "dry-run modified filesystem"
out=$("${PMR[@]}" install app 2>&1); echo "$out" | grep -q 'install  libfoo 2.0' && echo "$out" | grep -q 'install  app 2.0' && ok "install: pulls newest dependency first" || bad "install plan" "$out"
[[ "$(cat "$R1/usr/bin/app")" == app2 && -x "$R1/usr/bin/app" && "$(cat "$R1/usr/lib/libfoo.so.2")" == foo2 ]] && ok "install: files placed with modes" || bad "install: files"
expect_ok "check: installed files verify" "${PMR[@]}" check
[[ "$("${PMR[@]}" owner /usr/bin/app)" == *"owned by app 2.0"* ]] && ok "owner: file ownership tracked" || bad "owner"
[[ "$("${PMR[@]}" list | tr '\n' ' ')" == "app 2.0 libfoo 2.0 " || "$("${PMR[@]}" list | tr '\n' ' ')" == "libfoo 2.0 app 2.0 " ]] && ok "list: both installed" || bad "list"
find "$R1" -name '*.aether-new' -o -name '*.aether-old' | grep -q . && bad "no staging leftovers" || ok "no .aether-new/.aether-old leftovers"
[[ ! -e "$R1/var/lib/aether/pm/txn/journal" ]] && ok "journal cleaned after commit" || bad "journal left behind"
expect_ok "install: idempotent when already installed" "${PMR[@]}" install app
echo x >> "$R1/usr/bin/app"; expect_fail "check: detects modified file" "MODIFIED" "${PMR[@]}" check
# restore for later
"${PMR[@]}" remove --force app >/dev/null 2>&1 || true

# ---- dependency protection / conflicts / libc ABI ------------------------------------------
newroot "$R1"; "${PMR[@]}" update >/dev/null; "${PMR[@]}" install app >/dev/null
expect_fail "remove: reverse dependency blocks removal" "still requires" "${PMR[@]}" remove libfoo
expect_fail "install: declared conflict refused" "conflicts" "${PMR[@]}" install badapp
"${PMR[@]}" install libc-a >/dev/null 2>&1
expect_fail "install: refuses to mix libc ABIs" "libc ABI mismatch" "${PMR[@]}" install libc-old
expect_ok "remove: dependent first" "${PMR[@]}" remove app
expect_ok "remove: then its library" "${PMR[@]}" remove libfoo
[[ ! -e "$R1/usr/lib/libfoo.so.2" && ! -e "$R1/usr/bin/app" && ! -e "$R1/usr/share/app" ]] && ok "remove: files and empty dirs gone" || bad "remove leftovers"
[[ -z "$("${PMR[@]}" list | grep -E 'app|libfoo')" ]] && ok "remove: database updated" || bad "remove db"

# ---- upgrades (old versions installed first) -----------------------------------------------
newroot "$R1"; "${PMR[@]}" update >/dev/null
"${PMR[@]}" install libfoo=1.0 app=1.0 >/dev/null 2>&1
[[ "$(cat "$R1/usr/lib/libfoo.so.1" 2>/dev/null)" == foo1 && -e "$R1/usr/share/foo/old.txt" ]] && ok "setup: old versions installed" || bad "setup old versions"
out=$("${PMR[@]}" upgrade 2>&1); echo "$out" | grep -q 'upgrade' && ok "upgrade: plan lists upgrades" || bad "upgrade plan" "$out"
[[ "$(cat "$R1/usr/bin/app")" == app2 && "$(cat "$R1/usr/lib/libfoo.so.2")" == foo2 ]] && ok "upgrade: new versions in place" || bad "upgrade files"
[[ ! -e "$R1/usr/lib/libfoo.so.1" && ! -e "$R1/usr/share/foo/old.txt" ]] && ok "upgrade: files dropped by new version removed" || bad "upgrade stale files"
expect_ok "upgrade: check passes afterwards" "${PMR[@]}" check
expect_ok "upgrade: nothing left to do" "${PMR[@]}" upgrade

# ---- security: tampering, replay, untrusted signers ----------------------------------------
newroot "$R1"; "${PMR[@]}" update >/dev/null
cp "$TMP/repo/app-2.0.aether" "$TMP/app-2.0.good"
printf 'X' | dd of="$TMP/repo/app-2.0.aether" bs=1 seek=$(( $(stat -c %s "$TMP/repo/app-2.0.aether") - 3 )) conv=notrunc 2>/dev/null
expect_fail "tamper: modified package rejected (size/hash vs signed index)" "does not match the signed index" "${PMR[@]}" install app
[[ ! -e "$R1/usr/bin/app" ]] && ok "tamper: nothing installed" || bad "tamper: partial install"
cp "$TMP/app-2.0.good" "$TMP/repo/app-2.0.aether"
cp "$TMP/repo/INDEX" "$TMP/INDEX.good"; sed -i 's/^timestamp 100$/timestamp 100\n/' "$TMP/repo/INDEX"
expect_fail "tamper: modified index rejected on update" "" "${PMR[@]}" update
cp "$TMP/INDEX.good" "$TMP/repo/INDEX"
"$PM" index "$TMP/repo" --name test --key "$K/good.sec" --timestamp 200 >/dev/null; "${PMR[@]}" update >/dev/null
"$PM" index "$TMP/repo" --name test --key "$K/good.sec" --timestamp 150 >/dev/null
expect_fail "replay: older index refused" "OLDER" "${PMR[@]}" update
"$PM" index "$TMP/repo" --name test --key "$K/evil.sec" --timestamp 300 >/dev/null
expect_fail "untrusted: index signed by unknown key refused" "untrusted" "${PMR[@]}" update
"$PM" index "$TMP/repo" --name test --key "$K/good.sec" --timestamp 300 >/dev/null; "${PMR[@]}" update >/dev/null
# local files: signature policy
"$PM" build "$TMP/src/app-1.0/INFO" "$TMP/src/app-1.0/tree" "$TMP/unsigned.aether" >/dev/null
expect_fail "local: unsigned package refused by default" "unsigned" "${PMR[@]}" install "$TMP/unsigned.aether"
"$PM" build "$TMP/src/app-1.0/INFO" "$TMP/src/app-1.0/tree" "$TMP/evilsigned.aether" --key "$K/evil.sec" >/dev/null
expect_fail "local: package signed by untrusted key refused" "untrusted" "${PMR[@]}" install "$TMP/evilsigned.aether"
expect_ok "local: --allow-unsigned installs, with a loud warning" bash -c '"$@" 2>&1 | grep -q "WITHOUT signature verification"' _ "${PMR[@]}" --allow-unsigned install "$TMP/unsigned.aether"
"${PMR[@]}" remove --force app >/dev/null 2>&1; "${PMR[@]}" remove --force libfoo >/dev/null 2>&1
"$PM" build "$TMP/src/libfoo-1.0/INFO" "$TMP/src/libfoo-1.0/tree" "$TMP/libfoo-local.aether" --key "$K/good.sec" >/dev/null
expect_ok "local: package signed by trusted key installs" "${PMR[@]}" install "$TMP/libfoo-local.aether"

# ---- filesystem safety ---------------------------------------------------------------------
newroot "$R1"; "${PMR[@]}" update >/dev/null; mkdir -p "$TMP/outside" "$R1/usr"; ln -s "$TMP/outside" "$R1/usr/lib"
expect_fail "symlink: refuses to write through a symlinked directory" "not a directory" "${PMR[@]}" install libfoo=1.0
[[ -z "$(ls -A "$TMP/outside")" ]] && ok "symlink: nothing written outside the root" || bad "symlink escape!" "$(ls -A "$TMP/outside")"
rm -f "$R1/usr/lib"; mkdir -p "$R1/usr/bin"; echo precious > "$R1/usr/bin/app"
expect_fail "overwrite: unowned existing file is never replaced" "not owned by any package" "${PMR[@]}" install app=1.0
[[ "$(cat "$R1/usr/bin/app")" == precious ]] && ok "overwrite: file content preserved" || bad "overwrite clobbered file"
rm -f "$R1/usr/bin/app"
mkdir -p "$TMP/two"; mk filea 1.0 '' usr/share/shared/f=A; mk fileb 1.0 '' usr/share/shared/f=B
"$PM" index "$TMP/repo" --name test --key "$K/good.sec" --timestamp 400 >/dev/null; "${PMR[@]}" update >/dev/null
expect_fail "collision: two packages owning one path refused" "file conflict" "${PMR[@]}" install filea fileb
# a package owning the package database / trust store / usr-merge alias is not even buildable
mkdir -p "$TMP/src/evil/tree/etc/aether/keys"; echo k > "$TMP/src/evil/tree/etc/aether/keys/x.pub"; printf 'format 1\nname evil\nversion 1\narch x86_64\nlibc none\n' > "$TMP/src/evil/INFO"
expect_fail "policy: package cannot own the trust store" "not allowed" "$PM" build "$TMP/src/evil/INFO" "$TMP/src/evil/tree" "$TMP/evil.aether"
mkdir -p "$TMP/src/evil2/tree/lib64"; echo x > "$TMP/src/evil2/tree/lib64/ld.so"; cp "$TMP/src/evil/INFO" "$TMP/src/evil2/INFO"
expect_fail "policy: package cannot own /lib64 (usr-merge alias)" "not allowed" "$PM" build "$TMP/src/evil2/INFO" "$TMP/src/evil2/tree" "$TMP/evil2.aether"

# ---- privileges ----------------------------------------------------------------------------
if command -v setpriv >/dev/null && [[ $EUID -eq 0 ]]; then
    chmod -R a+rX "$TMP"; chmod a+x "$PM" "$P/build/bin" "$P/build" 2>/dev/null; U=(setpriv --reuid=65534 --regid=65534 --clear-groups)
    expect_ok "unprivileged: inspect works" "${U[@]}" "$PM" inspect "$TMP/repo/app-2.0.aether"
    expect_ok "unprivileged: verify works" "${U[@]}" "$PM" --keys "$K" verify "$TMP/repo/app-2.0.aether"
    expect_ok "unprivileged: list/check work on a readable root" "${U[@]}" "$PM" --root "$R1" check
    expect_fail "unprivileged: system install refused" "needs root" "${U[@]}" "$PM" install app
else echo "SKIP unprivileged tests (need root + setpriv)"; fi

# ---- crash safety: kill the process after every journal record -------------------------------
crash_case() { # crash_case LABEL COMMAND...   (root already prepared at $R1 with old versions, snapshot in $TMP/snap)
    local label="$1"; shift; local n=1 crashed=0 mixed=0 oldst=0 newst=0
    while [[ $n -le 400 ]]; do
        rm -rf "$R1"; cp -a "$TMP/snap" "$R1"
        AETHER_PM_CRASH_AT=$n "${PMR[@]}" "$@" >/dev/null 2>&1; rc=$?
        if [[ $rc -ne 99 ]]; then break; fi
        crashed=$((crashed+1))
        "${PMR[@]}" recover >/dev/null 2>&1 || { bad "$label: recover failed at crash point $n"; return; }
        chk=$("${PMR[@]}" check 2>&1); crc=$?
        lst=$("${PMR[@]}" list | sort | tr '\n' ' ')
        left=$(find "$R1" \( -name '*.aether-new' -o -name '*.aether-old' \) | head -1)
        if [[ $crc -ne 0 || -n "$left" || -e "$R1/var/lib/aether/pm/txn/journal" ]]; then bad "$label: inconsistent after crash at point $n" "check=$chk left=$left"; return; fi
        if [[ "$lst" == "$OLDLIST" ]]; then oldst=$((oldst+1)); elif [[ "$lst" == "$NEWLIST" ]]; then newst=$((newst+1)); else mixed=$((mixed+1)); fi
        n=$((n+1))
    done
    if [[ $crashed -gt 5 && $mixed -eq 0 ]]; then ok "$label: $crashed crash points all recovered to a consistent state ($oldst rolled back, $newst rolled forward)"; else bad "$label: crashed=$crashed mixed=$mixed (n=$n rc=${rc:-?})"; fi
}
newroot "$R1"; "${PMR[@]}" update >/dev/null; "${PMR[@]}" install libfoo=1.0 app=1.0 >/dev/null 2>&1
OLDLIST=$("${PMR[@]}" list | sort | tr '\n' ' '); cp -a "$R1" "$TMP/snap"
rm -rf "$R1"; cp -a "$TMP/snap" "$R1"; "${PMR[@]}" upgrade >/dev/null 2>&1; NEWLIST=$("${PMR[@]}" list | sort | tr '\n' ' ')
crash_case "crash-safety (upgrade)" upgrade
rm -rf "$TMP/snap"; newroot "$R1"; "${PMR[@]}" update >/dev/null; "${PMR[@]}" install libfoo=2.0 app=2.0 >/dev/null 2>&1
OLDLIST=$("${PMR[@]}" list | sort | tr '\n' ' '); cp -a "$R1" "$TMP/snap"
"${PMR[@]}" remove app >/dev/null 2>&1; NEWLIST=$("${PMR[@]}" list | sort | tr '\n' ' '); rm -rf "$R1"; cp -a "$TMP/snap" "$R1"
crash_case "crash-safety (remove)" remove app
rm -rf "$TMP/snap"; newroot "$R1"; "${PMR[@]}" update >/dev/null; OLDLIST=""; cp -a "$R1" "$TMP/snap"
rm -rf "$R1"; cp -a "$TMP/snap" "$R1"; "${PMR[@]}" install app >/dev/null 2>&1; NEWLIST=$("${PMR[@]}" list | sort | tr '\n' ' ')
crash_case "crash-safety (fresh install)" install app

echo "PM SUMMARY pass=$pass fail=$fail"; [[ $fail -eq 0 ]]
