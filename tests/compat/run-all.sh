#!/bin/sh
# Run the ELF-compat suite inside an Aether rootfs (chroot or QEMU). busybox-sh compatible.
# Prints one PASS/FAIL line per test and "COMPAT SUMMARY pass=N fail=M"; exit 0 iff fail=0.
D=/opt/aether-compat; B=$D/bin; L=/lib64/ld-linux-x86-64.so.2
pass=0; fail=0; out=/tmp/compat.out
ok()  { echo "PASS $1"; pass=$((pass+1)); }
bad() { echo "FAIL $1: $2"; [ -f $out ] && sed 's/^/    | /' $out | head -8; fail=$((fail+1)); }
run() {  # run NAME CMD...   expects rc 0 and "OK NAME" on stdout
    n=$1; shift; "$@" >$out 2>&1; rc=$?
    if [ $rc -eq 0 ] && grep -q "^OK $n" $out; then ok $n; else bad $n "rc=$rc"; fi
}
cd /tmp
for t in hello_dyn signals procs threads fs_paths kernel_ifaces nss_libc cxx_runtime static_glibc needed_so dlopen_plugin; do
    run $t $B/$t
done
AETHER_T='val=with=equals' run args_env $B/args_env
LD_PRELOAD=$D/lib/libpreload.so run preload_check $B/preload_check
$L --library-path $D/lib $B/needed_so >$out 2>&1; rc=$?
if [ $rc -eq 0 ] && grep -q '^OK needed_so' $out; then ok loader_direct; else bad loader_direct "rc=$rc"; fi
# the dynamic loader must report what it resolved
$L --list $B/needed_so >$out 2>&1; rc=$?
if [ $rc -eq 0 ] && grep -q 'libaethertest.so.1 =>' $out && grep -q 'libc.so.6 =>' $out; then ok loader_list; else bad loader_list "rc=$rc"; fi
LD_DEBUG=libs $B/hello_dyn >$out 2>&1; rc=$?
if [ $rc -eq 0 ] && grep -q 'search path=' $out; then ok ld_debug; else bad ld_debug "rc=$rc"; fi
# missing dependency: loader must fail cleanly with 127, not crash or hang
$B/missing_dep >$out 2>&1; rc=$?
if [ $rc -eq 127 ] && grep -q 'cannot open shared object file' $out; then ok missing_dep_fails_cleanly; else bad missing_dep_fails_cleanly "rc=$rc"; fi
echo "COMPAT SUMMARY pass=$pass fail=$fail"
[ $fail -eq 0 ]
