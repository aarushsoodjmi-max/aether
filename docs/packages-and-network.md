> `aether-pkg` (.apkg) described here is the **legacy bootstrap** manager. The native manager is `aether-pm` (`.aether`): see `aether-package-manager.md`.

# Packages and networking

## aether-pkg
`aether-pkg [-r ROOT] build DIR OUT.apkg NAME VERSION | install FILE | remove NAME | list | files NAME | verify [NAME]`

An `.apkg` is a newc cpio archive whose first entry `.PKGINFO` holds `NAME`,
`VERSION` and one `FILE=<sha256> <path>` line per regular file. Install checks
every checksum and that no file is unlisted or missing, rejects absolute paths,
`..`, duplicate entries, device nodes and writes through symlinked directories,
refuses to overwrite files owned by another package or not owned at all, writes
files atomically, and records ownership in `ROOT/var/lib/aether/pkg/NAME.list`.
Upgrade removes files the new version no longer ships. `verify` reports modified
or missing files. Limitations: packages are **not signed** (checksums only protect
against corruption, not a malicious package source), no dependency resolution,
no repository/download support, no rollback if an install fails halfway.

## aether-net
`aether-net up|check [DIR] | status`; DIR defaults to `/etc/aether/network` (one
`*.conf` per interface). Keys: `IFACE`, `ADDRESS=a.b.c.d/len`, `GATEWAY`,
`DNS` (up to 3, written to /etc/resolv.conf), `DHCP=yes`. Loopback is always
configured. Static configuration uses ioctl directly. `DHCP=yes` runs BusyBox
`udhcpc` with `/etc/aether/udhcpc.script` - this path is **untested**. The
`network` service (`/bin/aether-net-up`) runs it at boot.
Example QEMU user-mode network (`config/network/eth0.conf`, not shipped):
`IFACE=eth0`, `ADDRESS=10.0.2.15/24`, `GATEWAY=10.0.2.2`, `DNS=10.0.2.3`.
