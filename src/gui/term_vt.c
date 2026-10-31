#define _GNU_SOURCE
#include "term_vt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { S_GROUND, S_ESC, S_ESC_SKIP1, S_CSI, S_STR, S_STR_ESC };       /* parser states; S_STR = OSC/DCS/APC/PM string being swallowed */

static VtCell blank(const Vt *v) { VtCell c; memset(&c, 0, sizeof c); c.ch = ' '; c.fg = VT_DEFAULT; c.bg = v->bg; return c; }
static VtCell *at(const Vt *v, int r, int c) { return &v->grid[(size_t)r * (size_t)v->cols + (size_t)c]; }
static void clear_cells(Vt *v, int r, int c0, int c1)                        /* [c0, c1) on row r, erased with the current background */
{
    VtCell b = blank(v);
    for (int c = c0; c < c1; c++) *at(v, r, c) = b;
}
static void clear_wrap(Vt *v, int r) { v->wrap[r] = 0; }

static int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* ---- palette ---------------------------------------------------------------------------------------------------- */
uint32_t vt_palette(int i)
{
    static const uint32_t base[16] = {                                         /* Aurora-tinted ANSI colours; all readable on TH_DESKTOP */
        0x1C2B43, 0xFF7185, 0x55D6A0, 0xFFC66D, 0x398BFF, 0xC792EA, 0x63E5F5, 0xA7B5CC,
        0x4A5A75, 0xFF9AA9, 0x7BE8BA, 0xFFD98F, 0x57A0FF, 0xD9B3F5, 0x8FF0FA, 0xF2F6FF };
    if (i < 0) i = 0;
    if (i < 16) return base[i];
    if (i < 232) { int k = i - 16, r = k / 36, g = (k / 6) % 6, b = k % 6; static const int lv[6] = { 0, 95, 135, 175, 215, 255 }; return ((uint32_t)lv[r] << 16) | ((uint32_t)lv[g] << 8) | (uint32_t)lv[b]; }
    if (i > 255) i = 255;
    uint32_t g = (uint32_t)(8 + (i - 232) * 10); return (g << 16) | (g << 8) | g;
}
static int rgb_to_256(int r, int g, int b)
{
    r = clampi(r, 0, 255); g = clampi(g, 0, 255); b = clampi(b, 0, 255);
    int ri = (r + 25) / 51, gi = (g + 25) / 51, bi = (b + 25) / 51;           /* nearest of 6 evenly spaced levels (close enough to the xterm cube) */
    return 16 + 36 * clampi(ri, 0, 5) + 6 * clampi(gi, 0, 5) + clampi(bi, 0, 5);
}

/* ---- lifetime ----------------------------------------------------------------------------------------------------- */
static void set_tabs(Vt *v) { for (int c = 0; c < v->cols; c++) v->tabs[c] = (c % 8 == 0 && c) ? 1 : 0; }
static int alloc_screens(Vt *v, int cols, int rows)
{
    v->grid = calloc((size_t)cols * (size_t)rows, sizeof(VtCell)); v->alt_grid = calloc((size_t)cols * (size_t)rows, sizeof(VtCell));
    v->wrap = calloc((size_t)rows, 1); v->alt_wrap = calloc((size_t)rows, 1); v->tabs = calloc((size_t)cols, 1);
    return v->grid && v->alt_grid && v->wrap && v->alt_wrap && v->tabs ? 0 : -1;
}
void vt_reset(Vt *v)
{
    v->fg = VT_DEFAULT; v->bg = VT_DEFAULT; v->attr = 0; v->cx = v->cy = 0; v->wrap_pending = 0; v->top = 0; v->bot = v->rows - 1;
    v->cursor_visible = 1; v->app_cursor = 0; v->autowrap = 1; v->alt = 0; v->st = S_GROUND; v->np = 0; v->u8_left = 0; v->reply_n = 0;
    VtCell b = blank(v);
    for (size_t i = 0; i < (size_t)v->cols * (size_t)v->rows; i++) { v->grid[i] = b; v->alt_grid[i] = b; }
    memset(v->wrap, 0, (size_t)v->rows); memset(v->alt_wrap, 0, (size_t)v->rows); set_tabs(v);
    v->save_cx = v->save_cy = 0; v->save_fg = VT_DEFAULT; v->save_bg = VT_DEFAULT; v->save_attr = 0;
}
Vt *vt_new(int cols, int rows, int scrollback_lines)
{
    if (cols < 2 || rows < 1 || cols > 1000 || rows > 1000 || scrollback_lines < 0 || scrollback_lines > 100000) return NULL;
    Vt *v = calloc(1, sizeof *v); if (!v) return NULL;
    v->cols = cols; v->rows = rows; v->sb_cap = scrollback_lines;
    if (scrollback_lines) { v->sb = calloc((size_t)scrollback_lines, sizeof(VtLine)); if (!v->sb) { free(v); return NULL; } }
    if (alloc_screens(v, cols, rows) != 0) { vt_free(v); return NULL; }
    vt_reset(v); return v;
}
void vt_free(Vt *v)
{
    if (!v) return;
    for (int i = 0; i < v->sb_n; i++) free(v->sb[(v->sb_head + i) % v->sb_cap].c);
    free(v->sb); free(v->grid); free(v->alt_grid); free(v->wrap); free(v->alt_wrap); free(v->tabs); free(v);
}

/* ---- scrollback --------------------------------------------------------------------------------------------------- */
static void sb_push(Vt *v, const VtCell *row, int wrapped)
{
    if (v->sb_cap == 0) return;
    int len = v->cols; while (len > 0 && row[len - 1].ch == ' ' && row[len - 1].bg == VT_DEFAULT && !(row[len - 1].attr & VA_REVERSE)) len--;
    VtLine *slot;
    if (v->sb_n == v->sb_cap) { slot = &v->sb[v->sb_head]; free(slot->c); v->sb_head = (v->sb_head + 1) % v->sb_cap; }
    else { slot = &v->sb[(v->sb_head + v->sb_n) % v->sb_cap]; v->sb_n++; }
    slot->c = len ? malloc((size_t)len * sizeof(VtCell)) : NULL;
    if (len && !slot->c) len = 0;
    if (len) memcpy(slot->c, row, (size_t)len * sizeof(VtCell));
    slot->len = len; slot->wrapped = wrapped;
}
static void sb_clear(Vt *v) { for (int i = 0; i < v->sb_n; i++) { VtLine *l = &v->sb[(v->sb_head + i) % v->sb_cap]; free(l->c); l->c = NULL; } v->sb_n = 0; v->sb_head = 0; }

/* ---- scrolling ---------------------------------------------------------------------------------------------------- */
static void scroll_up(Vt *v, int n)                                             /* within the scroll region */
{
    int h = v->bot - v->top + 1; n = clampi(n, 0, h);
    for (int i = 0; i < n; i++) if (v->top == 0 && !v->alt) sb_push(v, at(v, i, 0), v->wrap[i]);
    if (n == 0) return;
    memmove(at(v, v->top, 0), at(v, v->top + n, 0), (size_t)(h - n) * (size_t)v->cols * sizeof(VtCell));
    memmove(&v->wrap[v->top], &v->wrap[v->top + n], (size_t)(h - n));
    for (int r = v->bot - n + 1; r <= v->bot; r++) { clear_cells(v, r, 0, v->cols); clear_wrap(v, r); }
}
static void scroll_down(Vt *v, int n)
{
    int h = v->bot - v->top + 1; n = clampi(n, 0, h); if (n == 0) return;
    memmove(at(v, v->top + n, 0), at(v, v->top, 0), (size_t)(h - n) * (size_t)v->cols * sizeof(VtCell));
    memmove(&v->wrap[v->top + n], &v->wrap[v->top], (size_t)(h - n));
    for (int r = v->top; r < v->top + n; r++) { clear_cells(v, r, 0, v->cols); clear_wrap(v, r); }
}
static void linefeed(Vt *v) { if (v->cy == v->bot) scroll_up(v, 1); else if (v->cy < v->rows - 1) v->cy++; }
static void reverse_index(Vt *v) { if (v->cy == v->top) scroll_down(v, 1); else if (v->cy > 0) v->cy--; }

static void put(Vt *v, uint32_t cp)
{
    if (v->wrap_pending) {
        if (v->autowrap) { v->wrap[v->cy] = 1; v->cx = 0; linefeed(v); }
        v->wrap_pending = 0;
    }
    VtCell *c = at(v, v->cy, v->cx);
    c->ch = cp > 0xFFFF ? 0xFFFD : (uint16_t)cp; c->fg = v->fg; c->bg = v->bg; c->attr = v->attr; c->pad = 0;
    if (v->cx == v->cols - 1) v->wrap_pending = v->autowrap ? 1 : 0; else v->cx++;
}

/* ---- screens (alt screen swap) ------------------------------------------------------------------------------------ */
static void swap_screens(Vt *v)
{
    VtCell *g = v->grid; v->grid = v->alt_grid; v->alt_grid = g;
    uint8_t *w = v->wrap; v->wrap = v->alt_wrap; v->alt_wrap = w; v->alt = !v->alt;
}
static void enter_alt(Vt *v, int save)
{
    if (v->alt) return;
    if (save) { v->save_cx = v->cx; v->save_cy = v->cy; v->save_fg = v->fg; v->save_bg = v->bg; v->save_attr = v->attr; }
    swap_screens(v); VtCell b = blank(v);
    for (size_t i = 0; i < (size_t)v->cols * (size_t)v->rows; i++) v->grid[i] = b;
    memset(v->wrap, 0, (size_t)v->rows);
    v->cx = v->cy = 0; v->wrap_pending = 0; v->top = 0; v->bot = v->rows - 1;
}
static void leave_alt(Vt *v, int restore)
{
    if (!v->alt) return;
    swap_screens(v); v->wrap_pending = 0; v->top = 0; v->bot = v->rows - 1;
    if (restore) { v->cx = clampi(v->save_cx, 0, v->cols - 1); v->cy = clampi(v->save_cy, 0, v->rows - 1); v->fg = v->save_fg; v->bg = v->save_bg; v->attr = v->save_attr; }
}

/* ---- CSI ---------------------------------------------------------------------------------------------------------- */
static void reply(Vt *v, const char *s) { size_t n = strlen(s); if ((size_t)v->reply_n + n < sizeof v->reply) { memcpy(v->reply + v->reply_n, s, n); v->reply_n += (int)n; } }
static int arg(const Vt *v, int i, int def) { return (i < v->np && v->params[i] > 0) ? v->params[i] : def; }          /* 0 / missing -> default */
static int arg0(const Vt *v, int i) { return (i < v->np && v->params[i] > 0) ? v->params[i] : 0; }

static void sgr(Vt *v)
{
    if (v->np == 0) { v->fg = VT_DEFAULT; v->bg = VT_DEFAULT; v->attr = 0; return; }
    for (int i = 0; i < v->np; i++) {
        int p = v->params[i] < 0 ? 0 : v->params[i];
        if (p == 0) { v->fg = VT_DEFAULT; v->bg = VT_DEFAULT; v->attr = 0; }
        else if (p == 1) v->attr |= VA_BOLD;
        else if (p == 4) v->attr |= VA_UNDERLINE;
        else if (p == 7) v->attr |= VA_REVERSE;
        else if (p == 22) v->attr = (uint8_t)(v->attr & ~VA_BOLD);
        else if (p == 24) v->attr = (uint8_t)(v->attr & ~VA_UNDERLINE);
        else if (p == 27) v->attr = (uint8_t)(v->attr & ~VA_REVERSE);
        else if (p >= 30 && p <= 37) v->fg = (uint16_t)(p - 30);
        else if (p == 39) v->fg = VT_DEFAULT;
        else if (p >= 40 && p <= 47) v->bg = (uint16_t)(p - 40);
        else if (p == 49) v->bg = VT_DEFAULT;
        else if (p >= 90 && p <= 97) v->fg = (uint16_t)(p - 90 + 8);
        else if (p >= 100 && p <= 107) v->bg = (uint16_t)(p - 100 + 8);
        else if (p == 38 || p == 48) {
            uint16_t *dst = p == 38 ? &v->fg : &v->bg;
            if (i + 2 < v->np && v->params[i + 1] == 5) { *dst = (uint16_t)clampi(v->params[i + 2], 0, 255); i += 2; }
            else if (i + 4 < v->np && v->params[i + 1] == 2) { *dst = (uint16_t)rgb_to_256(v->params[i + 2], v->params[i + 3], v->params[i + 4]); i += 4; }
            else break;                                                         /* malformed extended colour: stop, do not misread the rest */
        }
    }
}
static void private_mode(Vt *v, int set)
{
    for (int i = 0; i < v->np; i++) switch (v->params[i]) {
    case 1: v->app_cursor = set; break;
    case 7: v->autowrap = set; if (!set) v->wrap_pending = 0; break;
    case 25: v->cursor_visible = set; break;
    case 47: case 1047: if (set) enter_alt(v, 0); else leave_alt(v, 0); break;
    case 1049: if (set) enter_alt(v, 1); else leave_alt(v, 1); break;
    default: break;
    }
}
static void csi(Vt *v, int final)
{
    if (v->priv == '?') { if (final == 'h') private_mode(v, 1); else if (final == 'l') private_mode(v, 0); return; }
    if (v->priv) return;                                                        /* CSI > ... / CSI = ... / CSI < ...: not supported, ignore */
    int n;
    switch (final) {
    case 'A': v->cy = clampi(v->cy - arg(v, 0, 1), v->cy >= v->top ? v->top : 0, v->rows - 1); v->wrap_pending = 0; break;
    case 'B': case 'e': v->cy = clampi(v->cy + arg(v, 0, 1), 0, v->cy <= v->bot ? v->bot : v->rows - 1); v->wrap_pending = 0; break;
    case 'C': case 'a': v->cx = clampi(v->cx + arg(v, 0, 1), 0, v->cols - 1); v->wrap_pending = 0; break;
    case 'D': v->cx = clampi(v->cx - arg(v, 0, 1), 0, v->cols - 1); v->wrap_pending = 0; break;
    case 'E': v->cy = clampi(v->cy + arg(v, 0, 1), 0, v->rows - 1); v->cx = 0; v->wrap_pending = 0; break;
    case 'F': v->cy = clampi(v->cy - arg(v, 0, 1), 0, v->rows - 1); v->cx = 0; v->wrap_pending = 0; break;
    case 'G': case '`': v->cx = clampi(arg(v, 0, 1) - 1, 0, v->cols - 1); v->wrap_pending = 0; break;
    case 'd': v->cy = clampi(arg(v, 0, 1) - 1, 0, v->rows - 1); v->wrap_pending = 0; break;
    case 'H': case 'f': v->cy = clampi(arg(v, 0, 1) - 1, 0, v->rows - 1); v->cx = clampi(arg(v, 1, 1) - 1, 0, v->cols - 1); v->wrap_pending = 0; break;
    case 'J':
        n = arg0(v, 0); v->wrap_pending = 0;
        if (n == 0) { clear_cells(v, v->cy, v->cx, v->cols); for (int r = v->cy + 1; r < v->rows; r++) { clear_cells(v, r, 0, v->cols); clear_wrap(v, r); } }
        else if (n == 1) { for (int r = 0; r < v->cy; r++) { clear_cells(v, r, 0, v->cols); clear_wrap(v, r); } clear_cells(v, v->cy, 0, v->cx + 1); }
        else if (n == 2 || n == 3) { for (int r = 0; r < v->rows; r++) { clear_cells(v, r, 0, v->cols); clear_wrap(v, r); } if (n == 3) sb_clear(v); }
        break;
    case 'K':
        n = arg0(v, 0); v->wrap_pending = 0;
        if (n == 0) clear_cells(v, v->cy, v->cx, v->cols); else if (n == 1) clear_cells(v, v->cy, 0, v->cx + 1); else if (n == 2) clear_cells(v, v->cy, 0, v->cols);
        if (n == 0 || n == 2) clear_wrap(v, v->cy);
        break;
    case 'X': clear_cells(v, v->cy, v->cx, clampi(v->cx + arg(v, 0, 1), 0, v->cols)); break;
    case '@': {                                                                 /* insert blanks, shifting the rest of the row right */
        n = clampi(arg(v, 0, 1), 1, v->cols - v->cx); VtCell *row = at(v, v->cy, 0);
        memmove(row + v->cx + n, row + v->cx, (size_t)(v->cols - v->cx - n) * sizeof(VtCell)); clear_cells(v, v->cy, v->cx, v->cx + n); v->wrap_pending = 0; break; }
    case 'P': {
        n = clampi(arg(v, 0, 1), 1, v->cols - v->cx); VtCell *row = at(v, v->cy, 0);
        memmove(row + v->cx, row + v->cx + n, (size_t)(v->cols - v->cx - n) * sizeof(VtCell)); clear_cells(v, v->cy, v->cols - n, v->cols); v->wrap_pending = 0; break; }
    case 'L': if (v->cy >= v->top && v->cy <= v->bot) { int ot = v->top; v->top = v->cy; scroll_down(v, arg(v, 0, 1)); v->top = ot; v->cx = 0; v->wrap_pending = 0; } break;
    case 'M': if (v->cy >= v->top && v->cy <= v->bot) { int ot = v->top; v->top = v->cy; scroll_up(v, arg(v, 0, 1)); v->top = ot; v->cx = 0; v->wrap_pending = 0; } break;
    case 'S': scroll_up(v, arg(v, 0, 1)); break;
    case 'T': scroll_down(v, arg(v, 0, 1)); break;
    case 'm': sgr(v); break;
    case 'r': {
        int t = arg(v, 0, 1) - 1, b = arg(v, 1, v->rows) - 1;
        if (t >= 0 && b < v->rows && t < b) { v->top = t; v->bot = b; } else if (v->np == 0) { v->top = 0; v->bot = v->rows - 1; }
        v->cx = 0; v->cy = 0; v->wrap_pending = 0; break; }
    case 's': v->save_cx = v->cx; v->save_cy = v->cy; v->save_fg = v->fg; v->save_bg = v->bg; v->save_attr = v->attr; break;
    case 'u': v->cx = clampi(v->save_cx, 0, v->cols - 1); v->cy = clampi(v->save_cy, 0, v->rows - 1); v->fg = v->save_fg; v->bg = v->save_bg; v->attr = v->save_attr; v->wrap_pending = 0; break;
    case 'g': if (arg0(v, 0) == 0) v->tabs[v->cx] = 0; else if (arg0(v, 0) == 3) memset(v->tabs, 0, (size_t)v->cols); break;
    case 'n':
        if (arg0(v, 0) == 5) reply(v, "\x1b[0n");
        else if (arg0(v, 0) == 6) { char b[32]; snprintf(b, sizeof b, "\x1b[%d;%dR", v->cy + 1, v->cx + 1); reply(v, b); }
        break;
    case 'c': if (arg0(v, 0) == 0) reply(v, "\x1b[?6c"); break;                  /* "VT102" */
    default: break;
    }
}

/* ---- parser ------------------------------------------------------------------------------------------------------- */
static void c0(Vt *v, unsigned char b)
{
    switch (b) {
    case 0x08: if (v->cx > 0) v->cx--; v->wrap_pending = 0; break;               /* BS */
    case 0x09: { int c = v->cx + 1; while (c < v->cols - 1 && !v->tabs[c]) c++; v->cx = clampi(c, 0, v->cols - 1); v->wrap_pending = 0; break; }   /* HT */
    case 0x0a: case 0x0b: case 0x0c: v->wrap_pending = 0; linefeed(v); break;    /* LF VT FF */
    case 0x0d: v->cx = 0; v->wrap_pending = 0; break;                            /* CR */
    default: break;                                                              /* BEL, SO, SI, ... */
    }
}
static void ground(Vt *v, unsigned char b)
{
    if (v->u8_left) {                                                            /* UTF-8 continuation */
        if ((b & 0xC0) == 0x80) { v->u8 = (v->u8 << 6) | (b & 0x3Fu); if (--v->u8_left == 0) put(v, v->u8 < 0x20 || (v->u8 >= 0x7F && v->u8 < 0xA0) ? '?' : v->u8); return; }
        v->u8_left = 0; put(v, 0xFFFD);                                          /* truncated sequence: show one replacement, then reprocess this byte */
    }
    if (b >= 0x20 && b < 0x7F) put(v, b);
    else if (b < 0x20) { if (b == 0x1b) { v->st = S_ESC; } else c0(v, b); }
    else if (b >= 0xC2 && b <= 0xDF) { v->u8 = b & 0x1Fu; v->u8_left = 1; }
    else if (b >= 0xE0 && b <= 0xEF) { v->u8 = b & 0x0Fu; v->u8_left = 2; }
    else if (b >= 0xF0 && b <= 0xF4) { v->u8 = b & 0x07u; v->u8_left = 3; }
    else if (b >= 0x80) put(v, 0xFFFD);
    else if (b == 0x7F) { }
}
static void feed_byte(Vt *v, unsigned char b)
{
    if (v->st != S_GROUND && v->st != S_STR && v->st != S_STR_ESC) {              /* controls act even inside escape sequences */
        if (b == 0x18 || b == 0x1a) { v->st = S_GROUND; return; }                /* CAN / SUB abort */
        if (b == 0x1b) { v->st = S_ESC; return; }                                /* ESC restarts */
        if (b < 0x20) { c0(v, b); return; }
    }
    switch (v->st) {
    case S_GROUND: ground(v, b); break;
    case S_ESC:
        v->st = S_GROUND;
        switch (b) {
        case '[': v->st = S_CSI; v->np = 0; v->priv = 0; v->inter = 0; memset(v->params, 0, sizeof v->params); v->params[0] = -1; break;
        case ']': case 'P': case '_': case '^': case 'X': v->st = S_STR; break;
        case '(': case ')': case '*': case '+': case '#': case '%': v->st = S_ESC_SKIP1; break;
        case '7': v->save_cx = v->cx; v->save_cy = v->cy; v->save_fg = v->fg; v->save_bg = v->bg; v->save_attr = v->attr; break;
        case '8': v->cx = clampi(v->save_cx, 0, v->cols - 1); v->cy = clampi(v->save_cy, 0, v->rows - 1); v->fg = v->save_fg; v->bg = v->save_bg; v->attr = v->save_attr; v->wrap_pending = 0; break;
        case 'D': v->wrap_pending = 0; linefeed(v); break;
        case 'E': v->cx = 0; v->wrap_pending = 0; linefeed(v); break;
        case 'M': v->wrap_pending = 0; reverse_index(v); break;
        case 'H': v->tabs[v->cx] = 1; break;
        case 'c': vt_reset(v); break;
        default: break;                                                          /* ESC =, ESC >, ... */
        }
        break;
    case S_ESC_SKIP1: v->st = S_GROUND; break;
    case S_CSI:
        if (b >= '0' && b <= '9') { if (v->np == 0) v->np = 1; int *p = &v->params[v->np - 1]; if (*p < 0) *p = 0; if (*p < 100000) *p = *p * 10 + (b - '0'); }
        else if (b == ';' || b == ':') { if (v->np == 0) v->np = 1; if (v->np < 16) { v->params[v->np] = -1; v->np++; } }
        else if (b >= '<' && b <= '?') { v->priv = b; }
        else if (b >= 0x20 && b <= 0x2f) { v->inter = b; }
        else if (b >= 0x40 && b <= 0x7e) { v->st = S_GROUND; if (!v->inter) csi(v, b); }
        else v->st = S_GROUND;                                                   /* garbage: abandon the sequence */
        break;
    case S_STR:
        if (b == 0x07) v->st = S_GROUND; else if (b == 0x1b) v->st = S_STR_ESC;
        break;
    case S_STR_ESC: v->st = (b == '\\') ? S_GROUND : S_STR; break;
    default: v->st = S_GROUND; break;
    }
}
void vt_feed(Vt *v, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)data[i];
        if (v->st == S_STR && b == 0x18) { v->st = S_GROUND; continue; }
        feed_byte(v, b);
    }
}
size_t vt_take_reply(Vt *v, char *out, size_t cap)
{
    size_t n = (size_t)v->reply_n < cap ? (size_t)v->reply_n : cap;
    memcpy(out, v->reply, n); memmove(v->reply, v->reply + n, (size_t)v->reply_n - n); v->reply_n -= (int)n; return n;
}

/* ---- resize ------------------------------------------------------------------------------------------------------- */
void vt_resize(Vt *v, int cols, int rows)
{
    if (cols < 2 || rows < 1 || cols > 1000 || rows > 1000 || (cols == v->cols && rows == v->rows)) return;
    VtCell *ng = calloc((size_t)cols * (size_t)rows, sizeof(VtCell)), *na = calloc((size_t)cols * (size_t)rows, sizeof(VtCell));
    uint8_t *nw = calloc((size_t)rows, 1), *nwa = calloc((size_t)rows, 1), *nt = calloc((size_t)cols, 1);
    if (!ng || !na || !nw || !nwa || !nt) { free(ng); free(na); free(nw); free(nwa); free(nt); return; }
    VtCell b = blank(v);
    for (size_t i = 0; i < (size_t)cols * (size_t)rows; i++) { ng[i] = b; na[i] = b; }
    /* the active screen keeps its bottom-most content: surplus top rows move into the scrollback (normal screen only) */
    int shift = 0; if (rows < v->rows && v->cy >= rows) shift = v->cy - rows + 1;
    for (int pass = 0; pass < 2; pass++) {
        VtCell *src = pass == 0 ? v->grid : v->alt_grid, *dst = pass == 0 ? ng : na; uint8_t *sw = pass == 0 ? v->wrap : v->alt_wrap, *dw = pass == 0 ? nw : nwa;
        int isactive = (pass == 0), sh = isactive ? shift : 0;
        for (int r = 0; r < sh; r++) if (isactive && !v->alt) sb_push(v, &src[(size_t)r * (size_t)v->cols], sw[r]);
        for (int r = sh; r < v->rows && r - sh < rows; r++) {
            int cc = cols < v->cols ? cols : v->cols;
            memcpy(&dst[(size_t)(r - sh) * (size_t)cols], &src[(size_t)r * (size_t)v->cols], (size_t)cc * sizeof(VtCell)); dw[r - sh] = sw[r];
        }
    }
    free(v->grid); free(v->alt_grid); free(v->wrap); free(v->alt_wrap); free(v->tabs);
    v->grid = ng; v->alt_grid = na; v->wrap = nw; v->wrap_pending = 0; v->alt_wrap = nwa; v->tabs = nt;
    v->cy -= shift; v->cols = cols; v->rows = rows; v->top = 0; v->bot = rows - 1;
    v->cx = clampi(v->cx, 0, cols - 1); v->cy = clampi(v->cy, 0, rows - 1); set_tabs(v);
    v->save_cx = clampi(v->save_cx, 0, cols - 1); v->save_cy = clampi(v->save_cy, 0, rows - 1);
}

/* ---- reading back ------------------------------------------------------------------------------------------------- */
const VtCell *vt_cell(const Vt *v, int row, int col) { return (row < 0 || col < 0 || row >= v->rows || col >= v->cols) ? NULL : at(v, row, col); }
int vt_abs_lines(const Vt *v) { return v->alt ? v->rows : v->sb_n + v->rows; }
static int sb_base(const Vt *v) { return v->alt ? 0 : v->sb_n; }
VtCell vt_abs_cell(const Vt *v, int line, int col)
{
    VtCell b; memset(&b, 0, sizeof b); b.ch = ' '; b.fg = VT_DEFAULT; b.bg = VT_DEFAULT;
    if (line < 0 || col < 0) return b;
    int base = sb_base(v);
    if (line >= base) { int r = line - base; return r < v->rows && col < v->cols ? *at(v, r, col) : b; }
    const VtLine *l = &v->sb[(v->sb_head + line) % v->sb_cap];
    return col < l->len ? l->c[col] : b;
}
int vt_abs_wrapped(const Vt *v, int line)
{
    if (line < 0) return 0;
    int base = sb_base(v);
    if (line >= base) return line - base < v->rows ? v->wrap[line - base] : 0;
    return v->sb[(v->sb_head + line) % v->sb_cap].wrapped;
}
static size_t enc(uint32_t cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
}
size_t vt_text(const Vt *v, int l0, int c0_, int l1, int c1, char *out, size_t cap)
{
    size_t n = 0; if (cap == 0) return 0;
    int nl = vt_abs_lines(v);
    if (l0 > l1 || (l0 == l1 && c0_ > c1)) { int tl = l0, tc = c0_; l0 = l1; c0_ = c1; l1 = tl; c1 = tc; }
    l0 = clampi(l0, 0, nl - 1); l1 = clampi(l1, 0, nl - 1);
    for (int l = l0; l <= l1; l++) {
        int a = l == l0 ? c0_ : 0, z = l == l1 ? c1 : v->cols - 1; a = clampi(a, 0, v->cols - 1); z = clampi(z, 0, v->cols - 1);
        int last = z; while (last >= a && vt_abs_cell(v, l, last).ch == ' ') last--;           /* trim trailing blanks */
        for (int c = a; c <= last; c++) { char t[4]; size_t k = enc(vt_abs_cell(v, l, c).ch, t); if (n + k + 1 >= cap) goto done; memcpy(out + n, t, k); n += k; }
        if (l < l1 && !vt_abs_wrapped(v, l)) { if (n + 2 >= cap) goto done; out[n++] = '\n'; }
    }
done:
    out[n] = 0; return n;
}
static int word_char(uint16_t ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '-' || ch == '.' || ch == '/' || ch == '~' || ch == ':' || ch == '@' || ch == '%' || ch == '+' || ch == '=' || ch == ',' || ch > 0x7f; }
int vt_word_bounds(const Vt *v, int line, int col, int *c0_, int *c1)
{
    if (line < 0 || line >= vt_abs_lines(v) || col < 0 || col >= v->cols || !word_char(vt_abs_cell(v, line, col).ch)) return 0;
    int a = col, z = col;
    while (a > 0 && word_char(vt_abs_cell(v, line, a - 1).ch)) a--;
    while (z < v->cols - 1 && word_char(vt_abs_cell(v, line, z + 1).ch)) z++;
    *c0_ = a; *c1 = z; return 1;
}
