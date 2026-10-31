#!/usr/bin/env python3
"""End-to-end GUI test: boots the GUI image in QEMU (std VGA -> bochs-drm), drives the desktop with
mouse/keyboard events through the QEMU monitor, takes screendumps and asserts on pixels + logs.
Exit 77 = skipped (no QEMU / kernel / image). Screenshots are written to build/gui-shots/*.png."""
import os, re, socket, subprocess, sys, tempfile, threading, time

P = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
KERNEL = os.environ.get("KERNEL", f"{P}/build/kernel/bzImage"); INITRD = f"{P}/build/aether-gui-initramfs.cpio.gz"
SHOTS = f"{P}/build/gui-shots"
qemu = subprocess.run(["sh", "-c", "command -v qemu-system-x86_64"], capture_output=True, text=True).stdout.strip()
if not qemu or not os.path.exists(KERNEL) or not os.path.exists(INITRD):
    print("SKIP: need qemu-system-x86_64, a kernel and build/aether-gui-initramfs.cpio.gz"); sys.exit(77)
os.makedirs(SHOTS, exist_ok=True)
msock = tempfile.mktemp(prefix="aether-mon-", suffix=".sock")
proc = subprocess.Popen([qemu, "-m", "512", "-vga", "std", "-display", "none", "-serial", "stdio", "-no-reboot",
    "-monitor", f"unix:{msock},server,nowait", "-kernel", KERNEL, "-initrd", INITRD,
    "-append", "console=ttyS0 rdinit=/init panic=1"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
log = []; lock = threading.Lock()
def reader():
    for raw in iter(proc.stdout.readline, b""):
        with lock: log.append(raw.decode("utf-8", "replace").rstrip("\r\n"))
threading.Thread(target=reader, daemon=True).start()
def logtext():
    with lock: return "\n".join(log)
def wait_log(pat, timeout):
    end = time.time() + timeout
    while time.time() < end:
        m = re.search(pat, logtext())
        if m: return m
        if proc.poll() is not None: break
        time.sleep(0.3)
    return None
def serial(cmd): proc.stdin.write((cmd + "\n").encode()); proc.stdin.flush()
mon = None
def monitor(cmd):
    global mon
    if mon is None:
        for _ in range(50):
            try: mon = socket.socket(socket.AF_UNIX); mon.connect(msock); break
            except OSError: mon = None; time.sleep(0.2)
        mon.settimeout(5); time.sleep(0.3); mon.recv(65536)
    mon.sendall((cmd + "\n").encode()); time.sleep(0.25)
    try: return mon.recv(65536).decode("utf-8", "replace")
    except socket.timeout: return ""
def ppm(path):
    d = open(path, "rb").read(); m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", d); w, h = int(m.group(1)), int(m.group(2)); return w, h, d[m.end():]
def shot(name):
    f = f"{SHOTS}/{name}.ppm"; monitor(f"screendump {f}"); time.sleep(0.5)
    w, h, px = ppm(f)
    try:
        from PIL import Image; Image.frombytes("RGB", (w, h), px).save(f"{SHOTS}/{name}.png"); os.remove(f)
    except Exception: pass
    return w, h, px
def pix(img, x, y): w, h, px = img; o = (y * w + x) * 3; return (px[o], px[o + 1], px[o + 2])
def near(a, b, tol=6): return all(abs(p - q) <= tol for p, q in zip(a, b))
def count_diff(a, b, x0, y0, x1, y1):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if pix(a, x, y) != pix(b, x, y): n += 1
    return n
def count_not(img, x0, y0, x1, y1, color):
    return sum(1 for y in range(y0, y1) for x in range(x0, x1) if not near(pix(img, x, y), color, 3))
def move_to(x, y):                       # absolute positioning through relative PS/2 events
    for _ in range(12): monitor("mouse_move -200 -200")
    cx, cy = x, y
    while cx > 0 or cy > 0:
        dx, dy = min(cx, 100), min(cy, 100); monitor(f"mouse_move {dx} {dy}"); cx -= dx; cy -= dy
def click(x, y, wait=1.5):
    move_to(x, y); time.sleep(0.4); monitor("mouse_button 1"); time.sleep(0.3); monitor("mouse_button 0"); time.sleep(wait)

results = []
def check(name, cond, detail=""):
    results.append(bool(cond)); print(("PASS " if cond else "FAIL ") + name + ("" if cond else f"   [{detail}]"), flush=True)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gui_layout as L                    # colours and geometry come from src/gui/theme.h, so this test tracks the design
PANEL = L.rgb("TH_PANEL"); BLUE = L.rgb("TH_BLUE"); TITLE = L.TITLE_ACTIVE; WINBG = L.rgb("TH_WINDOW"); SIDEBAR = L.rgb("TH_PANEL")
def distinct(img, x0, y0, x1, y1, step=8): return len({pix(img, x, y) for y in range(y0, y1, step) for x in range(x0, x1, step)})
try:
    m = wait_log(r"aether-compositor: display (\d+)x(\d+) via KMS dumb buffer, driver=(\S+) card=(\S+)", 120)
    check("boot: kernel exposes a DRM device after the session loads the GPU module", m is not None, logtext()[-600:])
    if not m: raise SystemExit
    W, H, drv = int(m.group(1)), int(m.group(2)), m.group(3)
    print(f"INFO display {W}x{H} driver={drv}")
    SX = (W - L.T["TH_SETTINGS_W"]) // 2; SY = L.PANEL_H + L.TITLE_H + (H - L.PANEL_H - L.TITLE_H - L.T["TH_SETTINGS_H"]) // 2   # centred first window (client origin)
    SW = dict(x=SX, y=SY, w=L.T["TH_SETTINGS_W"], h=L.T["TH_SETTINGS_H"])
    check("display: KMS dumb-buffer scanout works (driver bochs-drm)", drv.startswith("bochs"), drv)
    check("input: evdev devices found", re.search(r"aether-compositor: ([1-9]\d*) input device", logtext()) is not None)
    check("shell: panel client connected and drew", wait_log(r"aether-shell: panel running on \d+x\d+ screen", 60) is not None)
    wait_log(r"aether-shell: desktop surface \d+x\d+ with \d+ shortcut", 60)      # the wallpaper is rendered by the desktop client
    time.sleep(3)
    s1 = shot("01-desktop")
    check("render: screendump matches framebuffer size", (s1[0], s1[1]) == (W, H), f"{s1[0]}x{s1[1]}")
    check("render: panel background drawn across the top", near(pix(s1, W // 2, 10), PANEL, 3) and near(pix(s1, W - 300, 3), PANEL, 3) and near(pix(s1, W // 2, L.PANEL_H - 1), L.rgb("TH_BORDER"), 3))
    pill = [x for x in range(100, 260) if near(pix(s1, x, 11), BLUE, 3)]
    check("render: active workspace indicator uses the accent colour", len(pill) >= 14, f"{len(pill)} px")
    check("render: panel clock text is drawn", count_not(s1, W - 160, 4, W - 12, 30, PANEL) > 30)
    check("render: Aurora wallpaper visible below the panel (rendered scene, bluer lower-right)", distinct(s1, 0, L.PANEL_H, W, H) > 300 and pix(s1, int(W * .84), int(H * .93))[2] > pix(s1, 100, 300)[2] + 20)
    ptr = sum(1 for y in range(H // 2 - 4, H // 2 + 20) for x in range(W // 2 - 4, W // 2 + 16) if near(pix(s1, x, y), L.rgb("TH_TEXT"), 3))
    check("render: mouse cursor sprite visible", ptr > 10, f"pointer px={ptr}")

    click(60, 19)
    check("launcher: click opens the application menu", wait_log(r"aether-shell: launcher menu with 3 application", 20) is not None and wait_log(r"window \d+ created kind=3", 5) is not None)
    time.sleep(1.0); s2 = shot("02-launcher")
    check("render: launcher popup is drawn under the button", count_diff(s1, s2, 12, L.PANEL_H + 4, 492, 270) > 2000)

    click(196, 176)                                                   # the Settings tile: second of the three real applications (Files, Settings, Terminal; column 1)
    check("launcher: choosing Settings starts the application", wait_log(r"aether-settings: window ready \((\d+) package entries\)", 30) is not None)
    check("wm: normal window created with its title", wait_log(r"window \d+ created kind=0 \d+x\d+ title='Settings'", 5) is not None)
    time.sleep(1.5); s3 = shot("03-settings")
    check("render: Settings title bar (active) drawn by the compositor", near(pix(s3, SX + 10, SY - 8), TITLE, 3), str(pix(s3, SX + 10, SY - 8)))
    check("render: Settings client area drawn from the app's shared memory", near(pix(s3, SX + 600, SY + 450), WINBG, 3) and near(pix(s3, SX + 100, SY + 450), SIDEBAR, 3))
    check("wm: launcher popup closed after launching", wait_log(r"client \d+ disconnected", 10) is not None)

    click(*L.nav_item(SW, 2), wait=2.0); s4 = shot("04-packages")
    check("toolkit: sidebar button switches the page (content area changed)", count_diff(s3, s4, SX + 200, SY + 10, SX + 740, SY + 300) > 300)
    check("toolkit: Packages page lists the installed packages", count_not(s4, SX + 210, SY + 90, SX + 740, SY + 300, WINBG) > 150)

    ws0 = (pill[0] + pill[-1]) // 2
    click(ws0 + (L.T["TH_WS_BTN"] + L.S1), 19, wait=2.0)
    check("workspaces: panel button switches workspace", wait_log(r"workspace -> 2", 10) is not None)
    s5 = shot("05-workspace2")
    check("workspaces: window is hidden on the other workspace", not near(pix(s5, SX + 10, SY - 8), TITLE, 3) and count_diff(s4, s5, SX, SY - 38, SX + 760, SY + 300) > 3000)
    monitor("sendkey alt-1"); time.sleep(2.0); s6 = shot("06-back")
    check("workspaces: Alt+1 shortcut returns to workspace 1", wait_log(r"workspace -> 1", 10) is not None and near(pix(s6, SX + 10, SY - 8), TITLE, 3))

    serial('aether-notify "Aether" "Hello from the shell"')
    check("notify: aether-notify connects and shows a notification", wait_log(r"aether-notify: showing 'Aether'", 30) is not None)
    time.sleep(1.0); s7 = shot("07-notification")
    nx, ny = W - 56, L.PANEL_H + 12 + 55                              # empty card interior, away from text and the close button
    check("render: notification window appears top-right", near(pix(s7, nx, ny), WINBG, 2), str(pix(s7, nx, ny)))
    time.sleep(6.0); s8 = shot("08-notification-gone")
    check("notify: notification auto-dismisses", not near(pix(s8, nx, ny), WINBG, 2))

    click(*L.close_btn(SW), wait=2.5)
    check("wm: title-bar close button closes the application", wait_log(r"client \d+ disconnected \(\d+ windows? closed\)", 15) is not None)
    s9 = shot("09-closed")
    check("render: closed window no longer drawn", not near(pix(s9, SX + 10, SY - 8), TITLE, 3))
finally:
    try: serial("poweroff")
    except Exception: pass
    time.sleep(2); proc.kill()
    try: os.unlink(msock)
    except OSError: pass
passed = sum(results); print(f"GUI-QEMU SUMMARY pass={passed} fail={len(results) - passed}")
sys.exit(0 if passed == len(results) and results else 1)
