# Aether Bootstrap Plan

## Goal
Build an independent userspace and software ecosystem
for Aether, using the Linux kernel.

## Bootstrap stages
1. Use host build tools to compile initial components.
2. Establish a controlled target sysroot.
3. Build the target C library and compiler toolchain.
4. Build a minimal userspace from pinned source releases.
5. Boot and test the resulting system in QEMU.
6. Gradually replace temporary components with Aether implementations.

## Initial decisions
- Architecture: x86_64
- Kernel: Linux
- Initial C library direction: glibc
- Initial build environment: host GCC and Make
- Test environment: QEMU
- Host OS must remain untouched.

## Build requirements
- Pin source versions and checksums.
- Keep source, build outputs, and target rootfs separate.
- Log build failures.
- Never install target files directly onto the host.

## Build procedure (current)

```
make                # Aether binaries, static, -Wall -Wextra -Werror (build/bin)
make busybox        # pinned BusyBox 1.37.0 (checksum-verified) -> build/bin/busybox
make test           # unit + integration tests (integration needs root)
make initramfs      # build/aether-initramfs.cpio.gz (deterministic)
make kernel KERNEL_SRC=/path/to/linux   # or KERNEL_TARBALL=...; see tools/build-kernel.sh
make qemu           # boot it;  make qemu-test for the automated boot test
```

Host requirements: gcc, make, bzip2, gzip, python3 (tests only), and for the
integration test root plus `unshare`/`chroot`. No `cpio` binary is needed:
`tools/mkinitramfs.c` writes the archive. Source tarballs are pinned in
`checksums/sources.sha256`; the BusyBox configuration is `config/busybox.config`.
