#!/bin/bash
# Fetch, verify (SHA-512) and extract the pinned glibc-ABI runtime listed in
# config/compat-runtime.lock into build/compat/{debs,runtime}. Never touches the host.
# The runtime is an UPSTREAM glibc 2.39 import (temporary); see docs/compat.md.
set -euo pipefail
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
LOCK="$PROJECT/config/compat-runtime.lock"
DEBS="$PROJECT/build/compat/debs"
OUT="$PROJECT/build/compat/runtime"
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
echo "compat runtime extracted: $OUT"
