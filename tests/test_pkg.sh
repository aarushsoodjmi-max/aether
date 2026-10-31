#!/bin/bash
# Tests for aether-pkg: normal operation, upgrade, conflicts, tampering,
# path traversal, symlink escape, corrupt archives, verify, remove.
set -uo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
PKG="$PROJECT/build/bin/aether-pkg"
TMP="$(mktemp -d /tmp/aether-pkg-XXXXXX)"
trap 'rm -rf -- "$TMP"' EXIT
fail=0
ok()  { echo "PASS: $1"; }
bad() { echo "FAIL: $1"; fail=1; }
R="$TMP/root"; mkdir -p "$R"
p() { "$PKG" -r "$R" "$@"; }

# --- sha256 sanity through a package ---
mkdir -p "$TMP/hello/usr/bin" "$TMP/hello/usr/share/hello"
printf '#!/bin/sh\necho hello\n' > "$TMP/hello/usr/bin/hello"; chmod 755 "$TMP/hello/usr/bin/hello"
printf 'doc\n' > "$TMP/hello/usr/share/hello/README"
ln -s hello "$TMP/hello/usr/bin/hi"
p build "$TMP/hello" "$TMP/hello-1.apkg" hello 1.0 >/dev/null && ok "build package" || bad "build package"
"$PKG" build "$TMP/hello" "$TMP/hello-1b.apkg" hello 1.0 >/dev/null
cmp -s "$TMP/hello-1.apkg" "$TMP/hello-1b.apkg" && ok "build is deterministic" || bad "build deterministic"
grep -aq "FILE=$(sha256sum "$TMP/hello/usr/share/hello/README" | cut -d' ' -f1) usr/share/hello/README" "$TMP/hello-1.apkg" \
    && ok "manifest checksum equals sha256sum" || bad "manifest checksum"

p install "$TMP/hello-1.apkg" >/dev/null && ok "install" || bad "install"
[[ "$("$R/usr/bin/hello")" == hello && -L "$R/usr/bin/hi" && -x "$R/usr/bin/hello" ]] && ok "files, mode and symlink installed" || bad "installed content"
[[ "$(p list)" == "hello 1.0" ]] && ok "list" || bad "list"
p files hello | grep -qx usr/bin/hello && ok "files" || bad "files"
p verify >/dev/null && ok "verify clean" || bad "verify clean"
ls "$R/usr/bin" | grep -q aether-new && bad "no temp files left" || ok "no temp files left"

echo tamper >> "$R/usr/bin/hello"
out="$(p verify)"; rc=$?
[[ $rc -eq 1 && "$out" == *"MODIFIED usr/bin/hello"* ]] && ok "verify detects modified file" || bad "verify modified"
rm "$R/usr/share/hello/README"
out="$(p verify)"; [[ "$out" == *"MISSING  usr/share/hello/README"* ]] && ok "verify detects missing file" || bad "verify missing"
printf '#!/bin/sh\necho hello\n' > "$R/usr/bin/hello"; printf 'doc\n' > "$R/usr/share/hello/README"

# --- upgrade drops stale files, keeps new ---
rm "$TMP/hello/usr/share/hello/README"; printf 'new\n' > "$TMP/hello/usr/bin/extra"; chmod 755 "$TMP/hello/usr/bin/extra"
p build "$TMP/hello" "$TMP/hello-2.apkg" hello 2.0 >/dev/null
out="$(p install "$TMP/hello-2.apkg")"; [[ "$out" == upgraded* && "$(p list)" == "hello 2.0" ]] \
    && [[ ! -e "$R/usr/share/hello/README" && -f "$R/usr/bin/extra" ]] && ok "upgrade removes stale files" || bad "upgrade"

# --- conflicts ---
mkdir -p "$TMP/other/usr/bin"; echo x > "$TMP/other/usr/bin/extra"
p build "$TMP/other" "$TMP/other.apkg" other 1.0 >/dev/null
out="$(p install "$TMP/other.apkg" 2>&1)"; [[ $? -eq 0 ]]; [[ "$out" == *"owned by package hello"* && ! -e "$R/usr/bin/extra.aether-new" ]] && [[ "$(cat "$R/usr/bin/extra")" == new ]] && ok "conflict with another package refused" || bad "package conflict"
echo mine > "$R/stray"; mkdir -p "$TMP/stray"; echo pkg > "$TMP/stray/stray"
p build "$TMP/stray" "$TMP/stray.apkg" stray 1.0 >/dev/null
p install "$TMP/stray.apkg" >/dev/null 2>&1; [[ $? -ne 0 && "$(cat "$R/stray")" == mine ]] && ok "untracked file not overwritten" || bad "untracked overwrite"

# --- tampered / corrupt / malicious archives ---
python3 - "$TMP" <<'PY'
import sys, hashlib
t = sys.argv[1]
def ent(name, data=b'', mode=0o100644):
    nm = name.encode() + b'\0'
    h = b'070701' + b''.join(b'%08X' % v for v in (1, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(nm), 0))
    out = h + nm; out += b'\0' * (-len(out) % 4)
    out += data; out += b'\0' * (-len(out) % 4)
    return out
def pkg(path, name, files, info_files=None, extra=b'', mode=0o100644):
    info = 'NAME=%s\nVERSION=1\n' % name
    for p, d in (info_files if info_files is not None else files):
        info += 'FILE=%s %s\n' % (hashlib.sha256(d).hexdigest(), p)
    a = ent('.PKGINFO', info.encode())
    for p, d in files: a += ent(p, d, mode)
    a += ent('TRAILER!!!') + extra
    open(t + '/' + path, 'wb').write(a)
pkg('traversal.apkg', 'evil1', [('../escape', b'x')])
pkg('absolute.apkg', 'evil2', [('/etc/escape', b'x')])
pkg('dotdot.apkg', 'evil3', [('a/../../escape', b'x')])
pkg('badsum.apkg', 'evil4', [('f', b'real')], info_files=[('f', b'fake')])
pkg('unlisted.apkg', 'evil5', [('f', b'x'), ('g', b'y')], info_files=[('f', b'x')])
pkg('missing.apkg', 'evil6', [('f', b'x')], info_files=[('f', b'x'), ('g', b'y')])
pkg('dup.apkg', 'evil7', [('f', b'x'), ('f', b'x')], info_files=[('f', b'x')])
pkg('badname.apkg', 'bad name', [('f', b'x')])
pkg('device.apkg', 'evil8', [('d', b'')], mode=0o020644)
# symlink escape: package 'lnk' installs link -> outside; 'under' writes through it
import os
os.makedirs(t + '/outside', exist_ok=True)
pkg('lnk.apkg', 'lnk', [('link', (t + '/outside').encode())], info_files=[], mode=0o120777)
pkg('under.apkg', 'under', [('link/pwned', b'x')])
d = open(t + '/hello-1.apkg', 'rb').read()
open(t + '/trunc.apkg', 'wb').write(d[:len(d) // 2])
open(t + '/garbage.apkg', 'wb').write(b'not an archive at all' * 20)
open(t + '/empty.apkg', 'wb').write(b'')
PY
for n in traversal absolute dotdot badsum unlisted missing dup badname device trunc garbage empty; do
    p install "$TMP/$n.apkg" >/dev/null 2>"$TMP/err"; rc=$?
    [[ $rc -eq 1 && -s "$TMP/err" ]] && ok "rejects $n.apkg" || bad "rejects $n.apkg (rc=$rc)"
done
[[ ! -e "$TMP/escape" && ! -e "$R/../escape" && ! -e "$R/etc/escape" && ! -e /etc/escape ]] && ok "nothing written outside root" || bad "escape files exist"
p install "$TMP/p-nonexistent.apkg" 2>/dev/null; [[ $? -eq 1 ]] && ok "missing file rejected" || bad "missing file"
p install "$TMP/lnk.apkg" >/dev/null 2>&1 && ok "symlink package installs" || bad "symlink package"
p install "$TMP/under.apkg" >/dev/null 2>&1; [[ $? -ne 0 && ! -e "$TMP/outside/pwned" ]] && ok "write through symlinked directory refused" || bad "symlink escape"
p remove lnk >/dev/null

# --- remove ---
p remove hello >/dev/null && [[ ! -e "$R/usr/bin/hello" && ! -e "$R/usr/bin/hi" && ! -d "$R/usr/share/hello" ]] \
    && [[ -z "$(p list)" ]] && ok "remove deletes files, empty dirs and db entry" || bad "remove"
p remove hello 2>/dev/null; [[ $? -eq 1 ]] && ok "remove unknown package fails" || bad "remove unknown"
p remove ../x 2>/dev/null; [[ $? -eq 1 ]] && ok "remove rejects bad name" || bad "remove bad name"
"$PKG" 2>/dev/null; [[ $? -eq 2 ]] && ok "usage error" || bad "usage"
exit $fail
