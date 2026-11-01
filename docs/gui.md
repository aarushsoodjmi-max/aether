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
maximize/restore and screen resize), `M_TASK_ACTIVATE` (panel only; `v0 = 1` = raise/restore/focus without ever minimising), `M_TASK_CLOSE` (panel only, id 9, appended last: ask a window to close), `E_TASK` (window list for the panel), `E_HOTKEY` (Super), `E_CONFIGURE.v4 = 1` (resize request),
`E_POINTER.v4` (a press that dismissed a popup, so the panel button toggles instead of reopening), create flags `WF_RESIZABLE | WF_CENTER | WF_TRANSPARENT`.
A popup owns the keyboard while open (launcher typing, arrows, Enter, Esc). Tapping Super alone toggles the launcher.

**Honest scope.** Only the apps that exist are shown: today that is Settings (no terminal or file manager exists, so none is drawn). The panel shows a network
icon only if a non-loopback interface exists in `/sys/class/net` and a battery only if `/sys/class/power_supply/BAT*` exists; there is no audio stack, so no audio icon.
Tooltips, dropdowns and confirmation dialogs do not exist yet; the tokens and primitives (rounded surfaces, shadows, `font_ellipsize`) are ready for them.

## Right-click context menus
A right click (button 3) opens a context menu, a `WK_POPUP` window drawn by `aether-shell --menu X Y KIND ARG` and built exactly like the launcher: the panel or
the desktop surface spawns it, it owns the keyboard while open, and the compositor closes it (`E_CLOSE`) when any button is pressed outside it.

| Right-click on | Menu | Action (performed by the process that owns the click) |
|---|---|---|
| Desktop | Terminal, Files, Settings, separator, Refresh | Opens the application. An entry appears **only if that application is installed and executable** (a stock system ships all three; a system without one simply omits it). Refresh re-reads the `.app` files, re-renders the wallpaper and rebuilds the shortcuts. |
| Desktop shortcut | Open | Starts that application (the right click also selects the shortcut). |
| Task chip (panel) | Activate, Close | Activate: `M_TASK_ACTIVATE v0=1`, restores / raises / focuses and never minimises (a left click on the chip still toggles minimise). Close: `M_TASK_CLOSE`, the compositor sends `E_CLOSE` to the window's client exactly like its title-bar close button; the application decides, so a window is never killed by the menu. |

Mouse: hover or arrows highlight a row, a left click or Enter chooses it, Escape or a click elsewhere closes it. Right-clicking away from an open menu only closes it.
Arrow keys skip the separator and wrap. The menu opens with its corner at the pointer, is pulled back onto the screen at the right edge, opens upwards near the
bottom edge and never covers the panel (a chip menu opens just below the panel).

**How it works.** The model is `ctxmenu.c/.h` (items per kind, command encoding and parsing, placement, keyboard stepping; no sockets or drawing, unit-tested).
The popup inherits a pipe on fd 9, as the launcher does. When a row is chosen it writes one line (`open NAME`, `refresh`, `activate ID`, `close ID`) and exits; the
owner reads it in its event loop, validates it with `cm_parse_command()` (unknown verbs, control characters, oversized or zero / overflowing ids are dropped) and
acts. This is why only the **panel** can address other windows: `M_TASK_ACTIVATE` and `M_TASK_CLOSE` are ignored from any other client (tested with a raw
protocol client), and the menu popup never talks to the compositor about other windows itself.

**Toolkit change (`ui.c`).** Widgets now press and activate on the **primary button only**; before, a right or middle click triggered a button exactly like a left
click. `UiWindow.on_context(w, widget, x, y)` is called when button 3 is pressed and released over the same interactive widget (or over none, `-1`), unless that press
only dismissed a popup (`E_POINTER.v4`). Raw `on_pointer` handlers (Terminal, Files) are unchanged. **Button numbers** in `E_POINTER.v3` and `wm_button_ex` are
1 = left, 2 = middle, 3 = right (the previous comment in `wm.h` had middle and right swapped; the code and the Terminal's middle-click paste always used this mapping).

**Repaint and ticks (`ui.c`).** `ui_redraw` paints into a private buffer and copies only the rows that differ into the shared window buffer, and sends
`M_COMMIT` only when something changed (or for the first frame). The compositor can read a window's buffer at any moment, so painting in place exposed a
cleared, half-drawn window (a full-screen flash of the window colour on the desktop surface) and every click, which repaints twice (press, release), made the
compositor recompose the whole screen for nothing. `ui_run` now fires `on_tick` on the clock instead of only when `poll()` times out: a pointer moving over a popup
delivers events faster than `tick_ms`, which used to freeze the menu / launcher fade at its first frame (nearly transparent, so the menu looked absent). Cost: one
extra window-sized buffer per client (the desktop surface: about 4 MiB at 1280x800). The headless compositor's control socket has a `stats` command
(recomposes and COMMITs so far) that the tests use to prove a click does not repaint the screen.

Not in this milestone: menus for windows' title bars or app content (Terminal and Files have no context menu of their own), submenus, icons or shortcuts in menu rows,
and "Properties/Rename/Delete" style actions: none of those exist in the system, so none are shown.

## Files: icon toolbar and Places sidebar
`aether-files` is the one file manager (`files.c` for the window, `fm_core.c` for everything that touches the filesystem). This pass added two things to it; navigation, listing,
history, preview, the path bar and file/folder opening are the same code as before.

**Icon buttons.** Back and Home are square (32 px) icon buttons with a 20 px glyph from the shared icon family (`ICON_BACK` left arrow, `ICON_HOME` house; Up, Hidden and Terminal stay text).
States use the icon tokens: default, hover, pressed (active) and disabled (Back with no history). The actions are unchanged: Back pops the history, Home opens `$HOME` (or `/` when it is unset),
and Alt+Left still goes back. Each icon button has a tooltip ("Back (Alt+Left)", "Home folder"): it appears after the pointer has rested on the button for 450 ms, is drawn inside the Files
window just below the toolbar, and disappears when the pointer leaves or the button is pressed. Tooltips exist only in Files; there is no toolkit-wide tooltip facility.

**Places sidebar.** A persistent panel on the left, always visible while browsing, with eight locations: Home, Desktop, Documents, Downloads, Pictures, Music, Videos and Root Filesystem
(a small gap and divider set the last one apart). The list, column header and info panel use the space to its right. Width is a quarter of the window clamped to 112-176 px; on a narrow list the
Modified column is dropped before the name is squeezed, and rows that do not fit above the status line are simply not laid out.

| Place | Opens | Icon |
|---|---|---|
| Home | `$HOME` (`/` if unset or empty) | house |
| Desktop, Documents, Downloads, Pictures, Music, Videos | `$HOME/<Name>` | display, page, download tray, picture, notes, video |
| Root Filesystem | `/` | disk |

- **Highlight.** The place that contains the current folder: the most specific match, so `~/Documents/work` keeps Documents highlighted and `~/projects` highlights Home. Everything outside `$HOME` highlights Root Filesystem (with no `$HOME`, only `/` itself counts as Home). Whole path components only (`DocumentsOld` is not inside `Documents`). The highlight is recomputed after every successful navigation, so Back, the path bar and double-clicks keep it right.
- **Unavailable places.** A place is checked when the window opens and after every navigation (for drawing), and **again at click time**. One that is missing, is not a directory or has no permission is drawn dimmed with no hover highlight; clicking it opens nothing, does not move the highlight and shows the system's own reason in the status line (`Pictures is not available: No such file or directory`; the same line is printed on stdout as `aether-files: place Pictures unavailable: ...`). Nothing is ever created: the folders are not made for you.
- **Same navigation path.** A sidebar click calls the same `navigate()` as the toolbar, double-click and path bar, so history, selection reset, status text and the `opened ...` log line are identical. Press-then-release on the same item is required, like the toolbar buttons.
- **Where the logic lives.** `fm_place_label/path/check/for_path`, `fm_home_path` and `fm_sidebar_width` are in `fm_core` (no drawing, unit-tested); the layout, drawing, hover/press state and click handling are in `files.c`. The compositor, window manager, input code and the toolkit's pointer path are untouched: Files still uses its raw `on_pointer` handler.
- **Repaint.** The 200 ms tick repaints only when the path-bar caret changes phase or a tooltip has just become due; an idle window commits nothing.

Tests: `test_fm_places` (31 checks across places/highlight/sidebar width and the new icons) in `tests/test_gui_unit.c`; `files_suite` in `tests/gui/headless_gui_test.py` runs the real
`aether-files` under the headless compositor with a throw-away `$HOME` (Pictures missing, Videos a plain file) at 1280x800 and 800x600, clicking where the application reports its
controls (`aether-files: layout ...` on stdout) and checking the folder it opened, the highlighted item by pixel, the disabled/unavailable behaviour, Back/Home, tooltips and list double-click.
Not covered: QEMU/real evdev input (no new input code was added) and the permission-denied place under a non-root user (skipped when the tests run as root).

## Testing
| Target | What | Where it ran |
|---|---|---|
| `make build/test/test_gui_unit && build/test/test_gui_unit` | 489 checks under ASan/UBSan (Aurora tokens, gfx, fonts, icons, WM, the mouse/window-management regressions below, **plus 59 for context menus: model, command parsing, placement, keyboard, protocol ids, `wm_task_close` / `wm_task_raise`, right-button routing, plus 17 for the toolkit: repaint publishes only finished frames, unchanged repaints send no COMMIT, left/right-click routing, ticks under event load, plus 31 for the Files sidebar/icons: places, highlight, sidebar width, new glyphs**) | sandbox |
| `make gui-headless-test` | 392 end-to-end checks (98 original + 72 real-client mouse/resize checks + 128 right-click context-menu checks, 64 each at 1280x800 and 800x600, + 18 interaction checks: clicks do not repaint, menus and the launcher become fully opaque while the pointer keeps moving, + 76 Files checks, 38 at each of those two sizes): the **real** compositor in `--headless WxH` mode plus the real panel/desktop/launcher/Settings/notify binaries, driven through a control socket and pixel-checked at 1280x800, 1024x768, 800x600, 1920x1080 | sandbox (no QEMU) |
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
Resize cursors (the pointer sprite does not change over a resize grip), multi-line text entry, text selection, a toolkit-wide tooltip facility (only the Files icon buttons have tooltips, drawn by Files itself), dropdowns/dialogs, per-application context menus, submenus, clipboard, drag-and-drop, multi-monitor, HiDPI, fractional scaling, keyboard layouts other than US, IME, screen lock, authentication, multi-user sessions (compositor runs as root), accessibility, sound, damage tracking, vsync/page flipping, and any X11/Wayland compatibility.


## Pointer devices, terminal and files
- Pointer: relative (PS/2/USB mouse) -> `IN_MOTION`; absolute (usb-tablet, virtio-tablet) -> `IN_ABS`, scaled once to the display (`input_abs_scale`: min->0, max->size-1). Touchpads are refused. Interactive use: `make qemu-gui` (`POINTER=tablet|virtio|ps2`, `DISPLAY_BACKEND=gtk|sdl`).
- Shortcuts: Super = launcher, Ctrl+Alt+T = terminal, Alt+Tab = cycle windows, Alt+F4 = close, Alt+1..N = workspace.
- Terminal: pty + BusyBox `ash`; VT100/xterm subset (see `term_vt.h` for exactly what is and is not supported), scrollback, mouse selection (double/triple click), Ctrl+Shift+C/V, middle-click paste. No clipboard shared with other apps.
- Files: browse-only (navigate, info, preview, hidden toggle, path bar, Back/Home icon buttons with tooltips, Places sidebar - see "Files: icon toolbar and Places sidebar"); no copy/move/delete.
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
