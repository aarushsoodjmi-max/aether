"""Layout/colour constants for GUI tests, parsed from src/gui/theme.h so tests and code cannot drift apart."""
import os, re

_HERE = os.path.dirname(os.path.abspath(__file__))
THEME = os.path.join(_HERE, "..", "..", "src", "gui", "theme.h")
PROTO = os.path.join(_HERE, "..", "..", "src", "gui", "proto.h")

def _parse():
    vals = {}
    for path in (PROTO, THEME):
        for line in open(path):
            m = re.match(r"#define\s+((?:TH|TY|PANEL)_?[A-Z0-9_]*)\s+(0x[0-9A-Fa-f]+)u?\b", line)
            if m: vals[m.group(1)] = int(m.group(2), 16); continue
            m = re.match(r"#define\s+((?:TH|TY|PANEL)_?[A-Z0-9_]*)\s+(\d+)\b", line)
            if m: vals[m.group(1)] = int(m.group(2))
    return vals
T = _parse()
def rgb(name):
    v = T[name]; return ((v >> 16) & 255, (v >> 8) & 255, v & 255)
def mix(a, b, t):          # same arithmetic as gfx_mix (t in 0..256)
    return tuple(((x * (256 - t)) + (y * t)) >> 8 for x, y in zip(a, b))

PANEL_H = T["PANEL_HEIGHT"]; TITLE_H = T["TH_TITLE_H"]; BTN = T["TH_TITLE_BTN"]; S1, S2 = T["TH_S1"], T["TH_S2"]
TITLE_ACTIVE = rgb("TH_SURFACE2"); TITLE_INACTIVE = rgb("TH_WINDOW")

def close_btn(w):   # window = dict(x,y,w,h) of the client area: centre of each title-bar control
    cy = w["y"] - TITLE_H + TITLE_H // 2
    return (w["x"] + w["w"] - S2 - BTN // 2, cy)
def max_btn(w):  c = close_btn(w); return (c[0] - (BTN + S1), c[1])
def min_btn(w, resizable=True): c = close_btn(w); return (c[0] - (BTN + S1) * (2 if resizable else 1), c[1])
def title_point(w, dx=60): return (w["x"] + dx, w["y"] - TITLE_H // 2)
def nav_item(w, i):   # Settings sidebar item centre
    return (w["x"] + T["TH_S2"] + (T["TH_SIDEBAR_W"] - 2 * T["TH_S2"]) // 2, w["y"] + T["TH_NAV_TOP"] + i * T["TH_NAV_PITCH"] + T["TH_NAV_H"] // 2)
