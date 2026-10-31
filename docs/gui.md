# Aether GUI

Original desktop; no GNOME/KDE/X11/Wayland code. Separate modules (all in `src/gui/`):

| Module | Files | Role |
|---|---|---|
| Display integration | `display.c` | KMS **dumb buffers** via raw DRM ioctls (no libdrm): connector/CRTC/mode pick, `CREATE_DUMB`, `ADDFB`, `SETCRTC`, `DIRTYFB` after each frame |
| Input integration | `input.c` | evdev keyboards/mice, **hotplug rescan** (needed: `psmouse` probes after the compositor starts), US keymap |
| Rendering | `gfx.c`, `font8x16.c` | clipped fills, blits, gradients, legacy 8x16 bitmap text (kept for compatibility); **Aurora additions**: alpha blending, anti-aliased rounded rects, SDF shadows, rounded/alpha blits, vector strokes |
| Design system | `theme.h` | the only place with colour/geometry/type tokens (Aether Aurora) |
| Typography | `font.c`, `fontdata.c` | anti-aliased proportional Poppins Regular/Medium, pre-rasterised by `tools/gen-font-aa.py` (see `NOTICE-fonts.md`) |
| Icons | `icons.c` | original vector icon family on a 24-unit grid, 5 states, app tiles |
| Wallpaper | `wallpaper.c` | procedural Aurora background for any resolution (no image asset) |
| Window management + compositor | `wm.c`, `compositor.c` | stacking, focus, drag, **minimize / maximize / close** controls, 4 workspaces, layers (desktop < normal < panel < notification < popup), rounded frames + shadows, popup opacity, scene cache with a cursor-only fast path, screen-resize handling |
| Protocol | `proto.c/.h` | `SOCK_SEQPACKET` at `/run/aether/display.sock`; pixels in client `memfd` passed with `SCM_RIGHTS` |
| UI toolkit | `ui.c/.h` | window + shared memory, label/button/switch/separator/**text entry**/custom-draw widgets, hover/press/click/focus, pages, watch fd, resize, event loop |
| Desktop shell | `shell.c` | one binary, three modes: **panel** (launcher button, workspaces, task chips, clock, real network/battery indicators), **launcher** popup (search + grid + keyboard), **desktop** surface (wallpaper + shortcuts) |
| Settings, notifications | `settings.c`, `notify.c` | About / Display / Packages / Notifications / Appearance, resizable; `aether-notify TITLE BODY [TIMEOUT_MS] [info\|success\|warning\|error]` |
| App metadata | `appmeta.c` | `/usr/share/aether/apps/*.app` (`name`, `comment`, `exec`, optional `icon`); only entries whose `exec` exists are shown |

Independent of the package manager: no shared code; the shell only reads `.app` files, Settings only lists the DB directory.


## Aether Aurora desktop
**Design tokens** live in `theme.h` (palette, 4 px spacing scale, geometry, type sizes, animation times) and nowhere else; `tests/gui/gui_layout.py`
parses it so the tests follow the design. Palette, as specified: void `#080D18`, desktop `#0B1220`, panel `#101A2C`, window `#131E31`, secondary `#1C2B43`,
text `#F2F6FF` / `#A7B5CC` / `#73839D`, blue `#398BFF` (hover `#57A0FF`), cyan `#63E5F5`, border `#2A3B54`, success/warning/error `#55D6A0` / `#FFC66D` / `#FF7185`.
Contrast is asserted in the unit tests (primary text 12:1+, dark text on blue 5.9:1 = AA).

**Who draws what.** The compositor draws window frames (38 px title bar, 10 px radius, shadow, blue-tinted rim on the focused window, minimize/maximize/close
with hover/pressed states), rounds and shadows popups/notifications, and shows a plain void-to-desktop gradient only as a fallback. The **desktop surface**
(`aether-shell --desktop`, protocol kind `WK_DESKTOP`) is an ordinary opaque client window pinned under the panel: it renders the Aurora wallpaper at the
real display size and the shortcuts on top of it, so shortcuts can never appear above a window and no per-pixel alpha is needed in the protocol.
If it crashes, the fallback gradient shows and everything else keeps working.

**Protocol additions** (append-only; all existing messages unchanged): `M_SET_OPACITY` (fades), `M_SET_BUFFER` (client swaps in a new sealed memfd: used by
maximize/restore and screen resize), `M_TASK_ACTIVATE` (panel only), `E_TASK` (window list for the panel), `E_HOTKEY` (Super), `E_CONFIGURE.v4 = 1` (resize request),
`E_POINTER.v4` (a press that dismissed a popup, so the panel button toggles instead of reopening), create flags `WF_RESIZABLE | WF_CENTER | WF_TRANSPARENT`.
A popup owns the keyboard while open (launcher typing, arrows, Enter, Esc). Tapping Super alone toggles the launcher.

**Honest scope.** Only the apps that exist are shown: today that is Settings (no terminal or file manager exists, so none is drawn). The panel shows a network
icon only if a non-loopback interface exists in `/sys/class/net` and a battery only if `/sys/class/power_supply/BAT*` exists; there is no audio stack, so no audio icon.
Context menus, tooltips, dropdowns and confirmation dialogs do not exist yet; the tokens and primitives (rounded surfaces, shadows, `font_ellipsize`) are ready for them.

## Testing
| Target | What | Where it ran |
|---|---|---|
| `make build/test/test_gui_unit && build/test/test_gui_unit` | 382 checks under ASan/UBSan (Aurora tokens, gfx, fonts, icons, WM, **plus the mouse/window-management regressions below**) | sandbox |
| `make gui-headless-test` | 170 end-to-end checks (98 original + 72 real-client mouse/resize checks at 1280x800 and 800x600): the **real** compositor in `--headless WxH` mode plus the real panel/desktop/launcher/Settings/notify binaries, driven through a control socket and pixel-checked at 1280x800, 1024x768, 800x600, 1920x1080 | sandbox (no QEMU) |
| `make gui-qemu-test` | the original 27 QEMU checks (same order and log patterns), updated to the new design | **not run by the author of this change: no QEMU/kernel in that environment** |

The headless mode exists only when `--headless` is passed; the KMS desktop never opens the control socket. Headless runs exercise the compositor, WM, protocol and clients,
not the kernel, initramfs, DRM scanout or evdev. Screenshots in `docs/screenshots/aurora-headless/` come from the headless runs; the older images in `docs/screenshots/` are real QEMU captures of the pre-Aurora desktop.

## Verified before Aurora (QEMU, std VGA, bochs-drm, 1280x800)
Session loads `psmouse` + `bochs`, compositor takes the display (log: `driver=bochs-drm`), panel/clock/cursor/gradient render, real mouse events
open the launcher menu, start Settings, switch its pages, switch workspaces (and Alt+1), show and auto-dismiss a notification, and close a window from its title bar.
Screenshots: `docs/screenshots/` (taken by the test, 1280x800).

Footprint measured inside a 256 MiB VM (pre-Aurora; headless Aurora figures in the test output: compositor ~17 MiB RSS, panel ~1 MiB, desktop surface ~8 MiB): compositor 5.4 MiB RSS (incl. 4 MiB back buffer), shell 1.0 MiB, Settings 1.3 MiB; ~125 MiB free with the whole initramfs in RAM.

## Safety properties of the compositor
Validates sizes (<= 4096x4096) and window counts; maps client buffers read-only; **requires `F_SEAL_SHRINK|F_SEAL_GROW`** so a client cannot SIGBUS the compositor by truncating its buffer; closes stray fds; the socket is mode 0600.

## Hardware status - read this
- **Intel HD 520 / i915 is untested.** The dumb-buffer path is driver-agnostic and i915 supports it, but i915 on a real laptop needs the module, firmware (DMC/GuC) and probably more dependencies. Not staged, not tried.
- Only `bochs` and `virtio-gpu` modules are staged; only bochs was run.
- Input verified: PS/2 keyboard and mouse in QEMU. Laptop touchpads are usually I2C-HID/PS/2 variants and need other modules: untested.
- Rendering is 100% CPU. A scene change recomposes the whole screen; moving the pointer only redraws the cursor rectangle. Fades are alpha blends of one popup, not a general animation system.

## Not implemented
Resize cursors (the pointer sprite does not change over a resize grip), multi-line text entry, text selection, context menus/tooltips/dropdowns/dialogs, clipboard, drag-and-drop, multi-monitor, HiDPI, fractional scaling, keyboard layouts other than US, IME, screen lock, authentication, multi-user sessions (compositor runs as root), accessibility, sound, damage tracking, vsync/page flipping, and any X11/Wayland compatibility.


## Pointer devices, terminal and files
- Pointer: relative (PS/2/USB mouse) -> `IN_MOTION`; absolute (usb-tablet, virtio-tablet) -> `IN_ABS`, scaled once to the display (`input_abs_scale`: min->0, max->size-1). Touchpads are refused. Interactive use: `make qemu-gui` (`POINTER=tablet|virtio|ps2`, `DISPLAY_BACKEND=gtk|sdl`).
- Shortcuts: Super = launcher, Ctrl+Alt+T = terminal, Alt+Tab = cycle windows, Alt+F4 = close, Alt+1..N = workspace.
- Terminal: pty + BusyBox `ash`; VT100/xterm subset (see `term_vt.h` for exactly what is and is not supported), scrollback, mouse selection (double/triple click), Ctrl+Shift+C/V, middle-click paste. No clipboard shared with other apps.
- Files: browse-only (navigate, info, preview, hidden toggle, path bar); no copy/move/delete.
- Run `make gui-interactive-test` for the QEMU pointer/keyboard scenario in all three pointer modes.


## Window dragging, edge resize and hit testing
**Hit testing only covers what is painted.** `wm.c: hit()` is the one place that decides what is under the pointer, and it follows the compositor's drawing rules:
popups/notifications with opacity 0 (a launcher mid fade) are not hit; frames, popups and notifications are hit through their *rounded* shape (same 50 %-coverage
pixel test as the rasteriser), so the unpainted corner pixels belong to whatever is behind; minimised and other-workspace windows have no region; a lower window's
grips are not reachable through a window above it. `wm_hit_test()` exposes the same function for tests.

**Edge resize** (windows created with `WF_RESIZABLE`; not while maximised). Eight grips: 5 px bands (`WM_RESIZE_BAND`) on the inside of the frame plus its 1 px painted
outline, 14 px corners (`WM_RESIZE_CORNER`). The title-bar controls win where they overlap a grip. The compositor owns the pointer during a resize (the client sees no
pointer events). It never resizes the client's pixels itself: the pointer decides a wanted rectangle, the compositor asks for a buffer of that size with the existing
`E_CONFIGURE v4=1`, and the window changes shape when the client answers with `M_SET_BUFFER`. One request is in flight; the newest wanted size follows each reply,
and releasing the button flushes the final size. The edges opposite the dragged ones stay fixed even if the client answers with another size. Limits: `WM_MIN_W x WM_MIN_H`
(320x200; a window already smaller keeps its own size as the floor), never past the screen edge, title bar never under the panel. A request the client never
answers expires after `WM_RESIZE_TIMEOUT_MS`, so a hung client cannot lock a window (this also protects maximise/restore).

**Interaction never outlives its window.** Minimising, removing, switching workspace or changing the screen size cancels any drag, resize, button press or pointer
grab on the affected window (previously a minimised window kept following the pointer).

**Not changed on purpose:** the drag policy (title bar stays reachable: >= 80 px horizontally, never above the panel, never below `screen height - title`).
