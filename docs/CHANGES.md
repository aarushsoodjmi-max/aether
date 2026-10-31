# Change report

Starting point: the supplied `aether-updated` project (init/service manager, legacy `.apkg` manager, static BusyBox initramfs). Its baseline was
reproduced first: clean `-Werror` build, 157 passing checks, and (new) a first real QEMU boot.

## What was added
| Area | New |
|---|---|
| Test environment | `tools/setup-test-env.sh` (QEMU, kernel, modules, zstd/kmod, all project-local), `tests/qemu-boot-test.sh` now actually runs |
| ELF compatibility | usr-merged rootfs layout; pinned glibc 2.39 runtime (`config/compat-runtime.lock`, `tools/fetch-compat-runtime.sh`); `tools/aether-import`; `tools/build-compat-repo.sh`; 14 test programs + runner (`tests/compat/`); `docs/compat.md` |
| Package manager | `src/pm/` (9 modules, ~2.1 kLOC): format v1, signed index, resolver, journaled transactions, DB; `tests/test_pm_unit.c`, `tests/test_pm.sh`, `tests/pm-qemu-test.sh`; `docs/aether-package-manager.md` |
| GUI | `src/gui/` (compositor, WM, display, input, toolkit, shell, settings, notify, app metadata); `tests/test_gui_unit.c`, `tests/gui/qemu_gui_test.py`; `docs/gui.md`, screenshots |
| Build | Makefile targets `pm-test gui gui-initramfs gui-qemu-test compat-*`; `tools/build-rootfs.sh` options `COMPAT=1`, `GUI=1`; pinned `config/build-deps.lock` (libsodium) |
| Docs | `STATUS.md` (rewritten, evidence matrix), `architecture-v2.md`, `compat.md`, `aether-package-manager.md`, `gui.md`, `kernel.md`, `BUILDING.md` |

Existing behaviour preserved: base image still boots (6/6 QEMU checks), all 157 original checks pass. The base `/bin`, `/sbin` are now symlinks to `/usr/*` (usr-merge) - the only layout change.

## Defects found by the tests and fixed (not hypothetical)
1. Package DB stored one directory per package; removal left an empty directory that the loader called corrupt (found by integration tests + crash injection). Now `db/NAME.meta`.
2. Directory removal inside a transaction could not succeed (backups still in the directory) -> emptied directories leaked. Now journaled post-commit removal, replayed by roll-forward.
3. UBSan: NULL `memcpy`/`qsort` on empty sets in the resolver. Fixed.
4. LeakSanitizer / ASan found test-fixture leaks and a lifetime rule (a `Plan` borrows from its `Universe`); documented in `resolve.h`.
5. `waitfor` helper returned failure right after success (harness bug) - fixed; QEMU tests were not trustworthy before that.
6. Screen stayed black: `bochs-drm` needs `DRM_IOCTL_MODE_DIRTYFB` after writing a dumb buffer.
7. No mouse: Ubuntu builds `psmouse` as a module -> session loads it; and it probes after the compositor starts -> compositor now hot-plugs input devices.
8. GUI test harness: busybox image has no `dd` symlink; services' stdout is not on the console (session now redirects to `/dev/console`).

## Open items, in priority order
1. **Run `make kernel` and boot the project's own kernel** (and build GPU/input drivers in). Nothing here is hardware-evidence.
2. **Real-hardware bring-up on the HD 520 laptop**: i915 + firmware, touchpad/keyboard drivers, storage; then disk root + `switch_root`.
3. Wayland (or X11) compatibility layer so existing Linux GUI applications can draw; then try representative apps (the compat claim is still limited to small CLI-style programs).
4. Build glibc from source into native `.aether` packages; add a loader-cache trigger; mirror the pinned upstream archives.
5. Package manager: maintainer-script/trigger design, config-file handling, key revocation + index expiry + per-repo key binding, HTTP(S) transport tests, retire `aether-pkg`.
6. Compositor: privilege separation (not root), damage tracking/vsync, resize, text-entry widget, more input layouts.

## Aether Aurora desktop (GUI redesign)
Replaced the plain gradient+panel desktop with the designed Aurora desktop: design tokens (`theme.h`), anti-aliased Poppins text, original icon family, procedural wallpaper,
desktop-surface client with shortcuts, 38 px panel with task chips, launcher with search/grid/keyboard/Super, rounded shadowed windows with minimize/maximize/close,
resizable Settings with five pages, level-styled notifications with fades, cursor-only redraw fast path, `--headless` compositor mode and a pixel-checked end-to-end test.
Fixed along the way: notification stacking assumed equal heights (overlap), now stacked by real height with reflow. Existing GUI unit checks all kept (two colour literals now reference theme tokens).
Details, limits and test status: `docs/gui.md`.


## Interactive desktop pass (mouse, terminal, file manager)
- **Input**: `src/gui/input.{h,c}` rewritten (relative + absolute devices, frame semantics, hotplug, SYN_DROPPED); `wm.c` gained `wm_pointer_to`, buttons 1-3, wheel, click counting, Alt+Tab, Caps Lock, Ctrl+Alt+T, workspace leave/enter fixes; `compositor.c` dispatches the new events and logs window/focus state; `config/gui/aether-session` + `tools/stage-gui-modules.sh` load/stage `hid usbhid hid-generic virtio_input`.
- **Apps**: `aether-terminal` (`terminal.c`, `term_vt.c`), `aether-files` (`files.c`, `fm_core.c`), `config/apps/{terminal,files}.app`, launcher running-indicator (resolved `/proc/PID/exe`), `config/fstab` devpts.
- **Build/test**: Makefile (`gui-interactive-test`, `qemu-gui`), `tools/run-qemu-gui.sh`, `tests/gui/qemu_session.py`, `tests/gui/qemu_interactive_test.py`, 223 new GUI unit checks. Existing tests kept; only `qemu_gui_test.py` expectations were adapted (3 launcher apps instead of 1).
