#!/bin/bash
# Build a signed .aether repository (build/compat/repo) from the pinned glibc runtime and the ELF-compat
# test programs, using aether-import to derive shared-library requirements. Dev signing key lives in
# build/keys/ (NOT shipped in images; only the public half is installed as a trusted key).
set -euo pipefail
P="$(cd "$(dirname "$0")/.." && pwd)"; PM="$P/build/bin/aether-pm"; IMP="$P/tools/aether-import"
RT="$P/build/compat/debs"; W="$P/build/compat/pkgwork"; REPO="$P/build/compat/repo"; KEYS="$P/build/keys"
[[ -x "$PM" ]] || { echo "build aether-pm first (make)" >&2; exit 1; }
[[ -d "$RT" && -d "$P/build/compat-tests" ]] || { echo "run: make compat-runtime compat-tests-build" >&2; exit 1; }
mkdir -p "$KEYS"; [[ -f "$KEYS/dev.sec" ]] || "$PM" keygen "$KEYS/dev" >/dev/null
rm -rf "$W" "$REPO"; mkdir -p "$W" "$REPO"
GLIBC=glibc-2.39
deb_pkg() { # deb_pkg PKGNAME DEBGLOB LIBC SUMMARY [--depends X]...
    local name="$1" glob="$2" libc="$3" sum="$4"; shift 4
    local deb ver; deb=$(ls "$RT"/${glob}_*.deb | head -1); ver=$(dpkg-deb -f "$deb" Version | sed 's/^[0-9]*://')
    rm -rf "$W/$name"; mkdir -p "$W/$name"; dpkg-deb -x "$deb" "$W/$name"
    rm -rf "$W/$name/usr/share/doc" "$W/$name/usr/share/man" "$W/$name/usr/share/lintian" "$W/$name/var"   # docs/caches are not part of the runtime
    find "$W/$name" -type d -empty -delete 2>/dev/null || true
    "$IMP" package --name "$name" --version "$ver" --libc "$libc" --summary "$sum" --tree "$W/$name" --out-dir "$REPO" --pm "$PM" --key "$KEYS/dev.sec" "$@"
}
# libc first: the others depend on it
V=$(dpkg-deb -f "$(ls "$RT"/libc6_*.deb | head -1)" Version)
deb_pkg libc6      libc6      "$GLIBC" "GNU C library (imported Ubuntu 24.04 build, glibc 2.39)"
deb_pkg libc-bin   libc-bin   "$GLIBC" "ldconfig, iconv, getconf (imported)" --depends "libc6 = $V"
deb_pkg libgcc-s1  libgcc-s1  "$GLIBC" "GCC runtime support (imported)"
deb_pkg libstdc++6 libstdc++6 "$GLIBC" "GNU C++ runtime (imported)" --depends "libgcc-s1"
deb_pkg zlib1g     zlib1g     "$GLIBC" "zlib (imported)"
# configuration that Aether (not glibc) decides: name-service switch
mkdir -p "$W/aether-glibc-config/etc"
install -m 0644 "$P/build/compat/runtime/usr/share/libc-bin/nsswitch.conf" "$W/aether-glibc-config/etc/nsswitch.conf"
"$IMP" package --name aether-glibc-config --version 1.0 --libc none --summary "Aether name-service configuration (files, dns)" --tree "$W/aether-glibc-config" --out-dir "$REPO" --pm "$PM" --key "$KEYS/dev.sec"
# the ELF-compat test programs
mkdir -p "$W/aether-compat-tests/opt"; cp -a "$P/build/compat-tests" "$W/aether-compat-tests/opt/aether-compat"
install -m 0755 "$P/tests/compat/run-all.sh" "$W/aether-compat-tests/opt/aether-compat/run-all.sh"
"$IMP" package --name aether-compat-tests --version 1.0 --libc "$GLIBC" --summary "ELF-compat test programs" --tree "$W/aether-compat-tests" --out-dir "$REPO" --pm "$PM" --key "$KEYS/dev.sec" \
    --ignore-needs libaethermissing.so.9 --depends "libc6" --depends "libstdc++6" --depends "libgcc-s1" --depends "libc-bin" --depends "aether-glibc-config"
# tiny packages for the runtime package-manager self-test (hello 1.0 -> 1.1, and a library it depends on)
mkdir -p "$W/hello10/usr/bin" "$W/hello11/usr/bin" "$W/hellolib/usr/lib/aether-hello"
printf '#!/bin/sh\necho "hello from package 1.0"\n' > "$W/hello10/usr/bin/aether-hello"; chmod 755 "$W/hello10/usr/bin/aether-hello"
printf '#!/bin/sh\necho "hello from package 1.1"\n' > "$W/hello11/usr/bin/aether-hello"; chmod 755 "$W/hello11/usr/bin/aether-hello"
echo greeting-data > "$W/hellolib/usr/lib/aether-hello/data"
"$IMP" package --name aether-hello --version 1.0-1 --libc none --summary "demo app" --tree "$W/hello10" --out-dir "$REPO" --pm "$PM" --key "$KEYS/dev.sec" >/dev/null
"$IMP" package --name aether-hello --version 1.1-1 --libc none --summary "demo app" --tree "$W/hello11" --out-dir "$REPO" --pm "$PM" --key "$KEYS/dev.sec" --depends "aether-hello-data >= 1.0" >/dev/null
"$IMP" package --name aether-hello-data --version 1.0-1 --libc none --summary "demo data" --tree "$W/hellolib" --out-dir "$REPO" --pm "$PM" --key "$KEYS/dev.sec" >/dev/null
"$PM" index "$REPO" --name aether --key "$KEYS/dev.sec" --timestamp "$(date +%s)"
ls -la "$REPO" | tail -n +2
