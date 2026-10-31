#!/usr/bin/env python3
"""End-to-end desktop test WITHOUT QEMU: real aether-compositor (software display, --headless) + the real client
binaries (panel, desktop, launcher, Settings, notifications), driven through the compositor's headless control socket
and checked by window state and pixels.

What this covers: wallpaper, panel, launcher, shortcuts, Settings, window management, workspaces, notifications,
right-click context menus (desktop / shortcut / task chip), screen resize, several resolutions, idle CPU/memory. What it does NOT cover (only tests/gui/qemu-gui-test.sh does):
the kernel, initramfs, DRM/KMS scanout and evdev input. Run `make gui` first.

usage: headless_gui_test.py [BIN_DIR] [--shots DIR]      exit status 0 when every check passed
"""
import fcntl, os, signal, socket, struct, subprocess, sys, tempfile, time
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
    def __init__(self, res="1280x800", extra_apps=True, app_files=None):
        self.res = res; self.w, self.h = map(int, res.split("x")); self.tmp = tempfile.mkdtemp(prefix="aether-gui-")
        apps = os.path.join(self.tmp, "apps"); os.makedirs(apps)
        open(os.path.join(apps, "settings.app"), "w").write(f"[Aether App]\nname=Settings\ncomment=System settings and information\nexec={BIN}/aether-settings\nicon=settings\n")
        if extra_apps: open(os.path.join(apps, "ghost.app"), "w").write("[Aether App]\nname=Ghost\ncomment=Not installed\nexec=/nonexistent/ghost\n")
        for fn, txt in (app_files or {}).items(): open(os.path.join(apps, fn), "w").write(txt)
        self.apps_dir = apps
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
    def rclick(self, x, y, settle=0.25): self.move(x, y); self.cmd("buttonx 3 1"); time.sleep(0.03); self.cmd("buttonx 3 0"); time.sleep(settle)     # button 3 = right
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

# ============================================================================================== right-click context menus
KEY_ESC, KEY_ENTER, KEY_UP, KEY_DOWN = 1, 28, 103, 108
MP, IH, SH, MW = L.T["TH_MENU_PAD"], L.T["TH_MENU_ITEM_H"], L.T["TH_MENU_SEP_H"], L.T["TH_MENU_W"]
def menu_h(rows): return 2 * MP + sum(IH if k == "i" else SH for k in rows)
def row_y(m, rows, i): return m["y"] + MP + sum(IH if k == "i" else SH for k in rows[:i]) + (IH if rows[i] == "i" else SH) // 2
def row_pt(m, rows, i): return (m["x"] + MW // 2, row_y(m, rows, i))
def menu_win(r): return r.win(3, "context-menu")
def wait_menu(r, timeout=3.0): return wait_for(lambda: (lambda m: m if m and m["alpha"] == 255 else None)(menu_win(r)), timeout)
def menu_gone(r, timeout=3.0): return wait_for(lambda: menu_win(r) is None, timeout)
def count_in(text, needle): return text.count(needle)

class RawClient:
    """A bare protocol client (what any third-party program could be): used to prove that only the panel may address other windows."""
    FMT = "<II6i48s"
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); self.s.connect(path); self.s.settimeout(0.4)
    def send(self, typ, window=0, v=(), text=b"", fd=None):
        data = struct.pack(self.FMT, typ, window, *(list(v) + [0] * 6)[:6], text)
        if fd is None: self.s.send(data)
        else: self.s.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack("i", fd))])
    def drain(self):
        out = []
        try:
            while True:
                d = self.s.recv(256)
                if not d: break
                f = struct.unpack(self.FMT, d); out.append((f[0], f[1], list(f[2:8])))
        except (socket.timeout, BlockingIOError): pass
        return out
    def create_window(self, w=120, h=80):
        fd = os.memfd_create("raw", os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING); os.ftruncate(fd, w * h * 4)
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_GROW)
        self.send(1, v=(1,)); self.drain()                                          # M_HELLO v0 = protocol version
        self.send(2, v=(w, h, 0, 0, 0, 0), text=b"raw-client", fd=fd)               # M_CREATE_WINDOW, a normal window
        for t, win, v in self.drain():
            if t == 101: return win                                                  # E_CONFIGURE carries the id
        return None
    def close(self): self.s.close()

def context_menu_suite(res="1280x800"):
    T = f"menu {res}"
    tools = tempfile.mkdtemp(prefix="aether-menu-tools-"); pids = {}
    def script(name):                                                                # a harmless 'application': records its PID, then idles
        p = os.path.join(tools, name + ".sh"); mark = os.path.join(tools, name + ".pid")
        open(p, "w").write(f"#!/bin/sh\necho $$ > {mark}\nexec sleep 40\n"); os.chmod(p, 0o755); return p, mark
    term, term_mark = script("terminal"); files, files_mark = script("files"); notes, notes_mark = script("notes")
    def app(name, exe): return f"[Aether App]\nname={name}\ncomment=test application\nexec={exe}\nicon=settings\n"
    r = Rig(res, app_files={"terminal.app": app("Terminal", term), "files.app": app("Files", files)}); W, H = r.w, r.h
    cx = W // 2                                                                      # the task chip of a single window sits in the middle of the panel
    def marker(path): return wait_for(lambda: os.path.exists(path) and open(path).read().strip() or None, 3.0)
    try:
        check(f"{T}: setup: desktop has shortcuts for the three real applications only", "with 3 shortcut(s)" in r.log("desk"), r.log("desk")[-200:])
        base = r.shot("ctx-base")

        # ---- right click on the panel's launcher button must not behave like a left click
        r.rclick(40, 19, 0.5)
        check(f"{T}: right-clicking the launcher button opens nothing", r.win(3) is None, str(r.windows()))
        r.click(40, 19, 0.5); check(f"{T}: ...while a left click still opens the launcher", r.win(3, "launcher") is not None)
        r.click(40, 19, 0.5); check(f"{T}: ...and closes it again", r.win(3) is None)

        # ---- desktop menu: contents, look, placement
        rows = ["i", "i", "i", "s", "i"]                                              # Terminal, Files, Settings, separator, Refresh
        px, py = W // 2 + 60, H // 2 - 60
        r.rclick(px, py, 0.3); m = wait_menu(r)
        check(f"{T}: right-click on the desktop opens a context menu", m is not None and m["kind"] == 3, str(menu_win(r)))
        if not m: return
        check(f"{T}: the menu opens with its corner at the pointer, fully faded in", (m["x"], m["y"]) == (px, py) and m["alpha"] == 255, str(m))
        check(f"{T}: it is {MW} px wide and exactly as tall as its five rows (3 apps, separator, Refresh)", (m["w"], m["h"]) == (MW, menu_h(rows)), str(m))
        check(f"{T}: the desktop menu lists only the applications that exist, then Refresh", "context menu (desktop) with 5 item(s)" in r.log("desk"), r.log("desk")[-300:])
        check(f"{T}: right-clicking launched nothing", [w for w in r.windows() if w["kind"] == 0] == [])
        im = r.shot("ctx-desktop-menu")
        check(f"{T}: the menu surface uses the window colour", near(im.px(m["x"] + MW - 10, row_y(m, rows, 0)), L.rgb("TH_WINDOW")))
        check(f"{T}: the separator is drawn in the border colour", near(im.px(m["x"] + MW // 2, row_y(m, rows, 3)), L.rgb("TH_BORDER")))
        check(f"{T}: every row has label text", all(im.count(m["x"] + 12, row_y(m, rows, i) - 8, 90, 16, lambda c: min(c) > 110) > 12 for i in (0, 1, 2, 4)))
        check(f"{T}: no row is highlighted before the pointer or keyboard picks one", all(near(im.px(m["x"] + MW - 10, row_y(m, rows, i)), L.rgb("TH_WINDOW")) for i in (0, 1, 2, 4)))
        hover = L.mix(L.rgb("TH_SURFACE2"), L.rgb("TH_BLUE"), 34)
        r.move(*row_pt(m, rows, 1)); time.sleep(0.2); im = r.shot()
        check(f"{T}: hovering a row highlights exactly that row", near(im.px(m["x"] + MW - 10, row_y(m, rows, 1)), hover, 4) and near(im.px(m["x"] + MW - 10, row_y(m, rows, 0)), L.rgb("TH_WINDOW")))
        r.key(KEY_DOWN); time.sleep(0.15); im = r.shot()
        check(f"{T}: Down moves the highlight to the next row", near(im.px(m["x"] + MW - 10, row_y(m, rows, 2)), hover, 4) and near(im.px(m["x"] + MW - 10, row_y(m, rows, 1)), L.rgb("TH_WINDOW")))
        r.key(KEY_DOWN); time.sleep(0.15); im = r.shot()
        check(f"{T}: Down skips the separator and lands on Refresh", near(im.px(m["x"] + MW - 10, row_y(m, rows, 4)), hover, 4) and near(im.px(m["x"] + MW - 10, row_y(m, rows, 2)), L.rgb("TH_WINDOW")))
        r.key(KEY_DOWN); time.sleep(0.15); im = r.shot()
        check(f"{T}: the selection wraps to the first row", near(im.px(m["x"] + MW - 10, row_y(m, rows, 0)), hover, 4))
        r.key(KEY_UP); time.sleep(0.15); im = r.shot()
        check(f"{T}: Up wraps to the last row", near(im.px(m["x"] + MW - 10, row_y(m, rows, 4)), hover, 4))
        r.move(5, H - 5); time.sleep(0.2); im = r.shot()
        check(f"{T}: moving the pointer away clears the highlight", all(near(im.px(m["x"] + MW - 10, row_y(m, rows, i)), L.rgb("TH_WINDOW")) for i in (0, 1, 2, 4)))
        r.key(KEY_ESC); check(f"{T}: Escape closes the menu", bool(menu_gone(r)))
        check(f"{T}: ...and did nothing else", [w for w in r.windows() if w["kind"] in (0, 3)] == [] and not os.path.exists(term_mark))
        check(f"{T}: closing the menu leaves no stale pixels", r.shot().region(px - 4, py - 4, MW + 40, menu_h(rows) + 40) == base.region(px - 4, py - 4, MW + 40, menu_h(rows) + 40))

        # ---- dismissal rules
        r.rclick(px, py, 0.3); m = wait_menu(r)
        r.click(200, H - 120, 0.4); check(f"{T}: a left click elsewhere dismisses the menu", menu_gone(r) is True and r.win(3) is None)
        r.rclick(px, py, 0.3); m = wait_menu(r)
        r.rclick(300, 300, 0.5)
        check(f"{T}: a right click elsewhere only dismisses the open menu (no second menu on that click)", menu_win(r) is None)
        r.rclick(300, 300, 0.3); m2 = wait_menu(r)
        check(f"{T}: the next right click opens a fresh menu at the new spot", m2 is not None and (m2["x"], m2["y"]) == (300, 300), str(m2))
        r.key(KEY_ESC); menu_gone(r)

        # ---- edge placement: the menu is always fully on screen and never over the panel
        r.rclick(W - 3, H - 3, 0.3); m = wait_menu(r)
        check(f"{T}: at the bottom-right corner the menu stays fully on screen", m is not None and m["x"] + m["w"] <= W and m["y"] + m["h"] <= H and m["y"] >= L.PANEL_H, str(m))
        check(f"{T}: ...opening up and to the left of the pointer", m is not None and m["x"] + m["w"] == W and m["y"] + m["h"] <= H - 3, str(m))
        r.key(KEY_ESC); menu_gone(r)
        r.rclick(6, L.PANEL_H + 2, 0.3); m = wait_menu(r)
        check(f"{T}: at the top-left corner it opens below the panel", m is not None and m["y"] >= L.PANEL_H and m["x"] >= 0, str(m))
        r.key(KEY_ESC); menu_gone(r)

        # ---- real actions: Terminal (mouse), Files (keyboard), Settings (mouse)
        r.rclick(px, py, 0.3); m = wait_menu(r); r.click(*row_pt(m, rows, 0), 0.3)
        pid = marker(term_mark)
        check(f"{T}: choosing Terminal starts the real Terminal application", bool(pid) and os.path.exists(f"/proc/{pid}"), str(pid))
        if pid: pids["terminal"] = int(pid)
        check(f"{T}: ...and the menu closes", bool(menu_gone(r)))
        r.rclick(px, py, 0.3); m = wait_menu(r); r.key(KEY_DOWN); r.key(KEY_DOWN); r.key(KEY_ENTER, 0.3)
        pid = marker(files_mark)
        check(f"{T}: Down, Down, Enter starts the Files application from the keyboard", bool(pid) and os.path.exists(f"/proc/{pid}"), str(pid))
        if pid: pids["files"] = int(pid)
        check(f"{T}: ...and the menu closes", bool(menu_gone(r)))

        # ---- Refresh: picks up applications installed/removed since the desktop started, redraws the surface
        before = r.shot(); region = (0, L.PANEL_H, 140, 4 * 108 + 40); logs0 = count_in(r.log("desk"), "desktop refreshed")
        open(os.path.join(r.apps_dir, "notes.app"), "w").write(app("Notes", notes))
        r.rclick(px, py, 0.3); m = wait_menu(r); r.click(*row_pt(m, rows, 4), 0.3)
        ok = wait_for(lambda: count_in(r.log("desk"), "desktop refreshed with 4 shortcut(s)") == 1, 3.0)
        check(f"{T}: Refresh re-reads the installed applications (a new one gets a shortcut)", bool(ok), r.log("desk")[-300:])
        after = r.shot("ctx-refreshed")
        check(f"{T}: ...and the desktop is repainted with it", after.region(*region) != before.region(*region))
        dw = r.win(4); check(f"{T}: the desktop surface keeps its place and size", dw is not None and (dw["x"], dw["y"], dw["w"], dw["h"]) == (0, L.PANEL_H, W, H - L.PANEL_H), str(dw))
        os.remove(os.path.join(r.apps_dir, "notes.app")); r.move(W - 20, H - 20)
        r.rclick(px, py, 0.3); m = wait_menu(r); r.click(*row_pt(m, rows, 4), 0.3); r.move(W - 20, H - 20)
        ok = wait_for(lambda: count_in(r.log("desk"), "desktop refreshed with 3 shortcut(s)") >= 1, 3.0)
        check(f"{T}: Refresh also drops an application that is gone", bool(ok), r.log("desk")[-300:])
        back = r.shot()
        check(f"{T}: ...and restores the exact original desktop (wallpaper re-rendered identically)", back.region(*region) == before.region(*region))
        check(f"{T}: Refresh only reloads: no application was started by it", not os.path.exists(notes_mark))

        # ---- shortcut menu: Open
        sy = L.PANEL_H + L.T["TH_DESK_MARGIN"] + 1 * L.T["TH_DESK_CELL_H"] + 40; sx = L.T["TH_DESK_MARGIN"] + L.T["TH_DESK_CELL_W"] // 2     # the second shortcut (Settings)
        idle = r.shot(); r.rclick(sx, sy, 0.3); m = wait_menu(r); srows = ["i"]
        check(f"{T}: right-click on a shortcut opens the shortcut menu (a single Open)", m is not None and (m["w"], m["h"]) == (MW, menu_h(srows)) and "context menu (shortcut) with 1 item(s)" in r.log("desk"), f"{m} {r.log('desk')[-200:]}")
        check(f"{T}: the right click only selected the shortcut: nothing opened", r.win(0, "Settings") is None)
        r.move(W - 20, H - 20); time.sleep(0.2)
        check(f"{T}: the shortcut shows it is selected", r.shot().region(24, L.PANEL_H + 24 + 108, 96, 100) != idle.region(24, L.PANEL_H + 24 + 108, 96, 100))
        r.key(KEY_ESC); menu_gone(r)
        check(f"{T}: Escape on the shortcut menu opens nothing", r.win(0, "Settings") is None)
        r.rclick(sx, sy, 0.3); m = wait_menu(r); r.click(*row_pt(m, srows, 0), 0.3)
        s1 = wait_for(lambda: r.win(0, "Settings"), 4.0)
        check(f"{T}: Open starts the real Settings application for that shortcut", s1 is not None)
        if not s1: return
        check(f"{T}: ...and the menu closes", bool(menu_gone(r)))

        # ---- task chip menu: Activate / Close
        s1 = r.win(0, "Settings"); sid = s1["id"]; cur = lambda: next((w for w in r.windows() if w["id"] == sid), None)
        trows = ["i", "i"]
        r.rclick(cx, 19, 0.3); m = wait_menu(r)
        check(f"{T}: right-click on a task chip opens the task menu (Activate, Close) just below the panel", m is not None and (m["w"], m["h"]) == (MW, menu_h(trows)) and m["y"] == L.PANEL_H + L.S1 and "context menu (task) with 2 item(s)" in r.log("panel"), f"{m} {r.log('panel')[-200:]}")
        check(f"{T}: opening it changed nothing (right click does not activate or minimise the window)", cur()["min"] == 0 and cur()["focus"] == 1)
        r.key(KEY_ESC); menu_gone(r)
        check(f"{T}: Escape leaves the window untouched", cur() is not None and cur()["min"] == 0 and cur()["focus"] == 1)
        r.rclick(cx, 19, 0.3); m = wait_menu(r); r.click(*row_pt(m, trows, 0), 0.4)
        check(f"{T}: Activate on the focused window keeps it visible and focused (a chip left-click would minimise it)", cur()["min"] == 0 and cur()["focus"] == 1, str(cur()))
        r.click(*L.min_btn(cur()), 0.4); check(f"{T}: (setup) minimise the window", cur()["min"] == 1)
        r.rclick(cx, 19, 0.3); m = wait_menu(r); r.click(*row_pt(m, trows, 0), 0.5)
        check(f"{T}: Activate restores a minimised window, raised and focused", cur()["min"] == 0 and cur()["focus"] == 1, str(cur()))

        # two windows: the menu acts on the chip that was clicked, not on another window (first window minimised so it cannot cover the desktop)
        r.click(*L.min_btn(cur()), 0.4); check(f"{T}: (setup) the first window is minimised", cur()["min"] == 1)
        r.rclick(px, py, 0.3); m = wait_menu(r); r.click(*row_pt(m, rows, 2), 0.3)
        s2 = wait_for(lambda: next((w for w in r.windows() if w["kind"] == 0 and w["id"] != sid), None), 4.0)
        check(f"{T}: a second Settings window opens from the desktop menu's Settings entry", s2 is not None)
        if s2:
            id2 = s2["id"]; cur2 = lambda: next((w for w in r.windows() if w["id"] == id2), None); left, right = cx - 92, cx + 92
            check(f"{T}: (setup) the new window has the focus", cur2()["focus"] == 1 and cur()["focus"] == 0)
            r.rclick(left, 19, 0.3); m = wait_menu(r); r.click(*row_pt(m, trows, 0), 0.5)
            check(f"{T}: Activate on the first chip restores and focuses the first window, not the second", cur() is not None and cur()["min"] == 0 and cur()["focus"] == 1 and cur2()["focus"] == 0, f"{cur()} {cur2()}")
            r.rclick(right, 19, 0.3); m = wait_menu(r); r.click(*row_pt(m, trows, 1), 0.6)
            check(f"{T}: Close on the second chip closes the second window only", cur2() is None and cur() is not None, f"{cur()} {cur2()}")
            check(f"{T}: ...and the closed window's process exits (it received a real close request)", bool(wait_for(lambda: subprocess.run(["pgrep", "-c", "-x", "aether-settings"], capture_output=True, text=True).stdout.strip() == "1", 3.0)))

        # a program that is not the panel cannot close, raise or minimise other windows
        rc = RawClient(r.env["AETHER_DISPLAY"]); mine = rc.create_window()
        check(f"{T}: (setup) a raw protocol client can create its own window", mine is not None)
        if mine:
            rc.drain(); rc.send(9, window=sid); rc.send(9, window=mine); rc.send(8, window=sid, v=(1,)); rc.send(8, window=sid); time.sleep(0.4)
            got = rc.drain()
            check(f"{T}: M_TASK_CLOSE from a non-panel client closes nothing (not another window, not its own)", cur() is not None and not any(t == 104 for t, _, _ in got), f"{cur()} {got}")
            check(f"{T}: M_TASK_ACTIVATE from a non-panel client is still ignored", cur()["min"] == 0 and cur()["focus"] in (0, 1))
        rc.close()
        r.click(*L.min_btn(cur()), 0.4); r.rclick(cx, 19, 0.3); m = wait_menu(r); r.click(*row_pt(m, trows, 1), 0.6)
        check(f"{T}: Close works on a minimised window too", cur() is None)
        check(f"{T}: ...and its chip is gone from the panel", r.shot().count(cx - 100, 5, 200, 28, lambda c: near(c, L.mix(L.rgb("TH_BORDER"), L.rgb("TH_BLUE"), 130), 4)) < 20)
        r.rclick(cx, 19, 0.5); check(f"{T}: right-clicking where no chip exists opens nothing", r.win(3) is None)
        check(f"{T}: no compositor/shell/menu process crashed", all(r.procs[n].poll() is None for n in ("comp", "panel", "desk")), str({n: r.procs[n].poll() for n in ("comp", "panel", "desk")}))
        check(f"{T}: no menu popup process is left behind", wait_for(lambda: subprocess.run(["pgrep", "-f", "aether-shell --menu"], capture_output=True).returncode != 0, 3.0))
    finally:
        for p in pids.values():
            try: os.kill(p, signal.SIGKILL)
            except OSError: pass
        r.stop()

if __name__ == "__main__":
    for exe in ("aether-compositor", "aether-shell", "aether-settings", "aether-notify"):
        if not os.path.exists(os.path.join(BIN, exe)): print(f"missing {BIN}/{exe}: run `make gui` first"); sys.exit(2)
    main_suite()
    for res in ("1024x768", "800x600", "1920x1080"): resolution_suite(res)
    for res in ("1280x800", "800x600"): mouse_suite(res)
    for res in ("1280x800", "800x600"): context_menu_suite(res)
    print(f"{passed}/{passed + failed} headless GUI checks passed")
    sys.exit(0 if failed == 0 else 1)
