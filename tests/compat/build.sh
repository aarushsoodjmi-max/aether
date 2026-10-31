#!/bin/bash
# Build the ELF-compat test programs into build/compat-tests/{bin,lib} with strict warnings.
# Layout mirrors the installed layout /opt/aether-compat/{bin,lib}; executables use
# RUNPATH $ORIGIN/../lib so library resolution is exercised without LD_LIBRARY_PATH.
set -euo pipefail
P="$(cd "$(dirname "$0")/../.." && pwd)"; S="$P/tests/compat/src"; O="$P/build/compat-tests"
rm -rf "$O"; mkdir -p "$O/bin" "$O/lib"
W="-std=gnu11 -O1 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Werror"
WX="-std=gnu++17 -O1 -Wall -Wextra -Werror"
RP='-Wl,-rpath,$ORIGIN/../lib'
CC="${CC:-gcc}"; CXX="${CXX:-g++}"
$CC $W -fPIC -shared -Wl,-soname,libaethertest.so.1 -o "$O/lib/libaethertest.so.1" "$S/libaethertest.c"
ln -s libaethertest.so.1 "$O/lib/libaethertest.so"
$CC $W -fPIC -shared -o "$O/lib/libplugin.so" "$S/libplugin.c"
$CC $W -fPIC -shared -nostartfiles -o "$O/lib/libpreload.so" "$S/libpreload.c"
$CC $W -fPIC -shared -Wl,-soname,libaethermissing.so.9 -o "$O/libaethermissing.so.9" "$S/missing_dep_lib.c"
for t in hello_dyn args_env signals procs threads fs_paths kernel_ifaces nss_libc preload_check; do
    $CC $W -pthread "$RP" -o "$O/bin/$t" "$S/$t.c" -lm
done
$CC $W "$RP" -o "$O/bin/needed_so" "$S/needed_so.c" -L"$O/lib" -laethertest
$CC $W "$RP" -o "$O/bin/dlopen_plugin" "$S/dlopen_plugin.c"
$CC $W "$RP" -o "$O/bin/missing_dep" "$S/missing_dep.c" -L"$O" -l:libaethermissing.so.9
rm -f "$O/libaethermissing.so.9"            # deliberately NOT shipped: loader must fail cleanly
$CXX $WX "$RP" -pthread -o "$O/bin/cxx_runtime" "$S/cxx_runtime.cpp"
$CC $W -static -o "$O/bin/static_glibc" "$S/static_glibc.c"
echo "compat tests built: $O"
