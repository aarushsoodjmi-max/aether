#!/bin/bash
# Verifies the initramfs generator: valid newc structure, correct content,
# device nodes, normal vs. invalid input, and byte-for-byte reproducibility.
set -uo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
MK="$PROJECT/build/bin/mkinitramfs"
TMP="$(mktemp -d /tmp/aether-mkinit-XXXXXX)"
trap 'rm -rf -- "$TMP"' EXIT
fail=0
ok()   { echo "PASS: $1"; }
bad()  { echo "FAIL: $1"; fail=1; }

mkdir -p "$TMP/root/bin" "$TMP/root/dev" "$TMP/root/etc"
printf 'hello\n' > "$TMP/root/etc/motd"
printf '#!/bin/sh\nexit 0\n' > "$TMP/root/bin/tool"; chmod 755 "$TMP/root/bin/tool"
ln -s tool "$TMP/root/bin/alias"
: > "$TMP/root/etc/empty"

"$MK" -r "$TMP/root" -o "$TMP/a.cpio" -n dev/console:c:600:5:1 -n dev/null:c:666:1:3 \
    && ok "archive created" || bad "archive created"
"$MK" -r "$TMP/root" -o "$TMP/b.cpio" -n dev/console:c:600:5:1 -n dev/null:c:666:1:3
cmp -s "$TMP/a.cpio" "$TMP/b.cpio" && ok "reproducible (same input, same bytes)" \
    || bad "reproducible"
# touching mtimes must not change the archive
touch -d '2001-01-01' "$TMP/root/etc/motd"
"$MK" -r "$TMP/root" -o "$TMP/c.cpio" -n dev/console:c:600:5:1 -n dev/null:c:666:1:3
cmp -s "$TMP/a.cpio" "$TMP/c.cpio" && ok "independent of file mtimes" || bad "mtime independence"

python3 - "$TMP/a.cpio" <<'PY' && ok "newc structure, contents, modes, devices" || bad "newc structure"
import sys, stat
data = open(sys.argv[1], 'rb').read()
pos, ents = 0, {}
while True:
    assert data[pos:pos+6] == b'070701', "bad magic at %d" % pos
    f = [int(data[pos+6+8*i:pos+14+8*i], 16) for i in range(13)]
    ino, mode, uid, gid, nlink, mtime, size, dmaj, dmin, rmaj, rmin, namesz, chk = f
    name = data[pos+110:pos+110+namesz-1].decode()
    assert data[pos+110+namesz-1] == 0
    pos = (pos + 110 + namesz + 3) & ~3
    body = data[pos:pos+size]
    pos = (pos + size + 3) & ~3
    if name == 'TRAILER!!!':
        break
    assert uid == 0 and gid == 0 and mtime == 0, name
    ents[name] = (mode, body, rmaj, rmin)
assert pos == len(data), "trailing garbage"
assert list(ents) == sorted(ents), "entries not sorted"
for d in ('.', 'bin', 'dev', 'etc'):
    assert stat.S_ISDIR(ents[d][0]), d
assert ents['etc/motd'][1] == b'hello\n'
assert ents['etc/empty'][1] == b''
assert stat.S_ISREG(ents['bin/tool'][0]) and ents['bin/tool'][0] & 0o777 == 0o755
assert stat.S_ISLNK(ents['bin/alias'][0]) and ents['bin/alias'][1] == b'tool'
m, _, ma, mi = ents['dev/console']
assert stat.S_ISCHR(m) and m & 0o777 == 0o600 and (ma, mi) == (5, 1)
m, _, ma, mi = ents['dev/null']
assert stat.S_ISCHR(m) and m & 0o777 == 0o666 and (ma, mi) == (1, 3)
PY

# --- invalid input ---
"$MK" -r "$TMP/nope" -o "$TMP/x.cpio" 2>/dev/null && bad "missing root rejected" || ok "missing root rejected"
"$MK" -r "$TMP/root" -o "$TMP/x.cpio" -n dev/x:q:600:1:1 2>/dev/null && bad "bad node type rejected" || ok "bad node type rejected"
"$MK" -r "$TMP/root" -o "$TMP/x.cpio" -n etc/motd:c:600:1:1 2>/dev/null && bad "node colliding with file rejected" || ok "node colliding with file rejected"
"$MK" -r "$TMP/root" 2>/dev/null && bad "missing -o rejected" || ok "missing -o rejected"
mkfifo "$TMP/root/etc/fifo"
"$MK" -r "$TMP/root" -o "$TMP/x.cpio" 2>/dev/null && bad "unsupported file type rejected" || ok "unsupported file type rejected"

exit $fail
