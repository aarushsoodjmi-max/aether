#!/bin/bash
# Provision the QEMU test environment INSIDE the project (build/hosttools); the host system is never modified.
#  - qemu-system-x86_64 (+ shim wrapper), zstd, kmod: downloaded as .debs with an isolated apt configuration
#    (private state/cache dirs) and extracted with dpkg -x
#  - a prebuilt Ubuntu kernel image -> build/kernel/bzImage (TEMPORARY foundation; see docs/kernel.md)
#  - its matching modules -> staged GUI modules in build/gui-modules via tools/stage-gui-modules.sh
# Requires: Ubuntu 24.04-like host (apt-get, dpkg), network access to archive.ubuntu.com. Versions are NOT pinned
# (they follow the archive); record what you got with: ls build/hosttools/debs.
# Afterwards:  export PATH="$PWD/build/hosttools/bin:$PATH"
set -euo pipefail
P="$(cd "$(dirname "$0")/.." && pwd)"; H="$P/build/hosttools"; A="$H/apt"
mkdir -p "$A/state/lists/partial" "$A/cache/archives/partial" "$A/etc/preferences.d" "$H/root" "$H/debs" "$H/bin" "$P/build/kernel"
cp /var/lib/dpkg/status "$A/state/status"
cat > "$A/etc/sources.list" <<'EOS'
deb [trusted=yes] http://archive.ubuntu.com/ubuntu noble main universe
deb [trusted=yes] http://archive.ubuntu.com/ubuntu noble-updates main universe
deb [trusted=yes] http://security.ubuntu.com/ubuntu noble-security main universe
EOS
cat > "$A/apt.conf" <<EOC
Dir::State "$A/state"; Dir::State::status "$A/state/status"; Dir::Cache "$A/cache";
Dir::Etc::sourcelist "$A/etc/sources.list"; Dir::Etc::sourceparts "/dev/null"; Dir::Etc::preferencesparts "$A/etc/preferences.d";
APT::Get::AllowUnauthenticated "true"; Debug::NoLocking "true";
EOC
export APT_CONFIG="$A/apt.conf"
echo "== apt update (isolated)"; apt-get update >/dev/null
echo "== downloading qemu, zstd, kmod"; apt-get install -y --download-only --no-install-recommends qemu-system-x86 zstd kmod >/dev/null
for d in "$A"/cache/archives/*.deb; do cp --update=none "$d" "$H/debs/"; dpkg -x "$d" "$H/root"; done
K="$(apt-cache search '^linux-image-unsigned-.*-generic$' | awk '{print $1}' | sort -V | tail -1)"; KV="${K#linux-image-unsigned-}"
echo "== kernel $K"; (cd "$H/debs" && apt-get download "$K" "linux-modules-$KV" >/dev/null 2>&1 || true)
rm -rf "$H/kernel" "$H/modules"; mkdir -p "$H/kernel" "$H/modules"
dpkg -x "$H"/debs/linux-image-unsigned-"$KV"_*.deb "$H/kernel"; dpkg -x "$H"/debs/linux-modules-"$KV"_*.deb "$H/modules"
install -m 0644 "$H/kernel/boot/vmlinuz-$KV" "$P/build/kernel/bzImage"
LIBS="$H/root/usr/lib/x86_64-linux-gnu"
cat > "$H/bin/qemu-system-x86_64" <<EOW
#!/bin/sh
# project-local QEMU wrapper (extracted .debs under $H/root; host untouched)
export LD_LIBRARY_PATH=$LIBS\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}
export QEMU_MODULE_DIR=$LIBS/qemu
exec $H/root/usr/bin/qemu-system-x86_64 -L $H/root/usr/share/qemu -L $H/root/usr/share/seabios -L $H/root/usr/lib/ipxe/qemu "\$@"
EOW
chmod +x "$H/bin/qemu-system-x86_64"
echo "== staging GUI kernel modules"; LD_LIBRARY_PATH="$LIBS" MODDIR="$H/modules/lib/modules/$KV" ZSTD="$H/root/usr/bin/zstd" "$P/tools/stage-gui-modules.sh" >/dev/null
echo "kernel: $P/build/kernel/bzImage ($KV)"; "$H/bin/qemu-system-x86_64" --version | head -1
echo "done. Run: export PATH=\"$H/bin:\$PATH\""
