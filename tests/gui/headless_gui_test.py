#!/usr/bin/env python3
"""End-to-end desktop test WITHOUT QEMU: real aether-compositor (software display, --headless) + the real client
binaries (panel, desktop, launcher, Settings, notifications), driven through the compositor's headless control socket
and checked by window state and pixels.

What this covers: wallpaper, panel, launcher, shortcuts, Settings, window management, workspaces, notifications,
screen resize, several resolutions, idle CPU/memory. What it does NOT cover (only tests/gui/qemu-gui-test.sh does):
the kernel, initramfs, DRM/KMS scanout and evdev input. Run `make gui` first.

usage: headless_gui_test.py [BIN_DIR] [--shots DIR]      exit status 0 when every check passed
"""
import os, signal, socket, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gui_layout as L

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BIN = os.path.join(ROOT, "build", "bin"); SHOTS = None
args = sys.argv[1:]
while args:
    a = args.pop(0)
    if a == "--shots": SHOTS = args.pop(0); os.makedirs(SHOTS, exist_ok=True)
    else: BIN = os.path.abspath(a)

passed = failed = 0
def check(name, cond, detail=""):
    global passed, failed
    if cond: passed += 1; print(f"PASS {name}")
    else: failed += 1; print(f"FAIL {name} {detail}")
def wait_for(fn, timeout=4.0, step=0.04):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v: return v
        time.sleep(step)
    return fn()

class Img:
    def __init__(self, path):
        d = open(path, "rb").read(); parts = d.split(b"\n", 3); self.w, self.h = map(int, parts[1].split()); self.d = parts[3]
    def px(self, x, y): i = (y * self.w + x) * 3; return (self.d[i], self.d[i + 1], self.d[i + 2])
    def region(self, x, y, w, h): return b"".join(self.d[((y + r) * self.w + x) * 3:((y + r) * self.w + x + w) * 3] for r in range(h))
    def count(self, x, y, w, h, pred): return sum(1 for r in range(y, y + h) for c in range(x, x + w) if pred(self.px(c, r)))
    def save_png(self, path):
        try:
            from PIL import Image; Image.frombytes("RGB", (self.w, self.h), self.d).save(path)
        except Exception: open(path.replace(".png", ".ppm"), "wb").write(b"P6\n%d %d\n255\n" % (self.w, self.h) + self.d)

class Rig:
    def __init__(self, res="1280x800", extra_apps=True):
        self.res = res; self.w, self.h = map(int, res.split("x")); self.tmp = tempfile.mkdtemp(prefix="aether-gui-")
        apps = os.path.join(self.tmp, "apps"); os.makedirs(apps)
        open(os.path.join(apps, "settings.app"), "w").write(f"[Aether App]\nname=Settings\ncomment=System settings and information\nexec={BIN}/aether-settings\nicon=settings\n")
        if extra_apps: open(os.path.join(apps, "ghost.app"), "w").write("[Aether App]\nname=Ghost\ncomment=Not installed\nexec=/nonexistent/ghost\n")
        self.env = dict(os.environ, AETHER_DISPLAY=os.path.join(self.tmp, "disp.sock"), AETHER_APP_DIR=apps)
        self.ctl_path = os.path.join(self.tmp, "ctl.sock"); self.logs = {}; self.procs = {}
        self.spawn("comp", [f"{BIN}/aether-compositor", "--headless", res, "--control", self.ctl_path]); time.sleep(0.5)
        self.spawn("panel", [f"{BIN}/aether-shell"]); self.spawn("desk", [f"{BIN}/aether-shell", "--desktop"])
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(60):
            try: self.s.connect(self.ctl_path); break
            except OSError: time.sleep(0.1)
        self.buf = b""; self.n = 0
        wait_for(lambda: len([x for x in self.windows() if x["kind"] in (1, 4)]) == 2, 6); time.sleep(0.4)
    def spawn(self, name, argv):
        self.logs[name] = open(os.path.join(self.tmp, name + ".log"), "w")
        self.procs[name] = subprocess.Popen(argv, env=self.env, stdout=self.logs[name], stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        return self.procs[name]
    def log(self, name): self.logs[name].flush(); return open(os.path.join(self.tmp, name + ".log")).read()
    def cmd(self, line):
        self.s.sendall((line + "\n").encode()); out = []
        while True:
            while b"\n" not in self.buf: self.buf += self.s.recv(65536)
            l, self.buf = self.buf.split(b"\n", 1); l = l.decode()
            if l == "ok": return out
            out.append(l)
    def windows(self):
        res = []
        for l in self.cmd("windows"):
            if not l.startswith("win "): continue
            head, title = l.split(" title=", 1); f = dict(p.split("=") for p in head.split()[2:]); d = {k: int(v) for k, v in f.items()}
            d["id"] = int(head.split()[1]); d["title"] = title; res.append(d)
        return res
    def win(self, kind=None, title=None):
        m = [w for w in self.windows() if (kind is None or w["kind"] == kind) and (title is None or w["title"] == title)]
        return m[-1] if m else None
    def shot(self, name=None):
        self.n += 1; path = os.path.join(self.tmp, f"s{self.n}.ppm"); self.cmd(f"shot {path}"); im = Img(path)
        if SHOTS and name: im.save_png(os.path.join(SHOTS, f"{self.res}-{name}.png"))
        return im
    def move(self, x, y): self.cmd(f"warp {x} {y}"); time.sleep(0.03)
    def down(self): self.cmd("button 1"); time.sleep(0.03)
    def up(self): self.cmd("button 0"); time.sleep(0.03)
    def click(self, x, y, settle=0.25): self.move(x, y); self.down(); self.up(); time.sleep(settle)
    def key(self, k, settle=0.05): self.cmd(f"key {k} 1"); self.cmd(f"key {k} 0"); time.sleep(settle)
    def drag(self, x0, y0, x1, y1):
        self.move(x0, y0); self.down()
        for i in range(1, 6): self.move(x0 + (x1 - x0) * i // 5, y0 + (y1 - y0) * i // 5)
        self.up(); time.sleep(0.15)
    def notify(self, title, body, timeout=3000, level="info"): return subprocess.Popen([f"{BIN}/aether-notify", title, body, str(timeout), level], env=self.env, stdout=subprocess.DEVNULL)
    def pid_stat(self, name):
        p = self.procs[name].pid; f = open(f"/proc/{p}/stat").read().rsplit(")", 1)[1].split(); rss = [l for l in open(f"/proc/{p}/status") if l.startswith("VmRSS")][0].split()[1]
        return int(f[11]) + int(f[12]), int(rss)
    def stop(self):
        for n in ("panel", "desk", "comp"):
            p = self.procs.get(n)
            if p and p.poll() is None: p.terminate()
        subprocess.run(["pkill", "-x", "aether-settings"], stderr=subprocess.DEVNULL); subprocess.run(["pkill", "-x", "aether-notify"], stderr=subprocess.DEVNULL)
        subprocess.run(["pkill", "-x", "aether-shell"], stderr=subprocess.DEVNULL)
        for p in self.procs.values():
            try: p.wait(3)
            except Exception: p.kill()

def lum(c): return sum(c)
def near(a, b, tol=3): return all(abs(x - y) <= tol for x, y in zip(a, b))
def kill_apps(): subprocess.run(["pkill", "-x", "aether-settings"], stderr=subprocess.DEVNULL); time.sleep(0.3)

# ======================================================================================================== 1280x800
def main_suite():
    r = Rig("1280x800"); W, H = r.w, r.h
    try:
        check("boot: compositor reports the display and listens", "display 1280x800" in r.log("comp") and "listening on" in r.log("comp"))
        pw, dw = r.win(1), r.win(4)
        check("boot: panel and desktop surface clients connected", bool(pw) and bool(dw))
        r.move(1255, 785); time.sleep(0.15)                      # park the pointer away from regions compared later
        base = r.shot("desktop")

        # ---- 1. wallpaper --------------------------------------------------------------------------------------
        check("wallpaper: desktop surface covers the whole work area below the panel", (dw["x"], dw["y"], dw["w"], dw["h"]) == (0, L.PANEL_H, W, H - L.PANEL_H), str(dw))
        distinct = len({base.px(x, y) for x in range(0, W, 16) for y in range(L.PANEL_H, H, 16)})
        check("wallpaper: is a rendered scene, not a flat or two-tone fill", distinct > 400, f"{distinct} distinct colours")
        check("wallpaper: blue emerges from the lower-right quadrant", base.px(1080, 740)[2] > base.px(120, 300)[2] + 25, f"{base.px(1080,740)} vs {base.px(120,300)}")
        check("wallpaper: edges darker than the interior (vignette)", lum(base.px(3, L.PANEL_H + 3)) <= lum(base.px(640, 200)), f"{base.px(3,41)} vs {base.px(640,200)}")
        check("wallpaper: quiet upper-left stays dark for icons", all(c < 0x28 for c in base.px(300, 150)), str(base.px(300, 150)))
        check("wallpaper: contains blue/cyan aurora ribbons", base.count(300, 380, 700, 300, lambda c: c[2] > 140 and c[1] > 100 and c[0] < 110) > 2000)
        check("wallpaper: no leftover legacy solid-blue desktop", base.px(900, 150) != (15, 52, 96) and base.px(100, 700) != (22, 33, 62))

        # ---- 2. panel --------------------------------------------------------------------------------------------
        check("panel: 38 px tall and full width at the top", (pw["x"], pw["y"], pw["w"], pw["h"]) == (0, 0, W, L.PANEL_H), str(pw))
        check("panel: uses the panel surface colour", base.px(700, 10) == L.rgb("TH_PANEL"), str(base.px(700, 10)))
        check("panel: 1 px lower border in the divider colour", base.px(700, L.PANEL_H - 1) == L.rgb("TH_BORDER") and base.px(700, L.PANEL_H) != L.rgb("TH_PANEL"))
        check("panel: clock text is rendered at the right", base.count(1100, 4, 170, 30, lambda c: min(c) > 190) > 25)
        row = [x for x in range(100, 260) if base.px(x, 11) == L.rgb("TH_BLUE")]
        check("panel: active workspace indicator in Aether Blue, compact", len(row) >= 14 and row[0] > 100, f"{len(row)} px starting {row[:1]}")
        c0 = (row[0] + row[-1]) // 2 if row else 131; ws_c = lambda i: (c0 + i * (L.T["TH_WS_BTN"] + L.S1), 19)
        r.move(*ws_c(2)); time.sleep(0.2); hov = r.shot()
        check("panel: hovered workspace indicator changes state", hov.px(ws_c(2)[0] - 9, 10) != L.rgb("TH_PANEL"), str(hov.px(ws_c(2)[0] - 9, 10)))
        nics = [n for n in os.listdir("/sys/class/net") if n != "lo"] if os.path.isdir("/sys/class/net") else []
        up = any(open(f"/sys/class/net/{n}/operstate").read().startswith("up") for n in nics)
        ind = base.count(1090, 6, 40, 26, lambda c: c[2] > 150 and c[0] < 150)
        if up: check("panel: network indicator is shown for a real, up interface", ind > 8, f"indicator pixels {ind}")
        elif not nics: check("panel: no network indicator when no real interface exists", ind <= 8, f"indicator pixels {ind}")

        # ---- 3. launcher -----------------------------------------------------------------------------------------
        r.move(900, 600); time.sleep(0.1); clean = r.shot()
        r.click(60, 19, 0.1); alphas = []
        t_end = time.time() + 0.5
        while time.time() < t_end: w = r.win(3); alphas.append(w["alpha"] if w else None); time.sleep(0.01)
        lw = r.win(3)
        check("launcher: opens from the launcher button below the panel", bool(lw) and lw["y"] >= L.PANEL_H and lw["w"] == 480 and lw["x"] >= 0, str(lw))
        seen = [a for a in alphas if a is not None]
        check("launcher: fades in (intermediate opacity observed, ends opaque)", any(0 < a < 255 for a in seen) and seen[-1] == 255, f"{sorted(set(seen))[:8]}")
        check("launcher: fits the screen and respects the 560 px maximum", lw["h"] <= 560 and lw["y"] + lw["h"] <= H)
        op = r.shot("launcher")
        check("launcher: search field is focused with the accent ring", op.count(lw["x"] + 12, lw["y"] + 12, 440, 6, lambda c: c == L.rgb("TH_BLUE")) > 100)
        check("launcher: button shows its active (open) state", op.px(14, 19) != base.px(14, 19))
        check("launcher: lists only applications that really exist (ghost entry hidden)", "launcher menu with 1 application(s)" in r.log("panel"), r.log("panel")[-200:])
        check("desktop: only real applications get a shortcut", "with 1 shortcut(s)" in r.log("desk"), r.log("desk")[-200:])
        r.key(44); r.key(44); time.sleep(0.3); none = r.shot("launcher-nomatch")
        check("launcher: search filters the real application list", none.region(lw["x"] + 20, lw["y"] + 100, 100, 80) != op.region(lw["x"] + 20, lw["y"] + 100, 100, 80))
        r.key(14); r.key(14); time.sleep(0.3)
        check("launcher: clearing the search restores the entries", r.shot().region(lw["x"] + 20, lw["y"] + 100, 100, 80) == op.region(lw["x"] + 20, lw["y"] + 100, 100, 80))
        r.key(1); time.sleep(0.5)
        check("launcher: Escape closes it", r.win(3) is None)
        r.move(900, 600); time.sleep(0.1)
        check("launcher: closing leaves no stale pixels", r.shot().region(0, L.PANEL_H, 520, 260) == clean.region(0, L.PANEL_H, 520, 260))
        r.click(60, 19, 0.5); r.click(900, 500, 0.5)
        check("launcher: clicking outside closes it", r.win(3) is None)
        r.click(60, 19, 0.5); r.click(60, 19, 0.6)
        check("launcher: clicking the button again closes it (no reopen race)", r.win(3) is None)
        r.cmd("key 125 1"); r.cmd("key 125 0"); time.sleep(0.5)
        check("launcher: Super key opens it", r.win(3) is not None)
        r.cmd("key 125 1"); r.cmd("key 125 0"); time.sleep(0.5)
        check("launcher: Super key closes it again", r.win(3) is None)
        r.click(60, 19, 0.5); r.key(28, 0.1)
        sw = wait_for(lambda: r.win(0, "Settings"), 5)
        check("launcher: Enter launches the real Settings application", bool(sw))
        check("launcher: closes itself after launching", wait_for(lambda: r.win(3) is None, 2) is True)
        sw = r.win(0, "Settings"); time.sleep(0.4)

        # ---- 4. Settings window + frame ---------------------------------------------------------------------------
        check("settings: opens as a real window at 760x520 (1280x800)", (sw["w"], sw["h"]) == (760, 520), str(sw))
        check("settings: centred in the work area and below the panel", abs(sw["x"] - (W - 760) // 2) <= 2 and sw["y"] - L.TITLE_H >= L.PANEL_H, str(sw))
        im = r.shot("settings")
        check("frame: title bar is 38 px in the active title colour", im.px(sw["x"] + 10, sw["y"] - 8) == L.TITLE_ACTIVE, str(im.px(sw["x"] + 10, sw["y"] - 8)))
        check("frame: rounded top corners (corner pixel is not title colour)", im.px(sw["x"], sw["y"] - L.TITLE_H) != L.TITLE_ACTIVE)
        check("frame: rounded bottom corners", im.px(sw["x"], sw["y"] + sw["h"] - 1) != L.rgb("TH_WINDOW"))
        check("frame: window title and controls are drawn", im.count(sw["x"] + 10, sw["y"] - 36, 80, 34, lambda c: min(c) > 200) > 20 and im.count(sw["x"] + sw["w"] - 110, sw["y"] - 34, 104, 30, lambda c: min(c) > 120) > 20)
        check("frame: sidebar is ~190 px with the panel surface colour", im.px(sw["x"] + 100, sw["y"] + 300) == L.rgb("TH_PANEL") and im.px(sw["x"] + 240, sw["y"] + 300) != L.rgb("TH_PANEL"))
        check("settings: selected nav item shows the blue indicator", im.px(sw["x"] + L.S2 + 3, sw["y"] + L.T["TH_NAV_TOP"] + 18) == L.rgb("TH_BLUE"), str(im.px(sw["x"] + 11, sw["y"] + 30)))
        pages = [im.region(sw["x"] + 200, sw["y"] + 10, 540, 200)]
        for i in range(1, 5):
            r.click(*L.nav_item(sw, i), 0.35); pages.append(r.shot(f"settings-page{i}").region(sw["x"] + 200, sw["y"] + 10, 540, 200))
        check("settings: every sidebar page renders distinct content", len(set(pages)) == 5)
        r.click(*L.nav_item(sw, 0), 0.3)
        check("tasks: panel shows a chip for the window, focused", r.shot().count(560, 5, 280, 28, lambda c: near(c, L.mix(L.rgb("TH_BORDER"), L.rgb("TH_BLUE"), 130), 4)) > 100)

        # ---- 5. window management ---------------------------------------------------------------------------------
        x0, y0 = sw["x"], sw["y"]
        r.drag(*L.title_point(sw), L.title_point(sw)[0] + 100, L.title_point(sw)[1] + 60)
        sw2 = r.win(0, "Settings")
        check("wm: dragging the title bar moves the window exactly", (sw2["x"], sw2["y"]) == (x0 + 100, y0 + 60), str(sw2))
        r.drag(*L.title_point(sw2), 300, -400)
        sw3 = r.win(0, "Settings")
        check("wm: a window cannot be dragged under the panel", sw3["y"] >= L.PANEL_H + L.TITLE_H, str(sw3))
        r.drag(*L.title_point(sw3), L.title_point(sw3)[0] - 200, 200); sw = r.win(0, "Settings")
        cb = L.close_btn(sw); r.move(*cb); r.down(); r.move(cb[0], cb[1] + 200); r.up(); time.sleep(0.2)
        check("wm: dragging off a title-bar button does not activate it", r.win(0, "Settings") is not None)
        r.move(*L.close_btn(sw)); time.sleep(0.2); hot = r.shot()
        check("wm: title-bar button shows a hover state (close turns error-red)", near(hot.px(L.close_btn(sw)[0] - 11, L.close_btn(sw)[1] - 11), L.rgb("TH_ERR"), 6), str(hot.px(L.close_btn(sw)[0] - 11, L.close_btn(sw)[1] - 11)))
        # second window -> focus, inactive title, raise
        r.click(60, 19, 0.5); r.key(28, 0.1); wait_for(lambda: len([w for w in r.windows() if w["title"] == "Settings"]) == 2, 5); time.sleep(0.4)
        a, b = sorted([w for w in r.windows() if w["title"] == "Settings"], key=lambda w: w["id"])
        check("wm: a second window opens focused on top, cascaded", b["focus"] == 1 and a["focus"] == 0 and (b["x"], b["y"]) != (a["x"], a["y"]))
        im = r.shot("two-windows")
        check("wm: inactive title bar is visibly different from the active one", im.px(a["x"] + 10, a["y"] - 8) == L.TITLE_INACTIVE and im.px(b["x"] + 10, b["y"] - 8) == L.TITLE_ACTIVE)
        inside = lambda w, x, y: w["x"] <= x < w["x"] + w["w"] and w["y"] - 38 <= y < w["y"] + w["h"]
        ex = next((x, y) for x in range(a["x"] + 5, a["x"] + a["w"], 40) for y in range(a["y"] + 5, a["y"] + a["h"], 40) if not inside(b, x, y))
        r.click(*ex, 0.3); fw = [w for w in r.windows() if w["title"] == "Settings" and w["focus"]]
        check("wm: clicking a window focuses and raises it", len(fw) == 1 and fw[0]["id"] == a["id"], f"{fw} click {ex}")
        r.click(*L.close_btn(a), 0.4); r.click(*L.close_btn(b), 0.4)
        check("wm: close button closes windows", wait_for(lambda: r.win(0, "Settings") is None, 3) is True)
        check("wm: closing windows leaves the wallpaper intact", r.shot().region(0, L.PANEL_H, 1280, 400) is not None and r.win(0) is None)
        kill_apps()

        # minimise / restore / maximise via the real controls
        r.click(60, 19, 0.5); r.key(28, 0.1); sw = wait_for(lambda: r.win(0, "Settings"), 5); time.sleep(0.5)
        area = (sw["x"], sw["y"] - 38, sw["w"], sw["h"] + 38); r.move(1250, 780); time.sleep(0.1)
        r.click(*L.min_btn(sw), 0.4); r.move(1250, 780); time.sleep(0.15); w = r.win(0, "Settings")
        check("wm: minimise hides the window (area shows the bare wallpaper again)", w["min"] == 1 and r.shot("minimised").region(*area) == base.region(area[0], area[1], area[2], area[3]) )
        chip = (640, 19); r.click(*chip, 0.4); w = r.win(0, "Settings")
        check("wm: clicking the panel task chip restores it", w["min"] == 0 and w["focus"] == 1)
        r.click(*chip, 0.4)
        check("wm: clicking the focused task chip minimises it", r.win(0, "Settings")["min"] == 1)
        r.click(*chip, 0.4)
        r.click(*L.max_btn(sw), 0.8); w = r.win(0, "Settings")
        check("wm: maximise resizes to the work area (real buffer resize)", (w["x"], w["y"], w["w"], w["h"]) == (0, L.PANEL_H + L.TITLE_H, W, H - L.PANEL_H - L.TITLE_H) and w["max"] == 1, str(w))
        mx = r.shot("maximised")
        check("wm: maximised content is laid out at the new size (sidebar left, content reaches the right edge)", mx.px(100, 400) == L.rgb("TH_PANEL") and mx.px(W - 8, 400) == L.rgb("TH_WINDOW"), f"{mx.px(100,400)} {mx.px(W-8,400)}")
        check("wm: maximised window has square corners flush with the screen", mx.px(0, H - 1) == L.rgb("TH_PANEL") and mx.px(W - 1, H - 1) == L.rgb("TH_WINDOW"), f"{mx.px(0,H-1)} {mx.px(W-1,H-1)}")
        r.click(*L.max_btn(w), 0.8); w = r.win(0, "Settings")
        check("wm: restore returns to the previous size and place", (w["x"], w["y"], w["w"], w["h"]) == (sw["x"], sw["y"], 760, 520) and w["max"] == 0, str(w))
        # ---- 6. workspaces ----------------------------------------------------------------------------------------
        r.cmd("key 56 1"); r.key(3); r.cmd("key 56 0"); time.sleep(0.5); w = r.win(0, "Settings")
        ws = [l for l in r.cmd("windows") if l.startswith("ptr")][0]
        check("workspace: Alt+2 switches workspace", " ws 1" in ws)
        im = r.shot("workspace2")
        r.move(1250, 780); time.sleep(0.1); im = r.shot("workspace2")
        check("workspace: windows of other workspaces are hidden (bare wallpaper where it was)", im.region(*area) == base.region(*area))
        check("workspace: panel highlights workspace 2 and drops the other window's chip", im.px(ws_c(1)[0] - 9, 11) == L.rgb("TH_BLUE") and im.count(560, 5, 280, 28, lambda c: near(c, L.mix(L.rgb("TH_BORDER"), L.rgb("TH_BLUE"), 130), 4)) < 20)
        r.click(*ws_c(0), 0.5); w = r.win(0, "Settings")
        check("workspace: clicking the panel indicator switches back and restores the window", [l for l in r.cmd("windows") if l.startswith("ptr")][0].endswith("ws 0") and r.shot().px(sw["x"] + 10, sw["y"] - 8) == L.TITLE_ACTIVE)
        r.click(*L.close_btn(sw), 0.5); kill_apps()

        # ---- 7. desktop shortcuts ----------------------------------------------------------------------------------
        cell = (L.T["TH_DESK_MARGIN"], L.PANEL_H + L.T["TH_DESK_MARGIN"], L.T["TH_DESK_CELL_W"], L.T["TH_DESK_CELL_H"] - L.S2)
        r.move(900, 600); time.sleep(0.1); idle = r.shot().region(*cell)
        cx, cy = cell[0] + cell[2] // 2, cell[1] + 8 + 32
        r.click(cx, cy, 0.6)
        check("desktop: single click selects the shortcut (visible selection state)", r.shot("shortcut-selected").region(*cell) != idle and r.win(0, "Settings") is None)
        r.click(900, 500, 0.3); r.move(900, 600); time.sleep(0.2)
        check("desktop: clicking the empty desktop clears the selection", r.shot().region(*cell) == idle)
        r.move(cx, cy); r.down(); r.up(); time.sleep(0.12); r.down(); r.up()
        check("desktop: double-click launches the application", wait_for(lambda: r.win(0, "Settings"), 5) is not None)
        sw = r.win(0, "Settings"); r.drag(*L.title_point(sw), 100, 120); time.sleep(0.2); sw = r.win(0, "Settings")
        im = r.shot("window-over-icon")
        check("desktop: shortcuts are drawn beneath windows", sw["x"] < cx < sw["x"] + sw["w"] and sw["y"] - 38 < cy < sw["y"] and im.px(cx, cy) == L.TITLE_ACTIVE, str(im.px(cx, cy)))
        r.click(*L.close_btn(sw), 0.5); kill_apps()

        # ---- 8. notifications ---------------------------------------------------------------------------------------
        r.move(700, 600); time.sleep(0.1); region = (W - 356, L.PANEL_H, 356, 200); clean = r.shot().region(*region)
        p1 = r.notify("Build finished", "All four GUI binaries compiled without warnings today.", 3000, "success"); time.sleep(0.15)
        p2 = r.notify("Low disk space", "Only 2 percent remains.", 3000, "error")
        ns = wait_for(lambda: [w for w in r.windows() if w["kind"] == 2] if len([w for w in r.windows() if w["kind"] == 2]) == 2 else None, 4); time.sleep(0.5)
        check("notify: popups appear top-right under the panel", bool(ns) and all(n["x"] == W - 340 - 16 and n["y"] >= L.PANEL_H for n in ns), str(ns))
        ns = sorted(ns, key=lambda n: n["id"])
        check("notify: stacked without overlap even with different heights", ns[1]["y"] >= ns[0]["y"] + ns[0]["h"], str(ns))
        im = r.shot("notifications")
        check("notify: level styling (success green, error red accent)", near(im.px(ns[0]["x"] + 1, ns[0]["y"] + 30), L.rgb("TH_OK"), 6) and near(im.px(ns[1]["x"] + 1, ns[1]["y"] + 30), L.rgb("TH_ERR"), 6))
        check("notify: readable text is rendered", im.count(ns[0]["x"] + 56, ns[0]["y"] + 10, 200, 40, lambda c: min(c) > 150) > 40)
        r.click(ns[0]["x"] + 100, ns[0]["y"] + 30, 0.2)
        fading = [w["alpha"] for w in r.windows() if w["kind"] == 2]
        check("notify: clicking dismisses with a fade", wait_for(lambda: len([w for w in r.windows() if w["kind"] == 2]) <= 1, 2) is True)
        left = wait_for(lambda: [w for w in r.windows() if w["kind"] == 2][0] if [w for w in r.windows() if w["kind"] == 2] else None, 1)
        check("notify: remaining notification reflows to the top slot", left and left["y"] == L.PANEL_H + L.T["TH_S3"], str(left))
        check("notify: auto-dismiss removes it", wait_for(lambda: not [w for w in r.windows() if w["kind"] == 2], 5) is True)
        r.move(700, 600); time.sleep(0.15)
        check("notify: dismissal leaves no stale pixels", r.shot().region(*region) == clean)

        # ---- 9. resize / repaint ------------------------------------------------------------------------------------
        r.click(60, 19, 0.5); r.key(28, 0.1); sw = wait_for(lambda: r.win(0, "Settings"), 5); time.sleep(0.4)
        r.cmd("resize 1024 768"); time.sleep(1.5); pw, dw = r.win(1), r.win(4); sw = r.win(0, "Settings"); im = r.shot("resized-1024")
        check("resize: panel and desktop surface follow the new display size", (pw["w"], dw["w"], dw["h"]) == (1024, 1024, 768 - L.PANEL_H), f"{pw} {dw}")
        check("resize: wallpaper re-rendered at the new size (not the fallback gradient)", len({im.px(x, y) for x in range(0, 1024, 8) for y in range(L.PANEL_H, 768, 8) if not (sw["x"] <= x < sw["x"] + sw["w"] and sw["y"] - 38 <= y < sw["y"] + sw["h"])}) > 400)
        check("resize: clock stays right-aligned in the panel", im.count(850, 4, 174, 30, lambda c: min(c) > 190) > 25 and im.count(1024 - 12, 4, 12, 30, lambda c: min(c) > 190) == 0)
        check("resize: open windows stay on screen", sw["x"] >= 0 and sw["x"] + sw["w"] <= 1024 and sw["y"] + sw["h"] <= 768, str(sw))
        r.cmd("resize 1280 800"); time.sleep(1.5); im = r.shot("resized-back")
        check("resize: growing back repaints correctly", r.win(1)["w"] == 1280 and r.win(4)["h"] == 800 - L.PANEL_H and im.px(400, 10) == L.rgb("TH_PANEL"))
        r.click(*L.close_btn(r.win(0, "Settings")), 0.4); kill_apps()

        # ---- 10. performance ----------------------------------------------------------------------------------------
        time.sleep(0.5); s0 = {n: r.pid_stat(n)[0] for n in ("comp", "panel", "desk")}; time.sleep(4.0); s1 = {n: r.pid_stat(n) for n in ("comp", "panel", "desk")}
        ticks = {n: s1[n][0] - s0[n] for n in s0}
        check("perf: idle desktop is idle (no busy loops, < 2% CPU over 4 s)", sum(ticks.values()) <= 8, f"ticks {ticks}")
        rss = {n: s1[n][1] // 1024 for n in s1}
        check("perf: resident memory is small (compositor+panel+desktop < 48 MiB)", sum(rss.values()) < 48, f"MiB {rss}")
        t = time.time()
        for i in range(400): r.cmd(f"warp {200 + (i * 7) % 800} {200 + (i * 3) % 400}")
        r.shot(); el = time.time() - t
        check("perf: 400 pointer moves processed promptly", el < 3.0, f"{el:.2f}s")
        print(f"INFO idle CPU ticks (4s, 100Hz): {ticks}; RSS MiB: {rss}; 400 moves: {el:.2f}s")
    finally:
        r.stop()

# ============================================================================================= other resolutions
def resolution_suite(res):
    r = Rig(res, extra_apps=False); W, H = r.w, r.h
    try:
        pw, dw = r.win(1), r.win(4)
        check(f"{res}: panel full-width and desktop surface fills the rest", (pw["w"], pw["h"]) == (W, L.PANEL_H) and (dw["w"], dw["h"]) == (W, H - L.PANEL_H), f"{pw} {dw}")
        im = r.shot("desktop")
        check(f"{res}: wallpaper rendered at the actual resolution", len({im.px(x, y) for x in range(0, W, 16) for y in range(L.PANEL_H, H, 16)}) > 300)
        r.click(60, 19, 0.6); lw = r.win(3)
        check(f"{res}: launcher fits on screen", bool(lw) and lw["x"] >= 0 and lw["x"] + lw["w"] <= W and lw["y"] + lw["h"] <= H and lw["w"] <= 480, str(lw))
        r.key(28, 0.1); sw = wait_for(lambda: r.win(0, "Settings"), 5); time.sleep(0.5)
        check(f"{res}: Settings is clamped to the screen", sw and sw["x"] >= 0 and sw["y"] - L.TITLE_H >= L.PANEL_H and sw["x"] + sw["w"] <= W and sw["y"] + sw["h"] <= H, str(sw))
        im = r.shot("settings")
        check(f"{res}: Settings content rendered (sidebar present)", im.px(sw["x"] + 20, sw["y"] + sw["h"] - 20) == L.rgb("TH_PANEL"))
        r.click(*L.close_btn(sw), 0.4)
    finally:
        r.stop()


# ============================================================================== mouse + window management (real clients)
import re as _re
_wmh = open(os.path.join(ROOT, "src", "gui", "wm.h")).read()
MIN_W = int(_re.search(r"#define WM_MIN_W (\d+)", _wmh).group(1)); MIN_H = int(_re.search(r"#define WM_MIN_H (\d+)", _wmh).group(1))
def mouse_suite(res="1280x800"):
    """Real compositor + real Settings windows: edge resize through the actual client re-layout, dragging into every screen
    edge, overlapping windows, minimise/maximise/close/focus afterwards, and no invisible blocker once the launcher is gone."""
    r = Rig(res, extra_apps=False); W, H = r.w, r.h; T = f"{res} mouse"
    ptr = lambda: tuple(int(v) for v in [l for l in r.cmd("windows") if l.startswith("ptr ")][0].split()[1:3])
    def settings(n=1):
        for _ in range(n): r.spawn(f"set{len(r.procs)}", [f"{BIN}/aether-settings"]); time.sleep(0.5)
        return [w for w in r.windows() if w["kind"] == 0]
    def frame(w): return (w["x"], w["y"] - L.TITLE_H, w["w"], w["h"] + L.TITLE_H)
    def grip(w, e):                                   # a point 2 px inside the frame, like a user would grab it
        x, y, fw, fh = frame(w); px = x + 2 if "l" in e else (x + fw - 3 if "r" in e else x + fw // 2); py = y + 2 if "t" in e else (y + fh - 3 if "b" in e else y + fh // 2); return px, py
    def settle(pred, t=3.0): return wait_for(pred, t, 0.05)
    def normalise(wid, nw=520, nh=340, cx=140, cy=150):
        get = lambda: next(q for q in r.windows() if q["id"] == wid)
        q = get(); r.drag(q["x"] + 60, q["y"] - 12, 4 + 60, L.PANEL_H + L.TITLE_H - 12 + 6); time.sleep(0.3)                    # top-left first, so the bottom-right grip is on screen
        q = get(); gx, gy = grip(q, "br"); r.move(gx, gy); r.down(); r.move(gx + (nw - q["w"]), gy + (nh - q["h"])); r.up()      # shrink
        settle(lambda: (lambda z: z if (z["w"], z["h"]) == (nw, nh) else None)(get()))
        q = get(); r.drag(q["x"] + 120, q["y"] - 12, cx + 120, cy - 12); time.sleep(0.3)                                        # then move it to a known spot (title drag)
        return settle(lambda: (lambda z: z if (z["w"], z["h"], z["x"], z["y"]) == (nw, nh, cx, cy) else None)(get()))
    try:
        r.move(W - 5, H - 5); sw = settings(1)[0]; sid = sw["id"]; cur = lambda: next((w for w in r.windows() if w["id"] == sid), None)
        check(f"{T}: Settings is a resizable window on screen", sw["w"] > 0 and sw["x"] >= 0 and sw["y"] - L.TITLE_H >= L.PANEL_H, str(sw))
        start = normalise(sid); check(f"{T}: setup: window normalised to 520x340 with room on all sides", bool(start), str(cur()))
        # -- resize on each edge/corner: the real client re-lays out and answers with a new buffer
        cases = [("r", 70, 0), ("b", 0, 40), ("l", -30, 0), ("t", 0, -20), ("br", 20, 20), ("tl", 25, 15), ("tr", -20, 15), ("bl", 15, -20)]
        for e, dx, dy in cases:
            before = cur(); gx, gy = grip(before, e); r.move(gx, gy); r.down()
            steps = 6
            for i in range(1, steps + 1): r.move(gx + dx * i // steps, gy + dy * i // steps)
            r.up(); exp_w = before["w"] + (dx if "r" in e else (-dx if "l" in e else 0)); exp_h = before["h"] + (dy if "b" in e else (-dy if "t" in e else 0))
            after = settle(lambda: (lambda c: c if c and c["w"] == exp_w and c["h"] == exp_h else None)(cur()))
            check(f"{T}: resize '{e}' gives {exp_w}x{exp_h}", bool(after), f"want {exp_w}x{exp_h}, got {cur()}")
            if after:
                fixed_ok = (("l" in e and after["x"] + after["w"] == before["x"] + before["w"]) or "l" not in e and after["x"] == before["x"]) and (("t" in e and after["y"] + after["h"] == before["y"] + before["h"]) or "t" not in e and after["y"] == before["y"])
                check(f"{T}: resize '{e}' keeps the opposite edges still", fixed_ok, f"{before} -> {after}")
        c = cur(); time.sleep(0.3); im = r.shot("resized"); x0, y0, fw, fh = frame(c)
        check(f"{T}: the client repainted at the new size (sidebar fills the new bottom-left)", im.px(c["x"] + 20, c["y"] + c["h"] - 20) == L.rgb("TH_PANEL"), str(im.px(c["x"] + 20, c["y"] + c["h"] - 20)))
        # -- limits
        gx, gy = grip(c, "br"); r.move(gx, gy); r.down(); r.move(gx - 1500, gy - 1500); r.up(); small = settle(lambda: (lambda q: q if q and q["w"] == MIN_W else None)(cur()))
        check(f"{T}: shrinking stops at the minimum size", bool(small) and small["h"] == MIN_H, str(cur()))
        c = cur(); gx, gy = grip(c, "br"); r.move(gx, gy); r.down(); r.move(W - 1, H - 1); r.up(); time.sleep(0.5); big = cur()
        check(f"{T}: growing stops at the screen edge", big["x"] + big["w"] <= W and big["y"] + big["h"] <= H, str(big))
        # -- every screen edge and corner: pointer arrives exactly, window title stays reachable, then drags back out
        ok_ptr = ok_title = ok_back = True
        for tx, ty in [(0, 0), (W // 2, 0), (W - 1, 0), (0, H // 2), (W - 1, H // 2), (0, H - 1), (W // 2, H - 1), (W - 1, H - 1)]:
            c = cur(); gx, gy = c["x"] + 120, c["y"] - 12; r.move(gx, gy); r.down()
            for i in range(1, 7): r.move(gx + (tx - gx) * i // 6, gy + (ty - gy) * i // 6)
            time.sleep(0.05); ok_ptr &= ptr() == (tx, ty); r.up(); time.sleep(0.1); d = cur()
            ok_title &= d["y"] - L.TITLE_H >= L.PANEL_H and d["x"] + d["w"] >= 80 and d["x"] <= W - 80 and d["y"] - L.TITLE_H <= H - L.TITLE_H
            gx, gy = d["x"] + min(120, d["w"] // 2), d["y"] - 12; gx = max(2, min(W - 3, gx)); r.move(gx, gy); r.down(); r.move(W // 2, H // 2); r.up(); time.sleep(0.1)
            e = cur(); ok_back &= (e["x"], e["y"]) != (d["x"], d["y"]) and e["y"] - L.TITLE_H >= L.PANEL_H
        check(f"{T}: the pointer reaches all 4 edges and 4 corners while dragging a window", ok_ptr)
        check(f"{T}: the dragged window's title bar stays reachable at every edge/corner", ok_title)
        check(f"{T}: the window drags back out of every edge/corner", ok_back)
        # -- two overlapping windows
        check(f"{T}: setup: first window parked at 420x300", bool(normalise(sid, 420, 300, 60, 150)), str(cur()))
        s2 = [w for w in settings(1) if w["id"] != sid][0]; id2 = s2["id"]; cur2 = lambda: next((w for w in r.windows() if w["id"] == id2), None)
        a = cur(); b = cur2()
        check(f"{T}: a second window opened and is focused above the first", b["focus"] == 1 and a["focus"] == 0, f"{a} {b}")
        # b opened centred on top of a: normalise b first (it is on top), then a, so that b overlaps a's right side
        check(f"{T}: setup: second window parked over the first one's right side", bool(normalise(id2, 360, 260, 380, 230)), str(cur2())); r.click(cur2()["x"] + 150, cur2()["y"] + 130, 0.2)
        a = cur(); b = cur2()
        gx = a["x"] + a["w"] - 3; gy = min(max(a["y"] + 40, frame(b)[1] + 5), b["y"] + b["h"] - 5)
        fb = frame(b); covered = fb[0] <= gx < fb[0] + fb[2] and fb[1] <= gy < fb[1] + fb[3]
        check(f"{T}: setup: a's right grip is under b", covered, f"a={a} b={b}")
        wa0 = a["w"]; r.move(gx, gy); r.down(); r.move(gx + 40, gy); r.up(); time.sleep(0.5)
        check(f"{T}: a grip hidden under another window does not resize the one below", cur()["w"] == wa0, str(cur()))
        gx, gy = a["x"] + a["w"] - 3, a["y"] - L.TITLE_H + 28; r.move(gx, gy); r.down(); r.move(gx + 30, gy); r.up()
        check(f"{T}: the uncovered part of that edge still resizes it", bool(settle(lambda: cur()["w"] == wa0 + 30)), str(cur()))
        # -- focus, maximise/restore, minimise, close: all still work after the resizing above
        a = cur(); r.click(a["x"] + 50, a["y"] + a["h"] - 30, 0.3); check(f"{T}: clicking a window focuses it", cur()["focus"] == 1 and cur2()["focus"] == 0, f"{cur()} {cur2()}")
        pre = cur(); r.click(*L.max_btn(pre), 0.2)
        mx = settle(lambda: (lambda q: q if q and q["max"] == 1 and q["w"] == W and q["h"] == H - L.PANEL_H - L.TITLE_H else None)(cur()))
        check(f"{T}: maximise fills the work area", bool(mx), str(cur()))
        r.click(*L.max_btn(cur()), 0.2)
        rs = settle(lambda: (lambda q: q if q and q["max"] == 0 and (q["w"], q["h"]) == (pre["w"], pre["h"]) else None)(cur()))
        check(f"{T}: restore returns to the size before maximising", bool(rs), f"{pre} -> {cur()}")
        r.click(*L.min_btn(cur()), 0.4); check(f"{T}: minimise still works", cur()["min"] == 1 and cur2()["focus"] == 1, f"{cur()} {cur2()}")
        r.click(*L.close_btn(cur2()), 0.6); check(f"{T}: close still works", cur2() is None)
        left = [w for w in r.windows() if w["kind"] == 0]
        check(f"{T}: exactly the minimised window is left", len(left) == 1 and left[0]["id"] == sid and left[0]["min"] == 1, str(left))
    finally:
        r.stop()

if __name__ == "__main__":
    for exe in ("aether-compositor", "aether-shell", "aether-settings", "aether-notify"):
        if not os.path.exists(os.path.join(BIN, exe)): print(f"missing {BIN}/{exe}: run `make gui` first"); sys.exit(2)
    main_suite()
    for res in ("1024x768", "800x600", "1920x1080"): resolution_suite(res)
    for res in ("1280x800", "800x600"): mouse_suite(res)
    print(f"{passed}/{passed + failed} headless GUI checks passed")
    sys.exit(0 if failed == 0 else 1)
