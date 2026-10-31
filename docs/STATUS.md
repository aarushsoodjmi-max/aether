# Aether OS - implementation status

Evidence levels, strongest last. A feature is only listed at a level if a test at that level actually ran.

| Level | Meaning |
|---|---|
| **implemented** | code exists |
| **compiled** | builds clean with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` |
| **unit-tested** | automated unit tests passed (PM and GUI suites run under ASan + UBSan + LeakSanitizer) |
| **integration-tested** | end-to-end tests on real files/sockets/chroots (host kernel) |
| **QEMU-tested** | ran in QEMU (TCG, no KVM) on a real Linux kernel with Aether's init as PID 1 |
| **hardware-tested** | ran on the target laptop. **Nothing has been.** |

**Kernel used for every QEMU result: a prebuilt Ubuntu `7.0.0-38-generic` image (a temporary stand-in, see `kernel.md`).
Aether's own `make kernel` path has still never been run. No result below is real-hardware evidence.**

## Test totals (all re-run on rebuilt images at the end of this session)
| Suite | Result |
|---|---|
| `make test` = core unit + integration (init, service manager, fstab, legacy `aether-pkg`, `aether-net`, mkinitramfs); log has 575 PASS lines, 0 FAIL | all pass |
| PM unit (`tests/test_pm_unit.c`, sanitizers) | 69/69 |
| GUI unit (`tests/test_gui_unit.c`, sanitizers; now includes input translation, pointer/click/focus/drag/shortcut WM tests, terminal emulator incl. 1.28 MB random-byte fuzz, file-manager core) | 296/296 |
| Headless GUI (`make gui-headless-test`) | 98/98 |
| PM integration (`tests/test_pm.sh`): repo flow, upgrade/remove, tamper/replay/untrusted key, symlink + overwrite refusal, unprivileged use, **crash injection at every journal record** (install 18 points, upgrade 22, remove 11) | 53/53 |
| Base boot in QEMU (`make qemu-test`) | 6/6 |
| ELF-compat suite, chroot of the package-installed rootfs (host kernel) | 17/17 |
| ELF-compat suite, **QEMU** (Aether init, glibc loader, package-installed) | 17/17 |
| `aether-pm` running **inside** the QEMU system (`tests/pm-qemu-test.sh`) | 14/14 |
| Desktop in QEMU (`tests/gui/qemu-gui-test.sh`): KMS display, real mouse/keyboard events, screenshots analysed pixel-by-pixel (expectations updated: launcher now lists 3 apps, Settings is the 2nd tile) | 27/27 |
| **Interactive desktop in QEMU** (`make gui-interactive-test`, `tests/gui/qemu_interactive_test.py`): QMP-injected pointer/keys into a PS/2 mouse, a `usb-tablet` and a `virtio-tablet`; corners, launcher, Settings drag/close, workspaces, terminal commands read back from the framebuffer, file manager vs. real `ls` | ps2 60/60, usb 59/59, virtio 59/59 |

## Component matrix
| Component | implemented | compiled | unit | integration | QEMU | hardware |
|---|---|---|---|---|---|---|
| Init, service manager, fstab, `aetherctl`, `aether-net` static | yes | yes | yes | yes | boot only (6 checks) | no |
| Linux ELF compatibility (glibc 2.39 import, FHS usr-merge layout, loader cache) | yes | yes | - | yes (chroot) | **yes** | no |
| `aether-import` (inspect / validate / package ELF software) | yes | n/a (python) | - | exercised by repo build | - | no |
| `.aether` format, `aether-pm` (build, sign, index, verify, update, install, upgrade, remove, check, owner, recover) | yes | yes | yes | yes | **yes** | no |
| Aether compositor / WM / workspaces / panel / launcher / Settings / notifications | yes | yes | yes (wm, gfx, proto, appmeta, keys) | - | **yes** | no |
| KMS dumb-buffer display backend | yes | yes | - | - | **bochs-drm only** | **Intel HD 520 / i915: no** |
| evdev input + hotplug | yes | yes | keymap only | - | **PS/2 keyboard + mouse only** | no |
| `aether-pm` HTTP(S) repositories | yes (shells out to wget/curl) | yes | - | **not tested** | not tested | no |
| Aether's own kernel build (`make kernel`) | yes (pre-existing) | n/a | - | - | **never run** | no |

## Not implemented (do not assume)
- Disk root / `switch_root` / persistent storage; the whole system runs from an initramfs.
- Running **existing Linux GUI applications**: Aether has no X11 or Wayland compatibility layer, so GTK/Qt/browser apps cannot draw. Only CLI-style ELF programs are covered. See `compat.md`.
- Real GPU drivers: the session only loads `bochs` and `virtio-gpu`. i915 needs firmware and more modules; not done.
- Touchpads/I2C-HID, USB input, audio, networking beyond static config, power management.
- Package-manager: install/remove scripts and triggers (by design none yet), config-file preservation, compression, key revocation, index expiry, per-repo key pinning. See `aether-package-manager.md`.
- Aether-native libc, loader, or kernel. The glibc runtime is a temporary upstream import.


## Interactive desktop (mouse fix, terminal, file manager) - evidence
- **Mouse root cause (verified in QEMU):** absolute pointing devices (QEMU `usb-tablet`, `virtio-tablet`) were never supported: the guest had no `hid`/`usbhid`/`virtio_input` modules staged or loaded, and the compositor only understood relative devices, so a tablet pointer (the default in GTK/SDL front ends with `-usb -device usb-tablet`) either did not exist or its absolute coordinates were treated as deltas. Fixed in `input.c` (IN_ABS, single scaling step `input_abs_scale`, per-frame ordering, SYN_DROPPED resync), `compositor.c`, `wm.c` (`wm_pointer_to`, buttons 1-3, wheel, click counts), module staging/loading.
- Verified on 1280x800 bochs-drm with QMP input events for all three device types: all four corners reachable exactly, no sticking when leaving a corner, 1:1 relative scale, 8 random absolute targets exact.
- Terminal (`aether-terminal`, pty + `/bin/sh -i`) and Files (`aether-files`) are QEMU-tested through the launcher; output is verified by decoding the framebuffer and by a SIGUSR1 text dump.
- **Not tested:** a real physical mouse, a GTK/SDL window with a human at the keyboard (the harness injects the same QEMU input events a front end does; nobody sat at a window), touchpads (deliberately refused), i915 hardware, KVM speed.
