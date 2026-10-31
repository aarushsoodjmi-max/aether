#!/bin/bash
# Fetch, verify (SHA-512) and extract pinned build-time deps (config/build-deps.lock) into build/deps. Never touches the host.
set -euo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
LOCK="$PROJECT/config/build-deps.lock"
DEBS="$PROJECT/build/deps/debs"
OUT="$PROJECT/build/deps/root"
mkdir -p "$DEBS"
rm -rf -- "$OUT"; mkdir -p "$OUT"
while read -r name ver sha size url; do
    [[ -z "${name:-}" || "$name" == \#* ]] && continue
    f="$DEBS/${url##*/}"; f="${f//%7e/\~}"; f="${f//%2b/+}"
    if [[ ! -f "$f" ]]; then
        echo "fetch $name $ver"
        curl -fsSL --retry 3 -o "$f.part" "$url"
        mv "$f.part" "$f"
    fi
    echo "$sha  $f" | sha512sum -c --quiet - \
        || { echo "CHECKSUM MISMATCH: $name $ver" >&2; rm -f "$f"; exit 1; }
    dpkg-deb -x "$f" "$OUT"
done < "$LOCK"
echo "build deps extracted: $OUT"
