#!/bin/bash
# aether-net tests: config validation, and real interface/route/DNS setup
# inside a throw-away network namespace (veth pair). Needs root; skips (77)
# otherwise. Uses the BusyBox applets ip/ping only to inspect and probe.
set -uo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
NET="$PROJECT/build/bin/aether-net"
BB="${BUSYBOX:-$PROJECT/build/bin/busybox}"
TMP="$(mktemp -d /tmp/aether-net-XXXXXX)"
trap 'rm -rf -- "$TMP"' EXIT
fail=0
ok()  { echo "PASS: $1"; }
bad() { echo "FAIL: $1"; fail=1; }

# ---- configuration validation (no privileges needed) ----
conf() { rm -rf "$TMP/c"; mkdir -p "$TMP/c"; printf '%b' "$1" > "$TMP/c/a.conf"; }
expect_bad() { conf "$2"; "$NET" check "$TMP/c" >/dev/null 2>&1 && bad "reject: $1" || ok "reject: $1"; }
conf 'IFACE=eth0\nADDRESS=10.0.2.15/24\nGATEWAY=10.0.2.2\nDNS=10.0.2.3,1.1.1.1\n'
"$NET" check "$TMP/c" >/dev/null && ok "valid static config" || bad "valid static config"
conf '# dhcp\nIFACE=eth0\nDHCP=yes\n'
"$NET" check "$TMP/c" >/dev/null && ok "valid dhcp config" || bad "valid dhcp config"
"$NET" check "$TMP/nonexistent" >/dev/null && ok "missing dir is lo-only (not an error)" || bad "missing dir"
expect_bad "missing IFACE" 'ADDRESS=10.0.0.1/24\n'
expect_bad "bad address" 'IFACE=e\nADDRESS=10.0.0.300/24\n'
expect_bad "address without prefix" 'IFACE=e\nADDRESS=10.0.0.1\n'
expect_bad "prefix > 32" 'IFACE=e\nADDRESS=10.0.0.1/33\n'
expect_bad "gateway without address" 'IFACE=e\nGATEWAY=10.0.0.1\nDHCP=no\n'
expect_bad "gateway off-subnet" 'IFACE=e\nADDRESS=10.0.0.5/24\nGATEWAY=192.168.1.1\n'
expect_bad "dhcp + static" 'IFACE=e\nDHCP=yes\nADDRESS=10.0.0.5/24\n'
expect_bad "no address and no dhcp" 'IFACE=e\n'
expect_bad "four DNS servers" 'IFACE=e\nADDRESS=10.0.0.5/24\nDNS=1.1.1.1,1.1.1.2,1.1.1.3,1.1.1.4\n'
expect_bad "bad DNS" 'IFACE=e\nADDRESS=10.0.0.5/24\nDNS=not-an-ip\n'
expect_bad "unknown key" 'IFACE=e\nADDRESS=10.0.0.5/24\nFOO=1\n'
expect_bad "duplicate IFACE" 'IFACE=e\nIFACE=f\nADDRESS=10.0.0.5/24\n'
expect_bad "interface name too long" 'IFACE=abcdefghijklmnopq\nADDRESS=10.0.0.5/24\n'
expect_bad "line without =" 'IFACE=e\nADDRESS=10.0.0.5/24\nnonsense\n'
conf 'IFACE=e\nADDRESS=10.0.0.5/24\n'; printf 'IFACE=e\nADDRESS=10.0.0.6/24\n' > "$TMP/c/b.conf"
"$NET" check "$TMP/c" >/dev/null 2>&1 && bad "reject: same interface in two files" || ok "reject: same interface in two files"
"$NET" bogus 2>/dev/null; [[ $? -eq 2 ]] && ok "usage error" || bad "usage error"

# ---- real configuration in a network namespace ----
[[ $EUID -eq 0 ]] || { echo "SKIP: live tests need root"; exit $fail; }
[[ -x "$BB" ]] || { echo "SKIP: live tests need busybox"; exit $fail; }
unshare --net true 2>/dev/null || { echo "SKIP: no netns"; exit $fail; }

mkdir -p "$TMP/net"
printf 'IFACE=v0\nADDRESS=10.9.8.7/24\nGATEWAY=10.9.8.2\nDNS=10.9.8.1,1.1.1.1\n' > "$TMP/net/v0.conf"
cat > "$TMP/live.sh" <<S
BB=$BB; NET=$NET
\$BB ip link add v0 type veth || exit 90   # BusyBox names the peer veth0
\$BB ip addr add 10.9.8.1/24 dev veth0 && \$BB ip link set veth0 up || exit 91
\$NET up $TMP/net || echo "UP-FAILED"
echo "--status"; \$NET status
echo "--route"; \$BB ip route
echo "--addr"; \$BB ip addr show v0
echo "--ping-lo"; \$BB ping -c1 -W2 127.0.0.1 >/dev/null && echo LO-PING-OK
echo "--ping-peer"; \$BB ping -c1 -W3 10.9.8.1 >/dev/null && echo PEER-PING-OK
echo "--again"; \$NET up $TMP/net >/dev/null 2>&1 && echo IDEMPOTENT-OK
S
AETHER_RESOLV_CONF="$TMP/resolv.conf" unshare --net sh "$TMP/live.sh" > "$TMP/live.out" 2>&1
rc=$?
if [[ $rc -eq 90 ]]; then echo "SKIP: kernel cannot create veth pairs"; exit $fail; fi
OUT="$(cat "$TMP/live.out")"
has() { grep -qF -- "$1" <<<"$OUT"; }
has "lo: up 127.0.0.1/8" && ok "loopback brought up" || bad "loopback up"
has "v0: up 10.9.8.7/24" && ok "interface configured" || { bad "interface configured"; echo "$OUT"; }
has "v0: up 10.9.8.7 " && ok "status lists interface" || bad "status"
has "default via 10.9.8.2 dev v0" && ok "default route installed" || bad "default route"
has "10.9.8.7/24" && has "UP" && ok "address and UP flag visible to ip" || bad "ip addr"
has "LO-PING-OK" && ok "loopback works (ping 127.0.0.1)" || bad "loopback ping"
has "PEER-PING-OK" && ok "real traffic over configured link (ping peer)" || bad "peer ping"
has "IDEMPOTENT-OK" && ok "running up twice is safe" || bad "idempotent"
[[ "$(cat "$TMP/resolv.conf" 2>/dev/null)" == $'nameserver 10.9.8.1\nnameserver 1.1.1.1' ]] && ok "resolv.conf written" || bad "resolv.conf"

printf 'IFACE=nosuch0\nADDRESS=10.1.1.1/24\n' > "$TMP/net/v0.conf"
unshare --net "$NET" up "$TMP/net" >/dev/null 2>&1; [[ $? -eq 1 ]] && ok "nonexistent interface reported (exit 1)" || bad "nonexistent interface"
exit $fail
