#!/usr/bin/env python3
"""Shared QEMU harness for the interactive-desktop tests.

It boots the real GUI initramfs in QEMU and injects input the way a display front-end (GTK/SDL/VNC) does:
QMP `input-send-event`. That goes through QEMU's input layer into whichever *guest* pointer device is attached, so the
guest kernel, evdev and the compositor see exactly what a physical mouse/tablet produces. (The older test used the HMP
`mouse_move` command, which only ever drives a relative PS/2 mouse.)

Pointer modes
  ps2      relative PS/2 mouse (QEMU default)           guest needs psmouse
  usb      absolute `usb-tablet` on the UHCI/XHCI bus    guest needs usbhid
  virtio   absolute `virtio-tablet-pci`                  guest needs virtio_input
Environment: KERNEL, INITRD (default build/aether-gui-initramfs.cpio.gz), QEMU (default: qemu-system-x86_64 on PATH).
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, threading, time

P = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ABS_MAX = 0x7FFF                      # QEMU's absolute axis range


def find_qemu():
    q = os.environ.get("QEMU") or shutil.which("qemu-system-x86_64")
    if not q:
        cand = f"{P}/build/hosttools/bin/qemu-system-x86_64"
        q = cand if os.path.exists(cand) else None
    return q


class Skip(Exception):
    pass


class Session:
    def __init__(self, pointer="ps2", mem=512, vga="std", initrd=None, extra_args=None, cmdline_extra="", shots=None):
        self.kernel = os.environ.get("KERNEL", f"{P}/build/kernel/bzImage")
        self.initrd = initrd or os.environ.get("INITRD", f"{P}/build/aether-gui-initramfs.cpio.gz")
        self.qemu = find_qemu()
        if not self.qemu or not os.path.exists(self.kernel) or not os.path.exists(self.initrd):
            raise Skip("need qemu-system-x86_64, a kernel and the GUI initramfs")
        self.pointer = pointer
        self.shots = shots or f"{P}/build/gui-shots"
        os.makedirs(self.shots, exist_ok=True)
        self.sock_path = tempfile.mktemp(prefix="aether-qmp-", suffix=".sock")
        args = [self.qemu, "-m", str(mem), "-vga", vga, "-display", "none", "-serial", "stdio", "-no-reboot",
                "-qmp", f"unix:{self.sock_path},server,nowait", "-kernel", self.kernel, "-initrd", self.initrd,
                "-append", "console=ttyS0 rdinit=/init panic=1 " + cmdline_extra]
        if pointer == "usb":
            args += ["-usb", "-device", "usb-tablet"]
        elif pointer == "virtio":
            args += ["-device", "virtio-tablet-pci"]
        elif pointer != "ps2":
            raise ValueError(pointer)
        args += (extra_args or [])
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.log, self.lock = [], threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()
        self.q = None
        self.W = self.H = 0
        self._btn_down = False

    # ---- serial console / log -------------------------------------------------------------------------------
    def _reader(self):
        for raw in iter(self.proc.stdout.readline, b""):
            with self.lock:
                self.log.append(raw.decode("utf-8", "replace").rstrip("\r\n"))

    def text(self):
        with self.lock:
            return "\n".join(self.log)

    def mark(self):
        with self.lock:
            return len(self.log)

    def text_since(self, mark):
        with self.lock:
            return "\n".join(self.log[mark:])

    def wait(self, pat, timeout=30, since=0):
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pat, self.text_since(since))
            if m:
                return m
            if self.proc.poll() is not None:
                break
            time.sleep(0.2)
        return None

    def serial(self, cmd):
        self.proc.stdin.write((cmd + "\n").encode())
        self.proc.stdin.flush()

    # ---- QMP -------------------------------------------------------------------------------------------------
    def _qmp_connect(self):
        for _ in range(100):
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(self.sock_path)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError("QMP socket never appeared")
        s.settimeout(10)
        self.q = s
        self._buf = b""
        self._qmp_read()                                            # greeting
        self.qmp("qmp_capabilities")

    def _qmp_read(self):
        while True:
            while b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                if line.strip():
                    j = json.loads(line)
                    if "event" in j:
                        continue
                    return j
            self._buf += self.q.recv(65536)

    def qmp(self, cmd, **args):
        if self.q is None:
            self._qmp_connect()
            if cmd == "qmp_capabilities":
                return {}
        self.q.sendall((json.dumps({"execute": cmd, "arguments": args}) + "\n").encode())
        r = self._qmp_read()
        if "error" in r:
            raise RuntimeError(f"QMP {cmd}: {r['error']}")
        return r.get("return")

    def hmp(self, cmd):
        return self.qmp("human-monitor-command", **{"command-line": cmd})

    # ---- input ------------------------------------------------------------------------------------------------
    def _events(self, evs):
        self.qmp("input-send-event", events=evs)

    def rel(self, dx, dy):
        """relative motion, split into small packets the way a real mouse reports them (PS/2 packets carry -256..255)"""
        while dx or dy:
            sx = max(-120, min(120, dx)); sy = max(-120, min(120, dy))
            self._events([{"type": "rel", "data": {"axis": "x", "value": sx}}, {"type": "rel", "data": {"axis": "y", "value": sy}}])
            dx -= sx; dy -= sy
            time.sleep(0.004)

    def abs_to(self, x, y):
        """absolute move to guest pixel (x, y) of the W x H display, in QEMU's 0..0x7fff tablet range"""
        ax = round(x * ABS_MAX / max(1, self.W - 1)); ay = round(y * ABS_MAX / max(1, self.H - 1))
        self._events([{"type": "abs", "data": {"axis": "x", "value": ax}}, {"type": "abs", "data": {"axis": "y", "value": ay}}])

    def button(self, down, which="left"):
        self._events([{"type": "btn", "data": {"down": bool(down), "button": which}}])

    def key(self, *keys, hold=0.05):
        """press and release a chord, e.g. key('alt','1')"""
        ks = [{"type": "qcode", "data": k} for k in keys]  # QMP wants key = {type: qcode, data: name}
        self._events([{"type": "key", "data": {"down": True, "key": k}} for k in ks])
        time.sleep(hold)
        self._events([{"type": "key", "data": {"down": False, "key": k}} for k in reversed(ks)])

    def type_text(self, s, delay=0.03):
        table = {" ": "spc", "-": "minus", "/": "slash", ".": "dot", "=": "equal", ",": "comma", ";": "semicolon", "'": "apostrophe",
                 "\n": "ret", "_": ("shift", "minus"), "|": ("shift", "backslash"), ">": ("shift", "dot"), "<": ("shift", "comma"),
                 "$": ("shift", "4"), "*": ("shift", "8"), "\"": ("shift", "apostrophe"), "~": ("shift", "grave_accent"), "&": ("shift", "7"),
                 "(": ("shift", "9"), ")": ("shift", "0"), ":": ("shift", "semicolon"), "?": ("shift", "slash"), "#": ("shift", "3"), "!": ("shift", "1")}
        for ch in s:
            k = table.get(ch)
            if k is None:
                k = ("shift", ch.lower()) if ch.isupper() else (ch,)
            self.key(*(k if isinstance(k, tuple) else (k,)))
            time.sleep(delay)

    # ---- pointer abstraction: same call works for every device type ---------------------------------------------
    def cursor(self):
        """locate the arrow sprite in a screendump -> (x, y) of its hot spot, or None"""
        img = self.shot("_cursor", keep=False)
        return find_cursor(img)

    def move_to(self, x, y):
        """Move the pointer to guest pixel (x, y).
        absolute devices: one absolute event.
        relative devices: closed loop on the *displayed* cursor (what a person does with a mouse): read where the sprite is,
        send the remaining delta, repeat until it is within 1 px."""
        if self.pointer in ("usb", "virtio"):
            self.abs_to(x, y)
            time.sleep(0.15)
            return
        for _ in range(12):
            c = self.cursor()
            if c is None:
                raise RuntimeError("cannot find the pointer on screen")
            dx, dy = x - c[0], y - c[1]
            if abs(dx) <= 1 and abs(dy) <= 1:
                return
            self.rel(dx, dy)
            time.sleep(0.15)
        raise RuntimeError(f"pointer did not converge on {(x, y)}; at {self.cursor()}")

    def click(self, x, y, wait=0.8, double=False):
        self.move_to(x, y)
        for _ in range(2 if double else 1):
            self.button(True); time.sleep(0.08); self.button(False); time.sleep(0.08 if double else 0)
        time.sleep(wait)

    def drag(self, x0, y0, x1, y1, steps=12):
        self.move_to(x0, y0)
        self.button(True); time.sleep(0.2)
        for i in range(1, steps + 1):
            x = x0 + (x1 - x0) * i // steps; y = y0 + (y1 - y0) * i // steps
            if self.pointer == "ps2":
                self.rel(x - (x0 + (x1 - x0) * (i - 1) // steps), y - (y0 + (y1 - y0) * (i - 1) // steps))
            else:
                self.abs_to(x, y)
            time.sleep(0.05)
        time.sleep(0.2)
        self.button(False); time.sleep(0.6)

    # ---- screenshots --------------------------------------------------------------------------------------------
    def shot(self, name, keep=True):
        f = f"{self.shots}/{name}.ppm"
        self.hmp(f"screendump {f}")
        for _ in range(50):
            if os.path.exists(f) and os.path.getsize(f) > 100:
                time.sleep(0.05)
                break
            time.sleep(0.1)
        d = open(f, "rb").read()
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", d)
        w, h = int(m.group(1)), int(m.group(2))
        img = (w, h, d[m.end():])
        try:
            if keep:
                from PIL import Image
                Image.frombytes("RGB", (w, h), img[2]).save(f"{self.shots}/{name}.png")
        except Exception:
            pass
        os.remove(f)
        return img

    def close(self):
        try:
            self.serial("poweroff")
        except Exception:
            pass
        time.sleep(1.5)
        try:
            self.proc.kill()
        except Exception:
            pass
        try:
            os.unlink(self.sock_path)
        except OSError:
            pass


# ---- image helpers -------------------------------------------------------------------------------------------------
def pix(img, x, y):
    w, h, px = img
    o = (y * w + x) * 3
    return (px[o], px[o + 1], px[o + 2])


def near(a, b, tol=6):
    return all(abs(p - q) <= tol for p, q in zip(a, b))


def count_diff(a, b, x0, y0, x1, y1):
    return sum(1 for y in range(y0, y1) for x in range(x0, x1) if pix(a, x, y) != pix(b, x, y))


def find_cursor(img):
    """The compositor's arrow: 12x16 sprite, outline TH_VOID (#080D18), fill TH_TEXT (#F2F6FF), tip at its top-left corner.
    Find the topmost-leftmost outline pixel that starts the sprite's signature column pattern."""
    w, h, px = img
    OUT = (0x08, 0x0D, 0x18); FILL = (0xF2, 0xF6, 0xFF)
    # fill pixels of the sprite: row 2 has 'X.X' -> fill at x+1; look for the unique vertical run of 7 fill pixels at x+1.. in rows 2..9
    best = None
    # fast pre-pass on 3-byte slices: collect candidate fill pixels
    fill = bytes(FILL)
    rows = {}
    for y in range(h):
        row = px[y * w * 3:(y + 1) * w * 3]
        xs = []
        i = row.find(fill)
        while i != -1:
            if i % 3 == 0:
                xs.append(i // 3)
                i = row.find(fill, i + 3)
            else:
                i = row.find(fill, i + 1)
        if xs:
            rows[y] = set(xs)
    # sprite fill layout relative to tip (tx,ty): row r has fill at x offsets given below
    layout = {2: [1], 3: [1, 2], 4: [1, 2, 3], 5: [1, 2, 3, 4], 6: [1, 2, 3, 4, 5], 7: [1, 2, 3, 4, 5, 6], 8: [1, 2, 3, 4, 5, 6, 7], 9: [1, 2, 3, 4, 5, 6, 7, 8]}
    for y, xs in rows.items():
        for x in xs:
            tx, ty = x - 1, y - 2                       # candidate tip given this pixel is row 2's fill
            if tx < 0 or ty < 0:
                continue
            ok = all(all((ty + r) in rows and (tx + o) in rows[ty + r] for o in offs) for r, offs in layout.items())
            if ok and near(pix(img, tx, ty), OUT, 2):
                best = (tx, ty)
                return best
    return best
