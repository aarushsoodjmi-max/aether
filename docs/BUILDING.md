# Building and testing

Host needs: gcc, make, python3, binutils (`readelf`), `dpkg-deb`, curl, root (integration tests use namespaces/chroot). Optional: Python Pillow
(only for `tools/gen-font.py` and PNG screenshots). Nothing is installed system-wide; everything lands under `build/`.

```
tools/setup-test-env.sh                 # QEMU + prebuilt kernel + modules + zstd/kmod into build/hosttools (isolated; ~1 min)
export PATH="$PWD/build/hosttools/bin:$PATH"

make                                    # all binaries (fetches pinned libsodium into build/deps on first use); strict -Werror
make busybox                            # pinned BusyBox 1.37.0 (needed by integration tests / images)
make test                               # unit (core + PM + GUI, sanitizers) + integration + PM integration   -> exits 0
make qemu-test                          # base image boots, services start, clean poweroff

make compat-initramfs                   # glibc runtime + test programs as signed .aether packages, installed with aether-pm
tests/compat/chroot-test.sh             # ELF-compat suite in a chroot (host kernel)
make compat-qemu-test                   # ELF-compat suite in QEMU
tests/pm-qemu-test.sh                   # aether-pm on the running system (update/install/upgrade/remove/tamper)

make gui-initramfs                      # desktop image (GUI=1 COMPAT=1)
make gui-qemu-test                      # drives the desktop with mouse/keyboard, checks screenshots -> build/gui-shots/*.png
make run  /  tools/run-qemu.sh          # interactive (set INITRD=build/aether-gui-initramfs.cpio.gz for the desktop)
```
Skipped tests exit 77 (no QEMU/kernel). The QEMU suites use TCG, so allow ~20-60 s each.
Reproducibility notes: libsodium and the glibc runtime are pinned by SHA-512 (`config/*.lock`); the QEMU/kernel set from `setup-test-env.sh`
follows the Ubuntu archive and is **not** pinned. The signing key for the dev repository is generated in `build/keys/` and is never put in an image.
