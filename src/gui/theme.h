/* Aether Aurora: the single source of truth for colour, geometry and typography tokens.
 * Everything that draws (compositor, shell, Settings, notifications) takes its values from here; nothing else
 * should contain a colour literal. tests/gui/gui_layout.py parses the plain #define lines of this file, so keep
 * numeric tokens as simple `#define NAME value` lines. */
#ifndef AETHER_THEME_H
#define AETHER_THEME_H
#include "gfx.h"
#include "proto.h"

/* ---- colour tokens (0xRRGGBB) --------------------------------------------------------------------------- */
#define TH_VOID        0x080D18u   /* wallpaper edges, deepest background */
#define TH_DESKTOP     0x0B1220u   /* main desktop surface, text-entry wells */
#define TH_PANEL       0x101A2Cu   /* top panel */
#define TH_WINDOW      0x131E31u   /* elevated window / menu / card surface */
#define TH_SURFACE2    0x1C2B43u   /* secondary surface: buttons, inactive-title lift, rows */
#define TH_TEXT        0xF2F6FFu   /* primary text */
#define TH_TEXT2       0xA7B5CCu   /* secondary text */
#define TH_MUTED       0x73839Du   /* muted / tertiary text, disabled glyphs */
#define TH_BLUE        0x398BFFu   /* Aether Blue: accent fills, focus */
#define TH_BLUE_HOVER  0x57A0FFu   /* accent hover */
#define TH_CYAN        0x63E5F5u   /* Aurora Cyan: accent highlights (never body text) */
#define TH_BORDER      0x2A3B54u   /* dividers and borders */
#define TH_OK          0x55D6A0u
#define TH_WARN        0xFFC66Du
#define TH_ERR         0xFF7185u

/* derived tokens: computed from the palette above, never separate literals */
#define TH_BLUE_PRESS     gfx_mix(TH_BLUE, TH_VOID, 56)          /* pressed accent */
#define TH_SURFACE2_HOVER gfx_mix(TH_SURFACE2, TH_BLUE, 34)      /* hovered secondary surface */
#define TH_SURFACE2_PRESS gfx_mix(TH_SURFACE2, TH_VOID, 70)
#define TH_TITLE_ACTIVE   TH_SURFACE2                            /* active window title bar */
#define TH_TITLE_INACTIVE TH_WINDOW                              /* inactive title bar merges with the window */
#define TH_BORDER_ACTIVE  gfx_mix(TH_BORDER, TH_BLUE, 110)       /* subtle blue-tinted rim on the focused window */
#define TH_CLOSE_HOVER    TH_ERR

/* ---- 4 px spacing scale ---------------------------------------------------------------------------------- */
#define TH_S1 4
#define TH_S2 8
#define TH_S3 12
#define TH_S4 16
#define TH_S5 20
#define TH_S6 24
#define TH_S8 32

/* ---- geometry (designed at 1280x800; callers clamp to the real display) --------------------------------- */
#define TH_PANEL_H      PANEL_HEIGHT   /* 38, defined in proto.h because the protocol needs it */
#define TH_PANEL_PAD    12
#define TH_TITLE_H      38
#define TH_R_WIN        10
#define TH_R_MENU       12
#define TH_R_CTRL       8
#define TH_CTRL_H       32
#define TH_ICONBTN      32
#define TH_TITLE_BTN    30
#define TH_WIN_PAD      20
#define TH_SHADOW_BLUR  12
#define TH_LAUNCHER_W   480
#define TH_LAUNCHER_H   560
#define TH_SEARCH_H     36
#define TH_TILE_W       104
#define TH_TILE_H       96
#define TH_TILE_ICON    56
#define TH_SETTINGS_W   760
#define TH_SETTINGS_H   520
#define TH_SIDEBAR_W    190
#define TH_NAV_H        36
#define TH_NAV_TOP      12
#define TH_NAV_PITCH    40
#define TH_ROW_H        44
#define TH_NOTIFY_W     340
#define TH_DESK_ICON    64
#define TH_DESK_CELL_W  96
#define TH_DESK_CELL_H  108
#define TH_DESK_MARGIN  24
#define TH_WS_BTN       26

/* ---- animation (milliseconds) ---------------------------------------------------------------------------- */
#define TH_ANIM_MENU_MS   140
#define TH_ANIM_NOTIFY_IN 120
#define TH_ANIM_NOTIFY_OUT 160
#define TH_FRAME_MS       16

/* ---- typography: Poppins Regular/Medium rasterised into font data (tools/gen-font-aa.py) ------------------ */
#define TY_CLOCK_PX   13   /* Medium */
#define TY_PANEL_PX   12   /* Medium */
#define TY_TITLE_PX   14   /* Medium: window titles */
#define TY_APP_PX     12   /* Regular: application names */
#define TY_BODY_PX    13   /* Regular: standard UI text */
#define TY_SMALL_PX   12   /* Regular: secondary / help */
#define TY_TINY_PX    11   /* Regular: captions */
#define TY_H1_PX      20   /* Medium: settings headings */

static inline int th_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int th_min(int a, int b) { return a < b ? a : b; }
static inline int th_max(int a, int b) { return a > b ? a : b; }
#endif
