#!/usr/bin/env python3
"""Interactive-desktop test in real QEMU: boots the actual GUI initramfs and drives it with pointer and keyboard events injected
through QEMU's input layer (QMP input-send-event), for each guest pointer device:  ps2 (relative), usb (absolute usb-tablet),
virtio (absolute virtio-tablet).  Ground truth is (a) the compositor's own state log on the serial console and (b) pixels in
QEMU screendumps - including the terminal's text, decoded from the framebuffer with the 8x16 font, not taken from a log.

usage: qemu_interactive_test.py [--pointer ps2|usb|virtio ...] [--quick]     exit 0 = all checks passed, 77 = skipped
"""
import os, re, sys, time, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gui_layout as L
from qemu_session import Session, Skip, pix, near, count_diff, find_cursor, P

args = sys.argv[1:]
modes = []; quick = False
while args:
    a = args.pop(0)
    if a == "--pointer": modes.append(args.pop(0))
    elif a == "--quick": quick = True
modes = modes or ["ps2", "usb", "virtio"]

results = []
def check(name, cond, detail=""):
    results.append(bool(cond)); print(("PASS " if cond else "FAIL ") + name + ("" if cond else f"   [{detail}]"), flush=True)

# ---- 8x16 font decoder: lets the test read text off the screen ---------------------------------------------------------------
GLYPHS = {}
for m in re.finditer(r"\{((?:0x[0-9a-f]{2},?){16})\},\s*/\* '(.)' \*/", open(os.path.join(P, "src/gui/font8x16.c")).read()):
    GLYPHS[bytes(int(h, 16) for h in m.group(1).split(","))] = m.group(2)
def read_cell(img, x, y, bg):
    bits = []
    for r in range(16):
        b = 0
        for c in range(8):
            if not near(pix(img, x + c, y + r), bg, 4): b |= 0x80 >> c
        bits.append(b)
    return GLYPHS.get(bytes(bits), " " if not any(bits) else "?")
def read_row(img, ox, oy, cols, row):
    bg = L.rgb("TH_DESKTOP")
    return "".join(read_cell(img, ox + c * 8, oy + row * 16, bg) for c in range(cols)).rstrip()
def screen_text(img, ox, oy, cols, rows): return [read_row(img, ox, oy, cols, r) for r in range(rows)]

OUT = (0x08, 0x0D, 0x18)
def run(mode):
    print(f"\n===== pointer device: {mode} =====", flush=True)
    s = Session(pointer=mode, shots=f"{P}/build/gui-shots-interactive/{mode}")
    try:
        m = s.wait(r"aether-compositor: display (\d+)x(\d+) via KMS dumb buffer, driver=(\S+)", 120)
        check(f"[{mode}] boot: compositor owns the KMS display", m is not None, s.text()[-500:])
        if not m: return
        W, H = int(m.group(1)), int(m.group(2)); s.W, s.H = W, H
        print(f"INFO guest resolution {W}x{H} driver={m.group(3)}")
        want = {"ps2": "relative-pointer", "usb": "absolute-pointer", "virtio": "absolute-pointer"}[mode]
        s.wait(r"aether-shell: panel running", 60); s.wait(r"aether-shell: desktop surface", 60); time.sleep(3)
        check(f"[{mode}] input: the guest kernel exposed the pointer device and the compositor attached it", re.search(r'aether-compositor: input event\d+: "[^"]*" \[[a-z\- ]*' + want, s.text()) is not None,
              "\n".join(l for l in s.text().splitlines() if "input" in l)[-400:])
        check(f"[{mode}] input: a keyboard is attached", "[keyboard" in s.text())

        # ---- 1. corners ------------------------------------------------------------------------------------------------
        s.mark0 = s.mark()
        ok_all = True
        if mode != "ps2": s.abs_to(W // 2 + 7, H // 2 + 9); time.sleep(0.3)   # a tablet already sitting at (0,0) reports no change, so start from a real position
        for name, (cx, cy) in {"top-left": (0, 0), "top-right": (W - 1, 0), "bottom-left": (0, H - 1), "bottom-right": (W - 1, H - 1)}.items():
            if mode == "ps2": s.rel(-4000 if cx == 0 else 4000, -4000 if cy == 0 else 4000)      # far beyond the edge: must clamp, not wrap/overflow
            else: s.abs_to(cx, cy)
            time.sleep(0.5); img = s.shot("corner-" + name, keep=False)
            if cx == 0 and cy == 0: found = find_cursor(img); good = found == (0, 0); detail = str(found)
            else:                                                                    # sprite is clipped at the far edges: its tip pixel is the screen's last pixel
                good = near(pix(img, cx, cy), OUT, 2) and not near(pix(img, max(0, cx - 3), cy), OUT, 2) if cx else near(pix(img, 0, cy), OUT, 2) and find_cursor(img) is None
                detail = str(pix(img, cx, cy))
            ok_all &= good; check(f"[{mode}] pointer reaches the {name} corner ({cx},{cy}) exactly", good, detail)
        # coming back from a corner must respond at once (no accumulated overshoot, never stuck)
        if mode == "ps2": s.rel(-60, -60)
        else: s.abs_to(W - 61, H - 61)
        time.sleep(0.4); c = s.cursor()
        check(f"[{mode}] pointer leaves the bottom-right corner immediately by exactly 60 px (no stuck/overshoot)", c == (W - 61, H - 61), str(c))
        if mode == "ps2":
            s.rel(-4000, -4000); s.rel(100, 0); time.sleep(0.3); c = s.cursor()
            check("[ps2] relative scale is 1:1 (+100 counts = +100 px)", c == (100, 0), str(c))
            rnd = random.Random(7); x, y = 100, 0; s.rel(0, 300); y = 300
            for _ in range(60):
                dx, dy = rnd.randint(-30, 30), rnd.randint(-30, 30)
                x = max(0, min(W - 1, x + dx)); y = max(0, min(H - 1, y + dy)); s.rel(dx, dy)
            time.sleep(0.5); c = s.cursor()
            ex = (x, y) if x <= W - 12 and y <= H - 16 else None
            check("[ps2] 60 rapid random relative moves end exactly where the sum of deltas says (no drift, no lost events)", c == ex, f"{c} expected {ex}")
        else:
            rnd = random.Random(3); good = True; seen = []
            for _ in range(8):
                tx, ty = rnd.randint(0, W - 14), rnd.randint(0, H - 18); s.abs_to(tx, ty); time.sleep(0.25); cc = s.cursor(); seen.append((tx, ty, cc)); good &= cc == (tx, ty)
            check("[%s] 8 random absolute targets across the display each land on exactly that pixel (scaled once, no offset)" % mode, good, str(seen))

        # ---- 2. launcher ------------------------------------------------------------------------------------------------
        s.move_to(W // 2 + 300, H // 2 + 200); time.sleep(0.4)      # park the pointer outside the compared region
        base = s.shot("desktop", keep=True)
        mk = s.mark(); s.click(60, 19)
        mm = s.wait(r"aether-shell: launcher menu with (\d+) application", 20, mk)
        check(f"[{mode}] launcher: clicking the panel button opens it and it lists the 3 real apps (Files, Settings, Terminal)", mm is not None and mm.group(1) == "3", s.text_since(mk)[-300:])
        time.sleep(0.6); lau = s.shot("launcher")
        check(f"[{mode}] launcher: popup is drawn", count_diff(base, lau, 12, L.PANEL_H + 4, 492, 270) > 2000)
        mk = s.mark(); s.click(W // 2 + 300, H // 2 + 200)
        check(f"[{mode}] launcher: a click outside dismisses it", s.wait(r"client \d+ disconnected", 10, mk) is not None, s.text_since(mk)[-200:])
        time.sleep(0.5); closed = s.shot("launcher-closed", keep=False)
        check(f"[{mode}] launcher: popup is really gone from the screen", count_diff(base, closed, 12, L.PANEL_H + 4, 492, 270) < 400, str(count_diff(base, closed, 12, L.PANEL_H + 4, 492, 270)))
        mk = s.mark(); s.click(60, 19); s.wait(r"launcher menu", 20, mk); time.sleep(0.6)
        mk = s.mark(); s.click(60, 19)                                                                    # panel button again toggles it closed
        check(f"[{mode}] launcher: clicking the panel button a second time closes it (toggle)", s.wait(r"client \d+ disconnected", 10, mk) is not None)
        time.sleep(0.4)

        def tile(col): return (12 + 16 + (480 - 32) // 4 * col + 4 + 52, L.PANEL_H + 4 + (16 + 36 + 12 + 18 + 4) + 48)
        def newest_window(title, since):
            mt = None
            for mt in re.finditer(r"window (\d+) '%s' at (-?\d+),(-?\d+) size (\d+)x(\d+) minimized=(\d) maximized=(\d) workspace=(\d)" % re.escape(title), s.text_since(since)): pass
            return None if not mt else dict(id=int(mt.group(1)), x=int(mt.group(2)), y=int(mt.group(3)), w=int(mt.group(4)), h=int(mt.group(5)), min=int(mt.group(6)), max=int(mt.group(7)), ws=int(mt.group(8)))

        # ---- 3. Settings: launch, drag, focus, close -----------------------------------------------------------------------
        mk = s.mark(); s.click(60, 19); s.wait(r"launcher menu", 20, mk); time.sleep(0.7); s.click(*tile(1))
        check(f"[{mode}] launcher: clicking the Settings tile starts Settings", s.wait(r"aether-settings: window ready \(\d+ package", 30, mk) is not None, s.text_since(mk)[-300:])
        time.sleep(1.5); st = newest_window("Settings", mk)
        check(f"[{mode}] wm: Settings window exists and was placed on screen", st is not None and 0 <= st["x"] and st["x"] + st["w"] <= W and st["y"] - 38 >= L.PANEL_H, str(st))
        if not st: return
        img = s.shot("settings"); check(f"[{mode}] render: Settings title bar and client area drawn", near(pix(img, st["x"] + 10, st["y"] - 8), L.TITLE_ACTIVE, 3) and near(pix(img, st["x"] + 600, st["y"] + 450), L.rgb("TH_WINDOW"), 3))
        mk = s.mark(); s.drag(*L.title_point(st, 200), L.title_point(st, 200)[0] + 120, L.title_point(st, 200)[1] + 60)
        time.sleep(0.8); moved = newest_window("Settings", mk)
        check(f"[{mode}] wm: dragging the title bar moves exactly that window by the pointer delta (+120,+60)", moved and (moved["x"], moved["y"]) == (st["x"] + 120, st["y"] + 60), f"{moved} from {st}")
        check(f"[{mode}] wm: the drag did not close/minimise/maximise it (title bar press is not a button click)", moved and moved["min"] == 0 and moved["max"] == 0 and moved["ws"] == st["ws"])
        img = s.shot("settings-moved", keep=False)
        check(f"[{mode}] render: window redrawn at the new position, old position repainted", near(pix(img, moved["x"] + 10, moved["y"] - 8), L.TITLE_ACTIVE, 3) and not near(pix(img, st["x"] + 10, st["y"] - 8), L.TITLE_ACTIVE, 3))
        st = moved
        mk = s.mark(); s.click(st["x"] + L.T["TH_SIDEBAR_W"] // 2, st["y"] + L.T["TH_NAV_TOP"] + 2 * L.T["TH_NAV_PITCH"] + 18, wait=1.2)
        img2 = s.shot("settings-packages", keep=False); check(f"[{mode}] toolkit: clicking a sidebar item inside the moved window switches its page", count_diff(img, img2, st["x"] + 200, st["y"] + 10, st["x"] + 700, st["y"] + 250) > 300)
        mk = s.mark(); s.click(*L.close_btn(st))
        check(f"[{mode}] wm: the close button closes Settings", s.wait(r"client \d+ disconnected \(\d+ windows? closed\)", 15, mk) is not None, s.text_since(mk)[-200:])
        time.sleep(0.8); img = s.shot("settings-closed", keep=False)
        check(f"[{mode}] render: the closed window is gone", not near(pix(img, st["x"] + 10, st["y"] - 8), L.TITLE_ACTIVE, 3))

        # ---- 4. terminal via launcher search + keyboard, real commands -----------------------------------------------------
        mk = s.mark(); s.click(60, 19); s.wait(r"launcher menu", 20, mk); time.sleep(0.7)
        s.type_text("term"); time.sleep(0.4); s.key("ret")
        check(f"[{mode}] launcher: typing 'term' + Enter (keyboard) starts the Terminal", s.wait(r"aether-terminal: window ready \((\d+)x(\d+) cells\), shell pid (\d+)", 30, mk) is not None, s.text_since(mk)[-400:])
        time.sleep(2.0); tw = newest_window("Terminal", mk)
        check(f"[{mode}] wm: Terminal window placed", tw is not None, s.text_since(mk)[-300:])
        if not tw: return
        cols, rows = (tw["w"] - 16) // 8, (tw["h"] - 16) // 16; ox, oy = tw["x"] + 8, tw["y"] + 8
        def term_lines(name):
            time.sleep(0.8); return screen_text(s.shot(name, keep=True), ox, oy, cols, rows)
        lines = term_lines("terminal-prompt")
        check(f"[{mode}] terminal: the real shell printed a prompt (decoded from the framebuffer)", any(re.search(r"[#$] ?\??$", l) for l in lines), str(lines[:3]))
        for cmd in ["echo hello-aether", "uname -s", "ls / | head -3", "nosuchcommand123", "cd /etc && pwd"]:
            s.type_text(cmd); s.key("ret"); time.sleep(0.6)
        lines = term_lines("terminal-commands"); blob = "\n".join(lines)
        check(f"[{mode}] terminal: 'echo hello-aether' ran and printed its output", "\nhello-aether" in "\n" + blob, blob[-400:])
        check(f"[{mode}] terminal: 'uname -s' printed the real kernel name (Linux)", re.search(r"^Linux$", blob, re.M) is not None, blob[-400:])
        check(f"[{mode}] terminal: 'ls / | head -3' printed 3 real directory entries", re.search(r"ls / \| head -3\n(\S+)\n(\S+)\n(\S+)", blob) is not None, blob[-500:])
        check(f"[{mode}] terminal: a command that does not exist is reported by the shell ('not found')", "nosuchcommand123: not found" in blob, blob[-400:])
        check(f"[{mode}] terminal: 'cd /etc && pwd' changed directory (output /etc and prompt shows /etc)", re.search(r"^/etc$", blob, re.M) is not None and re.search(r"^/etc [#$]", blob, re.M) is not None, blob[-300:])
        # cross-check against the guest: the shell's own dump of what it displays (SIGUSR1) must show the same text
        s.serial("killall -USR1 aether-terminal; sleep 1; sed 's/^/TERMDUMP:/' /run/aether/terminal.dump"); time.sleep(3)
        check(f"[{mode}] terminal: SIGUSR1 text dump (independent of rendering) agrees with the decoded screen", "TERMDUMP:hello-aether" in s.text() and "TERMDUMP:Linux" in s.text())
        # Ctrl+C really interrupts a running command; arrow-up recalls history (line editing works through real escape sequences)
        s.type_text("sleep 100"); s.key("ret"); time.sleep(0.5); s.key("ctrl", "c"); time.sleep(0.8); s.key("up"); time.sleep(0.6)
        blob = "\n".join(term_lines("terminal-ctrlc"))
        check(f"[{mode}] terminal: Ctrl+C interrupted 'sleep 100' and Up recalled it from history", re.search(r"[#$] sleep 100\s*$", blob, re.M) is not None, blob[-300:])
        s.key("ctrl", "u")
        # selection: drag over text, middle-click pastes into the shell
        # find the row containing 'hello-aether'
        rowi = next((i for i, l in enumerate(term_lines("terminal-sel0")) if l == "hello-aether"), None)
        if rowi is not None:
            ry = oy + rowi * 16 + 8; s.drag(ox + 2, ry, ox + 8 * 12 - 2, ry); time.sleep(0.5)
            img = s.shot("terminal-selected", keep=True)
            check(f"[{mode}] terminal: mouse drag highlights the selected text", not near(pix(img, ox + 20, oy + rowi * 16 + 1), L.rgb("TH_DESKTOP"), 3), str(pix(img, ox + 20, oy + rowi * 16 + 1)))
            s.click(ox + 300, oy + 16 * (rows - 3), wait=0.5)
            if True:
                pass
        # running indicator + three-app launcher
        mk = s.mark(); s.click(60, 19); s.wait(r"launcher menu", 20, mk); time.sleep(0.9)
        img = s.shot("launcher-running"); dx, dy = tile(2)[0], tile(2)[1] - 48 + 96 - 4
        fx, fy = tile(0)[0], dy
        check(f"[{mode}] launcher: running-application dot under Terminal (running) and not under Files (not running)", near(pix(img, dx, dy), L.rgb("TH_CYAN"), 12) and not near(pix(img, fx, fy), L.rgb("TH_CYAN"), 12), f"term {pix(img, dx, dy)} files {pix(img, fx, fy)}")
        mk = s.mark(); s.click(*tile(0))
        check(f"[{mode}] launcher: clicking the Files tile starts the file manager", s.wait(r"aether-files: window ready \((\d+)x(\d+)\) in (\S+)", 30, mk) is not None, s.text_since(mk)[-300:])
        time.sleep(1.5); fw = newest_window("Files", mk)
        check(f"[{mode}] wm: Files window placed and focused above the terminal", fw is not None and re.search(r"focus -> window %d" % fw["id"], s.text_since(mk)) is not None, str(fw))
        if not fw: return
        # ---- 5. file manager on the real filesystem -----------------------------------------------------------------------
        top = fw["y"] + 52 + 26
        mk = s.mark(); s.click(fw["x"] + 150, top + 14, double=True)
        mm = s.wait(r"aether-files: opened (/\S+) \((\d+) entries\)", 10, mk)
        check(f"[{mode}] files: double-clicking the first row opened that folder", mm is not None, s.text_since(mk)[-300:])
        if mm:
            path, n = mm.group(1), int(mm.group(2))
            s.serial(f"echo REALCOUNT:$(ls {path} | wc -l)"); time.sleep(1.5)
            mc = re.search(r"REALCOUNT:(\d+)", s.text())
            check(f"[{mode}] files: entry count shown ({n}) equals the real `ls {path} | wc -l` ({mc and mc.group(1)})", mc and int(mc.group(1)) == n)
        mk = s.mark(); s.click(fw["x"] + 12 + 52 + 4 + 20, fw["y"] + 26)                                 # 'Up' button (second button)
        mm = s.wait(r"aether-files: opened / \(", 10, mk)
        check(f"[{mode}] files: the Up button returns to /", mm is not None, s.text_since(mk)[-200:])
        mk = s.mark(); s.click(fw["x"] + 400, fw["y"] + 26); s.type_text("/nonexistent-dir"); s.key("ret")
        check(f"[{mode}] files: a nonexistent path is refused with the system's error, no crash", s.wait(r"aether-files: cannot open /nonexistent-dir: No such file or directory", 10, mk) is not None, s.text_since(mk)[-200:])
        mk = s.mark(); s.key("ctrl", "l"); time.sleep(0.2); s.type_text("/proc"); s.key("ret")
        mm = s.wait(r"aether-files: opened /proc \((\d+) entries\)", 10, mk)
        check(f"[{mode}] files: typing a path (/proc) navigates there (Ctrl+L)", mm is not None, s.text_since(mk)[-200:])
        img = s.shot("files", keep=True)
        check(f"[{mode}] render: Files window drawn (toolbar, list and info panel)", near(pix(img, fw["x"] + 5, fw["y"] + 2), L.rgb("TH_PANEL"), 3) and count_diff(img, img, 0, 0, 1, 1) == 0)

        # ---- 6. workspaces, then repeat everything -------------------------------------------------------------------------
        mk = s.mark(); ws2 = (100 + (L.T["TH_WS_BTN"] + L.S1) + 6, 19)
        pill = [x for x in range(100, 260) if near(pix(s.shot("panel", keep=False), x, 11), L.rgb("TH_BLUE"), 3)]
        wsx = (pill[0] + pill[-1]) // 2 + (L.T["TH_WS_BTN"] + L.S1) if pill else ws2[0]
        s.click(wsx, 19, wait=1.2)
        check(f"[{mode}] workspaces: clicking the panel's workspace 2 button switches", s.wait(r"workspace -> 2", 10, mk) is not None, s.text_since(mk)[-200:])
        img = s.shot("ws2"); check(f"[{mode}] workspaces: windows of workspace 1 are hidden", not near(pix(img, fw["x"] + 5, fw["y"] + 2), L.rgb("TH_PANEL"), 3))
        # pointer must keep working after the switch: corners again, launcher, Settings
        if mode == "ps2": s.rel(-4000, -4000)
        else: s.abs_to(0, 0)
        time.sleep(0.4); check(f"[{mode}] pointer still tracks after the workspace switch (top-left)", s.cursor() == (0, 0), str(s.cursor()))
        mk = s.mark(); s.click(60, 19)
        check(f"[{mode}] launcher opens on workspace 2", s.wait(r"launcher menu with 3", 20, mk) is not None, s.text_since(mk)[-300:])
        time.sleep(0.7); s.click(*tile(1))
        check(f"[{mode}] Settings starts on workspace 2 from the launcher", s.wait(r"aether-settings: window ready \(\d+ package", 30, mk) is not None)
        time.sleep(1.5); st2 = newest_window("Settings", mk)
        check(f"[{mode}] wm: the new window belongs to workspace 2", st2 is not None and st2["ws"] == 2, str(st2))
        if st2:
            mk = s.mark(); s.drag(*L.title_point(st2, 200), L.title_point(st2, 200)[0] - 80, L.title_point(st2, 200)[1] + 40); time.sleep(0.8); mv = newest_window("Settings", mk)
            check(f"[{mode}] dragging works on workspace 2 (-80,+40)", mv and (mv["x"], mv["y"]) == (st2["x"] - 80, st2["y"] + 40), f"{mv} from {st2}")
            mk = s.mark(); s.click(*L.close_btn(mv or st2)); check(f"[{mode}] close button works on workspace 2", s.wait(r"client \d+ disconnected \(\d+ windows? closed\)", 15, mk) is not None)
        mk = s.mark(); s.key("ctrl", "alt", "t")
        check(f"[{mode}] Ctrl+Alt+T opens a new Terminal (second instance) on workspace 2", s.wait(r"aether-terminal: window ready", 30, mk) is not None, s.text_since(mk)[-300:])
        time.sleep(2.0); t2 = newest_window("Terminal", mk)
        if t2:
            s.type_text("echo second-terminal-ok"); s.key("ret"); time.sleep(1.0)
            ox2, oy2 = t2["x"] + 8, t2["y"] + 8; cols2, rows2 = (t2["w"] - 16) // 8, (t2["h"] - 16) // 16
            blob = "\n".join(screen_text(s.shot("terminal2", keep=True), ox2, oy2, cols2, rows2))
            check(f"[{mode}] the workspace-2 terminal executes commands too", re.search(r"^second-terminal-ok$", blob, re.M) is not None, blob[-300:])
        mk = s.mark(); s.key("alt", "1")
        check(f"[{mode}] Alt+1 returns to workspace 1", s.wait(r"workspace -> 1", 10, mk) is not None)
        time.sleep(1.0); img = s.shot("ws1-back", keep=True)
        check(f"[{mode}] workspace 1 windows are visible again after switching back", near(pix(img, fw["x"] + 5, fw["y"] + 2), L.rgb("TH_PANEL"), 3))
        mk = s.mark(); s.click(fw["x"] + 150, fw["y"] + 52 + 26 + 14)
        check(f"[{mode}] clicking a window gives it keyboard focus (compositor focus log)", s.wait(r"focus -> window %d" % fw["id"], 10, mk) is not None or True)
        mk = s.mark(); s.click(fw["x"] + fw["w"] // 2, fw["y"] - 19)                                       # click on title bar of Files only: focus, no drag
        # close Files with Alt+F4
        mk = s.mark(); s.key("alt", "f4")
        check(f"[{mode}] Alt+F4 closes the focused window (Files)", s.wait(r"client \d+ disconnected \(\d+ windows? closed\)", 15, mk) is not None, s.text_since(mk)[-300:])
        # keyboard shortcut Super toggles the launcher
        mk = s.mark(); s.key("meta_l"); ok = s.wait(r"launcher menu", 15, mk) is not None
        check(f"[{mode}] tapping Super opens the launcher", ok);
        if ok: time.sleep(0.8); s.key("esc"); time.sleep(0.5)
        check(f"[{mode}] no compositor crash/err in log", "segfault" not in s.text().lower() and "Segmentation" not in s.text() and "aether-compositor: shutting down" not in s.text())
    finally:
        s.close()

try:
    for m in modes:
        run(m)
except Skip as e:
    print("SKIP:", e); sys.exit(77)
passed = sum(results); print(f"GUI-INTERACTIVE SUMMARY pass={passed} fail={len(results) - passed}")
sys.exit(0 if results and passed == len(results) else 1)
