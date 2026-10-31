#!/bin/bash
# Assemble the Aether root filesystem and pack it as a deterministic
# initramfs. Everything stays under build/; nothing touches the host.
#
# Environment overrides (used by the tests):
#   ROOT          rootfs directory       (default build/rootfs)
#   IMAGE         output .cpio.gz        (default build/aether-initramfs.cpio.gz)
#   INIT_BIN      init binary            (default build/bin/aether-init)
#   SERVICES_DIR  service .conf files    (default config/services)
#   BUSYBOX       busybox binary         (default build/bin/busybox)
#   NO_IMAGE=1    only assemble the rootfs directory
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$PROJECT/build/bin"
ROOT="${ROOT:-$PROJECT/build/rootfs}"
IMAGE="${IMAGE:-$PROJECT/build/aether-initramfs.cpio.gz}"
INIT_BIN="${INIT_BIN:-$BIN/aether-init}"
SERVICES_DIR="${SERVICES_DIR:-$PROJECT/config/services}"
BUSYBOX="${BUSYBOX:-$BIN/busybox}"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"

# Refuse empty/short/root-like paths before the rm -rf below.
case "$ROOT" in
    ""|"/"|"/home"|"/root"|"/usr"|"/etc"|"/var") echo "refusing ROOT=$ROOT" >&2; exit 1 ;;
esac
[[ ${#ROOT} -ge 8 ]] || { echo "refusing short ROOT=$ROOT" >&2; exit 1; }

for f in "$INIT_BIN" "$BUSYBOX" "$BIN/aetherctl" "$BIN/heartbeat" \
         "$BIN/service-status" "$BIN/mkinitramfs" "$BIN/aether-pkg" "$BIN/aether-net" "$BIN/aether-pm"; do
    [[ -x "$f" ]] || { echo "missing $f (run 'make' / 'make busybox')" >&2; exit 1; }
done
compgen -G "$SERVICES_DIR/*.conf" >/dev/null \
    || { echo "no service configs in $SERVICES_DIR" >&2; exit 1; }

# Validate configuration before packing it.
"$BIN/aetherctl" check "$SERVICES_DIR" >/dev/null \
    || { echo "service configuration invalid:" >&2
         "$BIN/aetherctl" check "$SERVICES_DIR" >&2 || true; exit 1; }

rm -rf -- "$ROOT"
# usr-merged FHS layout (required by glibc-ABI binaries: PT_INTERP is /lib64/ld-linux-x86-64.so.2).
mkdir -p "$ROOT"/{etc,proc,sys,dev,run,tmp,root,usr/bin,usr/sbin,usr/lib,usr/lib64,var/log,var/lib/aether/pkg}
for l in bin sbin lib lib64; do ln -s "usr/$l" "$ROOT/$l"; done
chmod 1777 "$ROOT/tmp"

printf 'root:x:0:0:Root:/root:/bin/sh\n' > "$ROOT/etc/passwd"
printf 'root:x:0:\n' > "$ROOT/etc/group"

install -m 0755 "$BUSYBOX" "$ROOT/bin/busybox"
for cmd in sh ash ls cat echo mount umount clear setsid cttyhack \
           dmesg uname hostname head cut whoami id pwd tty date ps free df \
           mkdir rm cp mv ln sleep sync grep sed kill killall touch chmod chown \
           find tail wc sort tr tar gzip more less vi ip ifconfig route ping nc \
           udhcpc wget blkid switch_root test '[' printf env basename dirname \
           stat du; do
    ln -s busybox "$ROOT/bin/$cmd"
done

mkdir -p "$ROOT/etc/aether/services"
cp "$SERVICES_DIR"/*.conf "$ROOT/etc/aether/services/"
[[ -f "$PROJECT/config/fstab" ]] && install -m 0644 "$PROJECT/config/fstab" "$ROOT/etc/aether/fstab"
if compgen -G "$PROJECT/config/network/*.conf" >/dev/null; then
    mkdir -p "$ROOT/etc/aether/network"
    cp "$PROJECT"/config/network/*.conf "$ROOT/etc/aether/network/"
    "$BIN/aether-net" check "$ROOT/etc/aether/network" >/dev/null \
        || { echo "network configuration invalid" >&2; exit 1; }
fi

install -m 0755 "$BIN/service-status" "$ROOT/bin/service-status"
install -m 0755 "$BIN/heartbeat"      "$ROOT/bin/aether-heartbeat"
install -m 0755 "$BIN/aetherctl"      "$ROOT/bin/aetherctl"
install -m 0755 "$BIN/aether-pkg"     "$ROOT/bin/aether-pkg"
install -m 0755 "$BIN/aether-net"     "$ROOT/bin/aether-net"
install -m 0755 "$BIN/aether-pm"      "$ROOT/bin/aether-pm"
ln -s aether-net "$ROOT/bin/aether-net-up"
ln -s aetherctl "$ROOT/bin/poweroff"
ln -s aetherctl "$ROOT/bin/reboot"
install -m 0755 "$PROJECT/config/udhcpc.script" "$ROOT/etc/aether/udhcpc.script"
install -m 0755 "$INIT_BIN" "$ROOT/init"

# --- optional glibc-ABI compatibility layer (COMPAT=1) -------------------------------
# Installs the signed compat packages (tools/build-compat-repo.sh) INTO the rootfs with aether-pm itself
# (real dependency resolution, signature verification, transactional install). The glibc runtime is a
# TEMPORARY upstream import; see docs/compat.md. The signing SECRET key never enters the image.
if [[ "${COMPAT:-0}" == 1 ]]; then
    REPO="$PROJECT/build/compat/repo"; KEYS="$PROJECT/build/keys"
    [[ -f "$REPO/INDEX" ]] || { echo "run tools/build-compat-repo.sh first" >&2; exit 1; }
    printf 'root:x:0:0:Root:/root:/bin/sh\nnobody:x:65534:65534:Nobody:/nonexistent:/bin/false\n' > "$ROOT/etc/passwd"
    printf 'root:x:0:\nnogroup:x:65534:\n' > "$ROOT/etc/group"
    printf '127.0.0.1 localhost\n::1 localhost\n127.0.1.1 aether\n' > "$ROOT/etc/hosts"
    printf 'NAME="Aether OS"\nID=aether\nPRETTY_NAME="Aether OS (glibc-compat preview)"\n' > "$ROOT/etc/os-release"
    mkdir -p "$ROOT/etc/aether/keys"; install -m 0644 "$KEYS/dev.pub" "$ROOT/etc/aether/keys/aether-dev.pub"
    printf 'repo aether file://%s\n' "$REPO" > "$ROOT/etc/aether/pm.conf"
    "$BIN/aether-pm" --root "$ROOT" update
    "$BIN/aether-pm" --root "$ROOT" install aether-compat-tests
    # loader cache: there is no post-install trigger mechanism yet, so the image build regenerates it
    "$ROOT/usr/sbin/ldconfig.real" -r "$ROOT" -C /etc/ld.so.cache
    # ship a small on-image repository so the running system can exercise update/install/upgrade/remove
    mkdir -p "$ROOT/var/lib/aether/repo"
    cp "$REPO"/INDEX "$REPO"/INDEX.sig "$REPO"/aether-hello*.aether "$ROOT/var/lib/aether/repo/"
    # INDEX lists every package; drop entries whose files are not shipped by re-indexing just the shipped ones
    "$BIN/aether-pm" index "$ROOT/var/lib/aether/repo" --name aether --key "$KEYS/dev.sec" --timestamp "$(date +%s)" >/dev/null
    printf 'repo aether file:///var/lib/aether/repo\n' > "$ROOT/etc/aether/pm.conf"
    rm -rf "$ROOT/var/cache/aether/pm"; mkdir -p "$ROOT/var/cache/aether/pm"
    rm -f "$ROOT/var/lib/aether/pm/state/"*.ts
fi

# --- optional graphical session (GUI=1) -----------------------------------------------
# Installs the Aether compositor/shell/settings/notify binaries, desktop app metadata, the staged kernel
# modules (tools/stage-gui-modules.sh) and a 'session' service. Independent of the package manager.
if [[ "${GUI:-0}" == 1 ]]; then
    for b in aether-compositor aether-shell aether-settings aether-notify aether-terminal aether-files; do install -m 0755 "$BIN/$b" "$ROOT/bin/$b"; done
    install -m 0755 "$PROJECT/config/gui/aether-session" "$ROOT/bin/aether-session"
    install -m 0644 "$PROJECT/config/gui/session.conf" "$ROOT/etc/aether/services/session.conf"
    mkdir -p "$ROOT/usr/share/aether/apps" "$ROOT/usr/lib/aether/modules" "$ROOT/run"
    install -m 0644 "$PROJECT"/config/apps/*.app "$ROOT/usr/share/aether/apps/"
    if compgen -G "$PROJECT/build/gui-modules/*.ko" >/dev/null; then install -m 0644 "$PROJECT"/build/gui-modules/*.ko "$ROOT/usr/lib/aether/modules/"; else echo "warning: no staged GPU modules; the session will stay on the text console" >&2; fi
fi

[[ "${NO_IMAGE:-0}" == 1 ]] && { echo "Aether rootfs built: $ROOT"; exit 0; }

mkdir -p "$(dirname "$IMAGE")"
# Device nodes are created inside the archive, so no root is needed.
"$BIN/mkinitramfs" -r "$ROOT" -o "$IMAGE.tmp" \
    -n dev/console:c:600:5:1 -n dev/null:c:666:1:3 \
    -n dev/tty:c:666:5:0
gzip -n -9 -c "$IMAGE.tmp" > "$IMAGE"
rm -f "$IMAGE.tmp"

echo "Aether initramfs built: $IMAGE"
ls -l "$IMAGE"
