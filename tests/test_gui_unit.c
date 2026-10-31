/* Unit tests for the Aether GUI modules: gfx, appmeta, input keymap, protocol, window manager. */
#define _GNU_SOURCE
#include "appmeta.h"
#include "ctxmenu.h"
#include "gfx.h"
#include "input.h"
#include "proto.h"
#include "wm.h"
#include "theme.h"
#include "font.h"
#include "icons.h"
#include "wallpaper.h"
#include "term_vt.h"
#include "fm_core.h"
#include <math.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>

static int fails, total;
#define T(cond, name) do { total++; if (cond) printf("PASS %s\n", name); else { printf("FAIL %s (line %d)\n", name, __LINE__); fails++; } } while (0)

static Surface mk(int w, int h) { Surface s = { calloc((size_t)w * (size_t)h, 4), w, h, w }; return s; }
static uint32_t at(const Surface *s, int x, int y) { return s->px[(size_t)y * (size_t)s->stride + (size_t)x]; }

static void test_gfx(void)
{
    Surface s = mk(20, 10);
    gfx_fill(&s, rect_make(2, 2, 5, 3), 0xff0000);
    T(at(&s, 2, 2) == 0xff0000 && at(&s, 6, 4) == 0xff0000 && at(&s, 7, 4) == 0 && at(&s, 2, 5) == 0, "gfx: fill covers exactly the rect");
    gfx_fill(&s, rect_make(-5, -5, 100, 100), 0x00ff00); T(at(&s, 0, 0) == 0x00ff00 && at(&s, 19, 9) == 0x00ff00, "gfx: oversize fill is clipped, no overflow");
    gfx_fill(&s, rect_make(50, 50, 5, 5), 1); gfx_fill(&s, rect_make(3, 3, -2, 4), 1); T(at(&s, 3, 3) == 0x00ff00, "gfx: off-screen/negative rect draws nothing");
    Rect r = rect_intersect(rect_make(0, 0, 10, 10), rect_make(5, 5, 10, 10)); T(r.x == 5 && r.y == 5 && r.w == 5 && r.h == 5, "rect: intersect");
    r = rect_intersect(rect_make(0, 0, 4, 4), rect_make(10, 10, 4, 4)); T(r.w == 0 && r.h == 0, "rect: disjoint intersect is empty");
    T(rect_contains(rect_make(1, 1, 3, 3), 3, 3) && !rect_contains(rect_make(1, 1, 3, 3), 4, 3), "rect: contains is half-open");
    Surface src = mk(4, 4); gfx_fill(&src, rect_make(0, 0, 4, 4), 0x123456); Surface dst = mk(6, 6);
    gfx_blit(&dst, -2, -2, &src, rect_make(0, 0, 4, 4)); T(at(&dst, 0, 0) == 0x123456 && at(&dst, 1, 1) == 0x123456 && at(&dst, 2, 2) == 0, "gfx: blit with negative destination is clipped");
    gfx_blit(&dst, 4, 4, &src, rect_make(0, 0, 4, 4)); T(at(&dst, 5, 5) == 0x123456 && at(&dst, 3, 3) == 0, "gfx: blit past the edge is clipped");
    gfx_blit(&dst, 0, 0, &src, rect_make(-10, -10, 100, 100)); T(1, "gfx: oversize source rect is clipped (sanitizer-checked)");
    T(gfx_text_width("abc") == 3 * FONT_W, "gfx: text width is advance * length");
    Surface t = mk(64, 20); gfx_text(&t, 0, 0, "A", 0xffffff); int lit = 0; for (int y = 0; y < 20; y++) for (int x = 0; x < 64; x++) if (at(&t, x, y)) lit++;
    T(lit > 15 && lit < 80, "gfx: glyph 'A' draws a plausible number of pixels");
    int outside = 0; for (int y = 0; y < 20; y++) for (int x = FONT_W; x < 64; x++) if (at(&t, x, y)) outside++; T(outside == 0, "gfx: glyph stays inside its 8x16 cell");
    Surface c = mk(64, 20); gfx_text_clipped(&c, rect_make(0, 0, 4, 20), 0, 0, "MMMM", 0xffffff); outside = 0; for (int y = 0; y < 20; y++) for (int x = 4; x < 64; x++) if (at(&c, x, y)) outside++; T(outside == 0, "gfx: clipped text respects the clip rect");
    Surface u = mk(8, 8); gfx_text(&u, 0, 0, "\x01\xff", 0xffffff); T(1, "gfx: non-ASCII bytes render as '?' (sanitizer-checked, no out-of-bounds font read)");
    Surface g = mk(4, 11); gfx_gradient_v(&g, rect_make(0, 0, 4, 11), 0x000000, 0xff8040); T(at(&g, 0, 0) == 0 && (at(&g, 0, 10) & 0xff0000) >= 0xfe0000, "gfx: gradient reaches both end colours");
    free(s.px); free(src.px); free(dst.px); free(t.px); free(c.px); free(u.px); free(g.px);
}

static void test_appmeta(void)
{
    AppEntry a;
    T(app_parse("[Aether App]\nname=Files\ncomment=Browse\nexec=/bin/files\n", &a) == 0 && !strcmp(a.name, "Files") && !strcmp(a.exec, "/bin/files") && !strcmp(a.comment, "Browse"), "app: valid entry parses");
    app_parse("# c\n\n[Aether App]\r\nname = x\nexec=/bin/x\n", &a);   /* odd whitespace must not crash (sanitizer-checked) */
    T(app_parse("[Aether App]\nname=Files\n", &a) != 0, "app: missing exec rejected");
    T(app_parse("[Aether App]\nexec=/bin/x\n", &a) != 0, "app: missing name rejected");
    T(app_parse("[Aether App]\nname=X\nexec=bin/x\n", &a) != 0, "app: relative exec path rejected");
    T(app_parse("[Other]\nname=X\nexec=/bin/x\n", &a) != 0, "app: wrong section rejected");
    T(app_parse("name=X\nexec=/bin/x\n", &a) != 0, "app: missing header rejected");
    char longv[400]; memset(longv, 'a', sizeof longv - 1); longv[sizeof longv - 1] = 0; char big[900]; snprintf(big, sizeof big, "[Aether App]\nname=%s\nexec=/bin/x\n", longv);
    T(app_parse(big, &a) == 0 && strlen(a.name) < sizeof a.name, "app: oversized fields are truncated safely");
    char d[] = "/tmp/aether-appunit-XXXXXX"; if (!mkdtemp(d)) exit(2); char p[300], cmd[400];
    snprintf(p, sizeof p, "%s/b.app", d); FILE *f = fopen(p, "w"); fputs("[Aether App]\nname=Beta\nexec=/bin/b\n", f); fclose(f);
    snprintf(p, sizeof p, "%s/a.app", d); f = fopen(p, "w"); fputs("[Aether App]\nname=Alpha\nexec=/bin/a\n", f); fclose(f);
    snprintf(p, sizeof p, "%s/bad.app", d); f = fopen(p, "w"); fputs("garbage\n", f); fclose(f);
    snprintf(p, sizeof p, "%s/skip.txt", d); f = fopen(p, "w"); fputs("[Aether App]\nname=No\nexec=/bin/n\n", f); fclose(f);
    AppEntry list[8]; size_t n = app_load_dir(d, list, 8); T(n == 2 && !strcmp(list[0].name, "Alpha") && !strcmp(list[1].name, "Beta"), "app: directory load is sorted and skips invalid / non-.app files");
    T(app_load_dir("/nonexistent-dir", list, 8) == 0, "app: missing directory yields no entries");
    snprintf(cmd, sizeof cmd, "rm -rf %s", d); if (system(cmd)) exit(2);
}

static void test_keys(void)
{
    T(keycode_to_ascii(KEY_A, 0) == 'a' && keycode_to_ascii(KEY_A, 1) == 'A', "keys: letters and shift");
    T(keycode_to_ascii(KEY_1, 0) == '1' && keycode_to_ascii(KEY_1, 1) == '!', "keys: digits and shifted symbols");
    T(keycode_to_ascii(KEY_SPACE, 0) == ' ' && keycode_to_ascii(KEY_ENTER, 0) == '\n' && keycode_to_ascii(KEY_BACKSPACE, 0) == '\b', "keys: space, enter, backspace");
    T(keycode_to_ascii(KEY_F1, 0) == 0 && keycode_to_ascii(-1, 0) == 0 && keycode_to_ascii(100000, 0) == 0, "keys: unmapped/out-of-range codes give 0 (no table overrun)");
    int m = 0; T(keycode_is_modifier(KEY_LEFTSHIFT, &m) && m == KMOD_SHIFT && keycode_is_modifier(KEY_RIGHTALT, &m) && m == KMOD_ALT && !keycode_is_modifier(KEY_A, &m), "keys: modifier classification");
}

static void test_proto(void)
{
    int sv[2]; if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv)) exit(2);
    Msg m; memset(&m, 0, sizeof m); m.type = M_CREATE_WINDOW; m.v[0] = 640; m.v[1] = 480; snprintf(m.text, sizeof m.text, "hello");
    int mf = memfd_create("t", 0); if (mf < 0 || ftruncate(mf, 4096)) exit(2); (void)!write(mf, "Z", 1);
    T(proto_send(sv[0], &m, mf) == 0, "proto: send with an fd");
    Msg r; int fd = -1; T(proto_recv(sv[1], &r, &fd) == 1 && r.type == M_CREATE_WINDOW && r.v[0] == 640 && !strcmp(r.text, "hello") && fd >= 0, "proto: message and fd arrive");
    char c = 0; T(pread(fd, &c, 1, 0) == 1 && c == 'Z', "proto: received fd refers to the same memory object");
    close(fd);
    T(proto_send(sv[0], &m, -1) == 0 && proto_recv(sv[1], &r, &fd) == 1 && fd == -1, "proto: no fd when none sent");
    (void)!write(sv[0], "short", 5); T(proto_recv(sv[1], &r, &fd) == -1, "proto: short packet rejected");
    memset(&m, 'x', sizeof m); m.type = 7; proto_send(sv[0], &m, -1); proto_recv(sv[1], &r, &fd); T(r.text[sizeof r.text - 1] == 0, "proto: text field is always NUL-terminated");
    close(sv[0]); T(proto_recv(sv[1], &r, &fd) == 0, "proto: peer close reads as EOF"); close(sv[1]); close(mf);
}

/* ---- window manager ---- */
typedef struct { int n; Msg last[32]; int clients[32]; } Cap;
static void cap(void *ctx, int client, const Msg *m) { Cap *c = ctx; if (c->n < 32) { c->last[c->n] = *m; c->clients[c->n++] = client; } }
static int count(const Cap *c, uint32_t type, uint32_t win) { int k = 0; for (int i = 0; i < c->n; i++) if (c->last[i].type == type && (win == 0 || c->last[i].window == win)) k++; return k; }
static const Msg *find(const Cap *c, uint32_t type) { for (int i = c->n - 1; i >= 0; i--) if (c->last[i].type == type) return &c->last[i]; return NULL; }
static uint32_t *buf(int w, int h, uint32_t color) { uint32_t *p = malloc((size_t)w * (size_t)h * 4); for (int i = 0; i < w * h; i++) p[i] = color; return p; }

static void test_wm(void)
{
    Cap c; memset(&c, 0, sizeof c); static Wm w; wm_init(&w, 800, 600, cap, &c);
    uint32_t *b1 = buf(200, 100, 0xaa0000), *b2 = buf(100, 80, 0x00aa00), *bp = buf(800, PANEL_HEIGHT, 0x101010), *bpop = buf(50, 40, 0x0000aa);
    T(wm_add_window(&w, 0, WK_NORMAL, 0, 100, 0, 0, "x", b1, 1000) == 0, "wm: zero-size window rejected");
    T(wm_add_window(&w, 0, WK_NORMAL, 200, 100, 0, 0, "x", b1, 100) == 0, "wm: buffer smaller than w*h*4 rejected");
    T(wm_add_window(&w, 0, 9, 200, 100, 0, 0, "x", b1, 80000) == 0, "wm: unknown kind rejected");
    T(wm_add_window(&w, 0, WK_NORMAL, 99999, 100, 0, 0, "x", b1, (size_t)99999 * 400) == 0, "wm: absurd width rejected");
    uint32_t panel = wm_add_window(&w, 1, WK_PANEL, 800, PANEL_HEIGHT, 0, 0, "panel", bp, (size_t)800 * PANEL_HEIGHT * 4);
    uint32_t a = wm_add_window(&w, 0, WK_NORMAL, 200, 100, 0, 0, "App A", b1, 80000);
    uint32_t b = wm_add_window(&w, 2, WK_NORMAL, 100, 80, 0, 0, "App B", b2, 32000);
    T(panel && a && b && a != b, "wm: windows get distinct ids");
    T(count(&c, E_CONFIGURE, a) == 1, "wm: client is told its position (CONFIGURE)");
    Win *wa = wm_find(&w, a), *wb = wm_find(&w, b);
    T(wa->y >= PANEL_HEIGHT + WM_TITLE_H && wb->x != wa->x, "wm: new windows are placed below the panel and cascaded");
    T(w.focus == (int)b, "wm: newest window has focus");
    Surface s = mk(800, 600); wm_compose(&w, &s);
    T(at(&s, wb->x + 10, wb->y + 10) == 0x00aa00, "compose: top window's pixels visible");
    T(at(&s, wa->x + 150, wa->y + 90) == 0xaa0000, "compose: lower window visible where not covered");
    T(at(&s, wb->x + 10, wb->y - 8) == TH_TITLE_ACTIVE, "compose: active title bar drawn by the compositor");
    T(at(&s, wa->x + 150, wa->y - 8) == TH_TITLE_INACTIVE, "compose: inactive title bar drawn in the inactive colour");
    T(at(&s, 400, 10) == 0x101010, "compose: panel drawn");
    /* click a window: raise + focus + local coordinates */
    c.n = 0; wm_pointer_move(&w, wa->x + 20 - w.ptr_x, wa->y + 30 - w.ptr_y); wm_button(&w, 1);
    const Msg *pe = find(&c, E_POINTER); T(w.focus == (int)a && pe && pe->v[0] == 1 && pe->v[1] == 20 && pe->v[2] == 30, "wm: click focuses and sends window-local press coordinates");
    wm_button(&w, 0); T(count(&c, E_POINTER, a) >= 2 && find(&c, E_POINTER)->v[0] == 2, "wm: release delivered to the window that got the press");
    wm_compose(&w, &s); T(at(&s, wa->x + 10, wa->y + 10) == 0xaa0000, "compose: clicked window is raised above the other");
    /* drag by title bar */
    int ox = wa->x, oy = wa->y; wm_pointer_move(&w, wa->x + 50 - w.ptr_x, wa->y - 10 - w.ptr_y); wm_button(&w, 1); wm_pointer_move(&w, 40, 25); wm_button(&w, 0);
    T(wa->x == ox + 40 && wa->y == oy + 25, "wm: dragging the title bar moves the window");
    wm_pointer_move(&w, 0, -5000); wm_pointer_move(&w, -5000, 0); wm_button(&w, 1); wm_button(&w, 0); T(w.ptr_x == 0 && w.ptr_y == 0, "wm: pointer is clamped to the screen");
    wm_pointer_move(&w, 5000, 5000); T(w.ptr_x == 799 && w.ptr_y == 599, "wm: pointer clamped at the far corner");
    /* drag cannot hide the title bar under the panel */
    wm_pointer_move(&w, wa->x + 50 - w.ptr_x, wa->y - 10 - w.ptr_y); wm_button(&w, 1); wm_pointer_move(&w, 0, -2000); wm_button(&w, 0);
    T(wa->y >= PANEL_HEIGHT + WM_TITLE_H, "wm: a window cannot be dragged under the panel");
    /* close button */
    c.n = 0; wm_pointer_move(&w, wa->x + wa->w - 12 - w.ptr_x, wa->y - 11 - w.ptr_y); wm_button(&w, 1); wm_button(&w, 0);
    T(count(&c, E_CLOSE, a) == 1, "wm: close button sends E_CLOSE to the client");
    /* keyboard: goes to focus only; shortcuts */
    c.n = 0; wm_key(&w, KEY_A, 1); const Msg *ke = find(&c, E_KEY); T(ke && ke->window == a && ke->v[1] == 'a' && ke->v[2] == 1, "wm: key events go to the focused window with ASCII");
    c.n = 0; wm_key(&w, KEY_LEFTSHIFT, 1); wm_key(&w, KEY_A, 1); ke = find(&c, E_KEY); T(ke && ke->v[1] == 'A' && (ke->v[3] & KMOD_SHIFT), "wm: shift modifier tracked");
    wm_key(&w, KEY_LEFTSHIFT, 0);
    c.n = 0; wm_key(&w, KEY_LEFTALT, 1); wm_key(&w, KEY_F4, 1); T(count(&c, E_CLOSE, w.focus ? (uint32_t)w.focus : 0) == 1, "wm: Alt+F4 closes the focused window"); wm_key(&w, KEY_LEFTALT, 0);
    /* workspaces */
    c.n = 0; wm_switch_workspace(&w, 1);
    T(w.ws == 1 && count(&c, E_WORKSPACE, panel) == 1 && find(&c, E_WORKSPACE)->v[0] == 1, "wm: panel is told about workspace changes");
    wm_compose(&w, &s); T(at(&s, wa->x + 10, wa->y + 10) != 0xaa0000 && at(&s, wb->x + 10, wb->y + 10) != 0x00aa00, "workspace: windows of another workspace are hidden");
    T(at(&s, 400, 10) == 0x101010, "workspace: panel stays visible on every workspace");
    wm_key(&w, KEY_LEFTALT, 1); wm_key(&w, KEY_1, 1); wm_key(&w, KEY_LEFTALT, 0); T(w.ws == 0, "workspace: Alt+1 switches back");
    wm_switch_workspace(&w, 99); wm_switch_workspace(&w, -1); T(w.ws == 0, "workspace: invalid index ignored");
    c.n = 0; wm_switch_workspace(&w, 2); wm_key(&w, KEY_B, 1); T(count(&c, E_KEY, 0) == 0, "workspace: no key events when the workspace has no windows");
    wm_switch_workspace(&w, 0);
    /* popup dismissal and layering */
    uint32_t pop = wm_add_window(&w, 3, WK_POPUP, 50, 40, 300, 300, "menu", bpop, 8000);
    wm_compose(&w, &s); T(at(&s, 310, 310) == 0x0000aa, "layers: popup drawn above normal windows");
    c.n = 0; wm_pointer_move(&w, 700 - w.ptr_x, 500 - w.ptr_y); wm_button(&w, 1); wm_button(&w, 0); T(count(&c, E_CLOSE, pop) == 1, "wm: clicking outside a popup asks it to close");
    wm_remove_window(&w, pop); T(wm_find(&w, pop) == NULL, "wm: window removal");
    /* client death */
    int removed = wm_remove_client(&w, 2, NULL); T(removed == 1 && wm_find(&w, b) == NULL, "wm: all windows of a dead client are removed");
    T(w.focus != (int)b, "wm: focus never points at a removed window");
    wm_remove_window(&w, a); wm_remove_window(&w, panel); wm_remove_window(&w, 9999); T(1, "wm: removing unknown ids is harmless");
    for (int i = 0; i < WM_MAX_WINDOWS + 4; i++) wm_add_window(&w, 5, WK_NOTIFY, 10, 10, 0, 0, "n", b1, 400); int used = 0; for (int i = 0; i < WM_MAX_WINDOWS; i++) used += w.wins[i].used; T(used == WM_MAX_WINDOWS, "wm: window table is bounded");
    free(s.px); free(b1); free(b2); free(bp); free(bpop);
}


/* ===================================================== Aether Aurora additions ===================================================== */
static double rel_lum(uint32_t c)
{
    double ch[3] = { (double)((c >> 16) & 255) / 255.0, (double)((c >> 8) & 255) / 255.0, (double)(c & 255) / 255.0 };
    for (int i = 0; i < 3; i++) ch[i] = ch[i] <= 0.03928 ? ch[i] / 12.92 : pow((ch[i] + 0.055) / 1.055, 2.4);
    return 0.2126 * ch[0] + 0.7152 * ch[1] + 0.0722 * ch[2];
}
static double contrast(uint32_t a, uint32_t b) { double x = rel_lum(a), y = rel_lum(b); if (x < y) { double t = x; x = y; y = t; } return (x + 0.05) / (y + 0.05); }

static void test_theme(void)
{
    T(TH_VOID == 0x080D18u && TH_DESKTOP == 0x0B1220u && TH_PANEL == 0x101A2Cu && TH_WINDOW == 0x131E31u && TH_SURFACE2 == 0x1C2B43u, "theme: surface tokens match the Aurora specification exactly");
    T(TH_TEXT == 0xF2F6FFu && TH_TEXT2 == 0xA7B5CCu && TH_MUTED == 0x73839Du, "theme: text tokens match the specification exactly");
    T(TH_BLUE == 0x398BFFu && TH_BLUE_HOVER == 0x57A0FFu && TH_CYAN == 0x63E5F5u && TH_BORDER == 0x2A3B54u, "theme: accent and border tokens match the specification exactly");
    T(TH_OK == 0x55D6A0u && TH_WARN == 0xFFC66Du && TH_ERR == 0xFF7185u, "theme: status tokens match the specification exactly");
    T(contrast(TH_TEXT, TH_WINDOW) > 12.0 && contrast(TH_TEXT2, TH_WINDOW) > 7.0, "theme: primary and secondary text keep strong contrast on windows");
    T(contrast(TH_TEXT, TH_PANEL) > 12.0 && contrast(TH_TEXT2, TH_SURFACE2) > 6.0, "theme: text is readable on the panel and on secondary surfaces");
    T(contrast(TH_MUTED, TH_WINDOW) >= 4.0, "theme: muted text still reaches 4:1 on windows");
    T(contrast(TH_VOID, TH_BLUE) >= 4.5 && contrast(TH_VOID, TH_ERR) >= 4.5, "theme: dark text/glyphs on accent and error fills (primary button, active workspace, close hover) meet WCAG AA 4.5:1");
    T(PANEL_HEIGHT == 38 && TH_TITLE_H == 38 && TH_R_WIN == 10 && TH_R_MENU == 12 && TH_CTRL_H == 32 && TH_WIN_PAD == 20 && TH_PANEL_PAD == 12, "theme: geometry targets (panel/title 38, radii 10/12, control 32, padding 20/12)");
    T(TH_S1 == 4 && TH_S2 == 8 && TH_S3 == 12 && TH_S4 == 16 && TH_S5 == 20 && TH_S6 == 24 && TH_S8 == 32, "theme: 4 px spacing scale");
    T(TH_SETTINGS_W == 760 && TH_SETTINGS_H == 520 && TH_SIDEBAR_W == 190 && TH_LAUNCHER_W == 480 && TH_LAUNCHER_H == 560 && TH_DESK_ICON == 64, "theme: window and launcher size targets");
    T(TH_ANIM_MENU_MS >= 100 && TH_ANIM_MENU_MS <= 160 && TH_TITLE_BTN >= 30 && TH_ICONBTN <= 34, "theme: animation 100-160 ms, icon buttons 30-34 px");
}
static void test_gfx_aurora(void)
{
    T(gfx_mix(0x102030, 0xf0e0d0, 0) == 0x102030 && gfx_mix(0x102030, 0xf0e0d0, 256) == 0xf0e0d0, "gfx: mix endpoints are exact");
    uint32_t m = gfx_mix(0x000000, 0xfefefe, 128); T(((m >> 16) & 255) == 127, "gfx: mix midpoint");
    Surface s = mk(40, 40); gfx_fill(&s, rect_make(0, 0, 40, 40), 0x101010);
    gfx_blend_px(&s, 5, 5, 0xffffff, 0); gfx_blend_px(&s, 6, 5, 0xffffff, 255); gfx_blend_px(&s, -3, 99, 0xffffff, 255);
    T(at(&s, 5, 5) == 0x101010 && at(&s, 6, 5) == 0xffffff, "gfx: blend with alpha 0 is a no-op, 255 is exact; off-surface is safe");
    gfx_fill_alpha(&s, rect_make(10, 10, 4, 4), 0xffffff, 128); uint32_t hv = at(&s, 11, 11); T(hv > 0x101010 && hv < 0xffffff && ((hv >> 16) & 255) > 0x40 && ((hv >> 16) & 255) < 0xc0, "gfx: translucent fill mixes with the backdrop");
    gfx_fill(&s, rect_make(0, 0, 40, 40), 0x000000); gfx_rrect(&s, rect_make(4, 4, 32, 32), 10, 0xff0000);
    T(at(&s, 20, 20) == 0xff0000 && at(&s, 4, 20) == 0xff0000 && at(&s, 20, 4) == 0xff0000, "gfx: rounded rect is solid inside and along the straight edges");
    T(at(&s, 4, 4) == 0x000000, "gfx: rounded rect leaves the very corner pixel untouched");
    uint32_t edge = at(&s, 6, 7); T(((edge >> 16) & 255) > 0 && ((edge >> 16) & 255) < 255, "gfx: rounded rect corners are anti-aliased (partial coverage)");
    gfx_fill(&s, rect_make(0, 0, 40, 40), 0x000000); gfx_rrect_ex(&s, rect_make(4, 4, 32, 32), 10, 0, 0, 0, 0xff0000, 255);
    T(at(&s, 4, 4) == 0 && at(&s, 35, 4) == 0xff0000 && at(&s, 4, 35) == 0xff0000, "gfx: corner radii are independent");
    gfx_rrect(&s, rect_make(-20, -20, 200, 200), 12, 0x00ff00); gfx_rrect(&s, rect_make(10, 10, 0, 5), 4, 1); gfx_rrect(&s, rect_make(10, 10, 5, 5), 99, 1); T(1, "gfx: oversize/degenerate/oversized-radius rrects are clipped (sanitizer-checked)");
    Surface sh = mk(120, 100); gfx_fill(&sh, rect_make(0, 0, 120, 100), 0x808080); Rect body = rect_make(30, 30, 60, 40);
    gfx_shadow(&sh, body, 10, 12, 4, 150);
    T(at(&sh, 60, 50) == 0x808080 && at(&sh, 40, 40) == 0x808080, "gfx: shadow never touches the body interior");
    T(at(&sh, 60, 72) < 0x808080 && at(&sh, 60, 78) < 0x808080 && at(&sh, 60, 72) < at(&sh, 60, 78), "gfx: shadow is darkest at the edge and fades outward");
    T(at(&sh, 60, 90) == 0x808080 && at(&sh, 5, 5) == 0x808080, "gfx: shadow is confined to its blur radius");
    Surface src = mk(30, 30), dst = mk(40, 40); gfx_fill(&src, rect_make(0, 0, 30, 30), 0x0000ff); gfx_fill(&dst, rect_make(0, 0, 40, 40), 0xff0000);
    gfx_blit_rounded(&dst, 5, 5, &src, rect_make(0, 0, 30, 30), 8, 8, 8, 8, 255);
    T(at(&dst, 20, 20) == 0x0000ff && at(&dst, 5, 5) == 0xff0000 && at(&dst, 5, 20) == 0x0000ff, "gfx: rounded blit copies the interior exactly and clips the corners");
    gfx_fill(&dst, rect_make(0, 0, 40, 40), 0xff0000); gfx_blit_rounded(&dst, 5, 5, &src, rect_make(0, 0, 30, 30), 0, 0, 0, 0, 128);
    uint32_t mv = at(&dst, 20, 20); T(((mv >> 16) & 255) > 0x50 && ((mv >> 16) & 255) < 0xb0 && (mv & 255) > 0x50 && (mv & 255) < 0xb0, "gfx: constant-alpha blit mixes source and destination");
    gfx_fill(&dst, rect_make(0, 0, 40, 40), 0xff0000); gfx_blit_rounded(&dst, 5, 5, &src, rect_make(0, 0, 30, 30), 0, 0, 0, 0, 0);
    T(at(&dst, 20, 20) == 0xff0000, "gfx: alpha 0 blit draws nothing");
    gfx_blit_rounded(&dst, -50, -50, &src, rect_make(-9, -9, 500, 500), 9, 9, 9, 9, 255); gfx_blit_rounded(&dst, 30, 30, &src, rect_make(0, 0, 30, 30), 9, 9, 9, 9, 255); T(1, "gfx: rounded blit clips on both surfaces (sanitizer-checked)");
    Surface st = mk(40, 40); gfx_capsule(&st, 5.0f, 20.0f, 35.0f, 20.0f, 1.0f, 0xffffff, 255); gfx_disc(&st, 20.0f, 8.0f, 3.0f, 0xffffff, 255); gfx_ring(&st, 20.0f, 30.0f, 5.0f, 1.0f, 0xffffff, 255); gfx_quad(&st, 2.0f, 2.0f, 20.0f, 0.0f, 38.0f, 2.0f, 1.0f, 0xffffff, 255);
    T(at(&st, 20, 19) != 0 && at(&st, 20, 8) == 0xffffff && at(&st, 20, 25) != 0 && at(&st, 20, 30) == 0 && at(&st, 1, 39) == 0, "gfx: capsule, disc, ring and curve strokes land where expected");
    gfx_capsule(&st, -100.0f, -100.0f, 200.0f, 200.0f, 3.0f, 0xffffff, 255); gfx_disc(&st, 500.0f, 500.0f, 4.0f, 1, 255); T(1, "gfx: strokes far off-surface are safe (sanitizer-checked)");
    gfx_gradient_h(&st, rect_make(0, 0, 40, 4), 0x000000, 0xffffff); T(at(&st, 0, 1) == 0 && at(&st, 39, 1) == 0xffffff && at(&st, 20, 1) > 0x303030 && at(&st, 20, 1) < 0xd0d0d0, "gfx: horizontal gradient runs end to end");
    free(s.px); free(sh.px); free(src.px); free(dst.px); free(st.px);
}
static void test_font(void)
{
    const Face *r13 = font_face(FW_REGULAR, 13), *m13 = font_face(FW_MEDIUM, 13), *r11 = font_face(FW_REGULAR, 11), *h1 = font_face(FW_MEDIUM, 20);
    T(r13 && m13 && r11 && h1 && r13->px == 13 && h1->px == 20 && m13->weight == FW_MEDIUM, "font: all UI sizes and both weights are available");
    T(font_face(FW_REGULAR, 40)->px >= 13 && font_face(FW_MEDIUM, 1) != NULL, "font: unknown sizes fall back to the nearest face");
    T(font_width(r13, "") == 0 && font_width(r13, "Settings") > font_width(r13, "Set") && font_width(r13, "a") > 0, "font: width grows with the text");
    T(font_width(h1, "Settings") > font_width(r13, "Settings") && font_width(r13, "Settings") > font_width(r11, "Settings"), "font: larger sizes are wider");
    T(font_width(m13, "Packages") >= font_width(r13, "Packages"), "font: medium is at least as wide as regular");
    T(font_width(r13, "iiiiiiii") < font_width(r13, "WWWWWWWW"), "font: faces are proportional (narrow vs wide glyphs)");
    T(font_width(r13, "caf\xc3\xa9 \x01") == font_width(r13, "caf?? ?"), "font: unsupported bytes render as '?' without crashing");
    Surface s = mk(120, 30); gfx_fill(&s, rect_make(0, 0, 120, 30), 0x000000);
    int adv = font_draw(&s, rect_make(0, 0, 120, 30), 4, 20, "Hello", r13, 0xffffff); int lit = 0, partial = 0;
    for (int y = 0; y < 30; y++) for (int x = 0; x < 120; x++) { uint32_t p = at(&s, x, y) & 255; if (p) lit++; if (p > 0 && p < 255) partial++; }
    T(adv == font_width(r13, "Hello") && lit > 60 && partial > 20, "font: text is drawn anti-aliased (partial-coverage pixels) and reports its advance");
    Surface c = mk(40, 30); font_draw(&c, rect_make(10, 5, 10, 10), -30, 15, "Clipping test", r13, 0xffffff);
    int outside = 0; for (int y = 0; y < 30; y++) for (int x = 0; x < 40; x++) if (at(&c, x, y) && !(x >= 10 && x < 20 && y >= 5 && y < 15)) outside++;
    T(outside == 0, "font: drawing respects the clip rectangle");
    font_draw(&c, rect_make(0, 0, 40, 30), 500, 500, "off", r13, 1); font_draw(&c, rect_make(0, 0, 40, 30), -500, -500, "off", r13, 1); T(1, "font: far off-surface text is safe (sanitizer-checked)");
    char buf[128]; font_ellipsize(buf, sizeof buf, "Short", r13, 200); T(!strcmp(buf, "Short"), "font: text that fits is unchanged");
    font_ellipsize(buf, sizeof buf, "A rather long application title that cannot fit", r13, 120);
    T(font_width(r13, buf) <= 120 && strlen(buf) > 3 && !strcmp(buf + strlen(buf) - 3, "..."), "font: long text is cut to the width and ends with an ellipsis");
    char tiny[6]; font_ellipsize(tiny, sizeof tiny, "Overflowing destination buffer", r13, 4000); T(strlen(tiny) < sizeof tiny, "font: ellipsize never overruns a small buffer");
    char lines[4][96]; int n = font_wrap("The quick brown fox jumps over the lazy dog and keeps running far away", r13, 150, lines, 4); int ok = n >= 2 && n <= 4;
    for (int i = 0; i < n; i++) if (font_width(r13, lines[i]) > 150) ok = 0;
    T(ok, "font: word wrap respects the width and the line limit");
    n = font_wrap("one two three four five six seven eight nine ten eleven twelve thirteen", r13, 100, lines, 2); T(n == 2 && font_width(r13, lines[1]) <= 100 && strstr(lines[1], "..."), "font: wrap ellipsizes the last allowed line");
    n = font_wrap("Supercalifragilisticexpialidocious_unbreakable_word_that_is_too_wide", r13, 80, lines, 3); ok = n >= 1; for (int i = 0; i < n; i++) if (font_width(r13, lines[i]) > 80) ok = 0; T(ok, "font: an unbreakable word is clipped to the width");
    T(font_wrap("", r13, 100, lines, 3) == 0 && font_wrap("   ", r13, 100, lines, 3) == 0, "font: empty text wraps to zero lines");
    Rect box = rect_make(0, 0, 100, 30); Surface b = mk(100, 30); font_draw_box(&b, box, "Mx", r13, 0xffffff, FA_CENTER);
    int minx = 100, maxx = -1, miny = 30, maxy = -1; for (int y = 0; y < 30; y++) for (int x = 0; x < 100; x++) if (at(&b, x, y)) { if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y; }
    T(maxx >= 0 && abs((minx + maxx) / 2 - 50) <= 2 && abs((miny + maxy) / 2 - 15) <= 2, "font: boxed text is centred horizontally and on the cap height");
    free(s.px); free(c.px); free(b.px);
}
static void test_icons(void)
{
    T(icon_by_name("settings") == ICON_SETTINGS && icon_by_name("aether") == ICON_AETHER && icon_by_name("nope") == ICON_NONE && icon_by_name("") == ICON_NONE && icon_by_name(NULL) == ICON_NONE, "icons: lookup by name");
    int allok = 1, allbounded = 1;
    for (int id = ICON_AETHER; id < ICON_COUNT; id++) for (int size = 16; size <= 32; size += 8) {
        int margin = 6, dim = size + 2 * margin; Surface s = mk(dim, dim); icon_draw(&s, (IconId)id, margin, margin, size, IS_DEFAULT);
        int lit = 0, stray = 0; for (int y = 0; y < dim; y++) for (int x = 0; x < dim; x++) if (at(&s, x, y)) { lit++; if (x < margin - 2 || y < margin - 2 || x >= margin + size + 2 || y >= margin + size + 2) stray++; }
        if (lit < size) allok = 0; if (stray) allbounded = 0; free(s.px);
    }
    T(allok, "icons: every glyph draws visible pixels at 16, 24 and 32 px"); T(allbounded, "icons: every glyph stays inside its design box");
    uint32_t cols[5]; for (int i = 0; i < 5; i++) cols[i] = icon_main_color((IconState)i);
    int distinct = 1; for (int i = 0; i < 5; i++) for (int j = i + 1; j < 5; j++) if (cols[i] == cols[j]) distinct = 0;
    T(distinct, "icons: default, muted, hover, active and disabled states have distinct colours");
    T(icon_main_color(IS_HOVER) == TH_TEXT && icon_main_color(IS_DEFAULT) == TH_TEXT2 && icon_accent_color(IS_ACTIVE) == TH_CYAN, "icons: state colours come from theme tokens");
    Surface a = mk(24, 24), b = mk(24, 24); icon_draw_ex(&a, ICON_BATTERY, 0, 0, 24, TH_TEXT2, TH_TEXT2, 10); icon_draw_ex(&b, ICON_BATTERY, 0, 0, 24, TH_TEXT2, TH_TEXT2, 90);
    int diff = 0; for (int i = 0; i < 24 * 24; i++) if (a.px[i] != b.px[i]) diff++; T(diff > 10, "icons: battery glyph reflects the level it is given");
    Surface n1 = mk(24, 24), n2 = mk(24, 24); icon_draw_ex(&n1, ICON_NETWORK, 0, 0, 24, TH_TEXT2, TH_BLUE, -1); icon_draw_ex(&n2, ICON_NETWORK, 0, 0, 24, TH_TEXT2, TH_BLUE, 0);
    diff = 0; for (int i = 0; i < 24 * 24; i++) if (n1.px[i] != n2.px[i]) diff++; T(diff > 10, "icons: network glyph shows a distinct link-down state");
    Surface tile = mk(64, 64); icon_app_tile(&tile, rect_make(0, 0, 64, 64), ICON_NONE, "terminal", 0, 0); int cyan = 0; for (int i = 0; i < 64 * 64; i++) if (tile.px[i] == TH_CYAN) cyan++;
    T(at(&tile, 0, 0) == 0 && at(&tile, 32, 32) != 0 && cyan > 5, "icons: app tile has rounded corners and falls back to the app's initial");
    free(a.px); free(b.px); free(n1.px); free(n2.px); free(tile.px);
}
static unsigned long hash_surface(const Surface *s) { unsigned long h = 1469598103934665603ul; for (int i = 0; i < s->w * s->h; i++) { h ^= s->px[i]; h *= 1099511628211ul; } return h; }
static void test_wallpaper(void)
{
    static const int sizes[][2] = { { 1, 1 }, { 2, 3 }, { 17, 9 }, { 320, 200 }, { 800, 562 }, { 1280, 762 } }; int ok = 1;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) { Surface s = mk(sizes[i][0], sizes[i][1]); wallpaper_render(&s); if (!s.px) ok = 0; free(s.px); }
    wallpaper_render(NULL); Surface bad = { NULL, 10, 10, 10 }; wallpaper_render(&bad); Surface z = mk(10, 10); z.w = 0; wallpaper_render(&z);
    T(ok, "wallpaper: renders at any size from 1x1 to 1280x762 without overflow (sanitizer-checked)");
    Surface a = mk(640, 400), b = mk(640, 400); wallpaper_render(&a); wallpaper_render(&b); T(hash_surface(&a) == hash_surface(&b), "wallpaper: rendering is deterministic");
    T(at(&a, 560, 380) % 256 > at(&a, 60, 80) % 256 + 15, "wallpaper: blue emerges from the lower-right quadrant");
    int bright = 0; uint32_t mx = 0; for (int i = 0; i < 640 * 400; i++) { uint32_t p = a.px[i]; if ((p >> 24) != 0) ok = 0; if ((p & 255) > 140 && ((p >> 8) & 255) > 100 && ((p >> 16) & 255) < 110) bright++; if ((p & 255) > mx) mx = p & 255; }
    T(bright > 300 && ok, "wallpaper: has blue/cyan aurora ribbons and keeps the unused alpha byte clear");
    T(mx < 0xd0, "wallpaper: restrained brightness (no blown-out neon)");
    int steps = 0; uint32_t prev = at(&a, 5, 399); for (int x = 6; x < 400; x++) { uint32_t p = at(&a, x, 399); if (p != prev) steps++; prev = p; }
    T(steps > 40, "wallpaper: dithered gradients (no large flat bands)");
    free(a.px); free(b.px); free(z.px);
}
static void test_appmeta_aurora(void)
{
    char dir[] = "/tmp/aether-gui-appsXXXXXX"; if (!mkdtemp(dir)) { T(0, "appmeta: temp dir"); return; }
    char path[300]; snprintf(path, sizeof path, "%s/a.app", dir); FILE *f = fopen(path, "w"); fputs("[Aether App]\nname=Alpha\ncomment=First\nexec=/bin/sh\nicon=settings\n", f); fclose(f);
    snprintf(path, sizeof path, "%s/b.app", dir); f = fopen(path, "w"); fputs("[Aether App]\nname=Beta\nexec=/nonexistent/beta\n", f); fclose(f);
    snprintf(path, sizeof path, "%s/c.app", dir); f = fopen(path, "w"); fputs("[Aether App]\nname=Gamma\nexec=relative/path\n", f); fclose(f);
    snprintf(path, sizeof path, "%s/d.app", dir); f = fopen(path, "w"); fputs("[Aether App]\nname=Delta\nexec=/etc/hostname\n", f); fclose(f);      /* exists but not executable */
    AppEntry e[8]; size_t all = app_load_dir(dir, e, 8); size_t run = app_load_runnable(dir, e, 8);
    T(all == 3, "appmeta: valid entries load; a relative exec path is rejected at load time (unchanged behaviour)");
    T(run == 1 && !strcmp(e[0].name, "Alpha") && !strcmp(e[0].icon, "settings"), "appmeta: only entries whose executable exists and is executable are runnable; icon key parsed");
    app_load_dir(dir, e, 8); AppEntry rel; memset(&rel, 0, sizeof rel); snprintf(rel.exec, sizeof rel.exec, "relative/path");
    T(!e[1].icon[0] && app_runnable(&e[0]) && !app_runnable(&e[1]) && !app_runnable(&e[2]) && !app_runnable(&rel) && !app_runnable(NULL), "appmeta: missing icon key is empty; runnable check rejects missing, non-executable and relative paths");
    for (char c = 'a'; c <= 'd'; c++) { snprintf(path, sizeof path, "%s/%c.app", dir, c); unlink(path); } rmdir(dir);
}

/* ---- window-manager behaviour of the Aurora desktop ---- */
static void ptr_to(Wm *w, int x, int y) { wm_pointer_move(w, x - w->ptr_x, y - w->ptr_y); }
static void click_at(Wm *w, int x, int y) { ptr_to(w, x, y); wm_button(w, 1); wm_button(w, 0); }
static void btn_at(const Win *x, int slot, int *cx, int *cy) { *cx = x->x + x->w - TH_S2 - TH_TITLE_BTN / 2 - slot * (TH_TITLE_BTN + TH_S1); *cy = x->y - WM_TITLE_H / 2; }
static uint32_t *buf_of(int w, int h, uint32_t color) { uint32_t *b = malloc((size_t)w * (size_t)h * 4); for (int i = 0; i < w * h; i++) b[i] = color; return b; }

static void test_wm_aurora(void)
{
    Cap c; memset(&c, 0, sizeof c); Wm *w = calloc(1, sizeof *w); wm_init(w, 800, 600, cap, &c); Surface s = mk(800, 600);
    uint32_t *bp = buf_of(800, PANEL_HEIGHT, 0x101010), *bd = buf_of(800, 600 - PANEL_HEIGHT, 0x303030), *ba = buf_of(300, 200, 0xaa0000), *bb = buf_of(200, 150, 0x00aa00);
    uint32_t panel = wm_add_window(w, 1, WK_PANEL, 800, PANEL_HEIGHT, 0, 0, "panel", bp, 800 * PANEL_HEIGHT * 4);
    uint32_t desk = wm_add_window(w, 2, WK_DESKTOP, 800, 600 - PANEL_HEIGHT, 5, 5, "desktop", bd, (size_t)800 * (600 - PANEL_HEIGHT) * 4);
    Win *dw = wm_find(w, desk); T(dw && dw->x == 0 && dw->y == PANEL_HEIGHT && dw->alpha == 255, "desktop: surface is pinned under the panel at the left edge");
    wm_compose(w, &s); T(at(&s, 300, 300) == 0x303030 && at(&s, 400, 10) == 0x101010, "desktop: surface is painted below the panel; panel on top");
    T(w->focus == 0, "desktop: never takes focus");
    uint32_t a = wm_add_window(w, 3, WK_NORMAL, 300, 200, 0, 0, "A", ba, 300 * 200 * 4); wm_set_flags(w, a, WF_RESIZABLE); Win *wa = wm_find(w, a);
    wm_compose(w, &s); T(at(&s, wa->x + 100, wa->y + 100) == 0xaa0000, "layers: normal windows are drawn above the desktop surface");
    c.n = 0; ptr_to(w, wa->x + 50, wa->y + 50); wm_button(w, 1); wm_button(w, 0); T(count(&c, E_POINTER, a) >= 2 && count(&c, E_POINTER, desk) == 0, "input: a window above the desktop receives the click, not the desktop");
    c.n = 0; ptr_to(w, 700, 500); wm_button(w, 1); wm_button(w, 0); T(count(&c, E_POINTER, desk) >= 3 && w->focus == (int)a, "input: empty desktop area receives pointer events and does not steal focus");
    /* title-bar buttons act on release, only when still over the same button */
    int cx, cy; btn_at(wa, 0, &cx, &cy); c.n = 0; ptr_to(w, cx, cy); wm_button(w, 1); T(count(&c, E_CLOSE, a) == 0, "wm: pressing the close button does nothing yet");
    ptr_to(w, cx, cy + 100); wm_button(w, 0); T(count(&c, E_CLOSE, a) == 0 && w->press_btn == 0, "wm: releasing away from the button cancels it");
    c.n = 0; ptr_to(w, cx, cy); wm_button(w, 1); wm_button(w, 0); T(count(&c, E_CLOSE, a) == 1, "wm: press and release on the close button closes");
    /* hover state is tracked and dirties the scene only when it changes */
    ptr_to(w, 650, 450); wm_compose(w, &s); ptr_to(w, 700, 500); T(!w->dirty && w->cursor_dirty, "wm: moving the pointer over empty desktop only dirties the cursor layer");
    w->cursor_dirty = 0; ptr_to(w, cx, cy); T(w->dirty && w->hot_win == (int)a && w->hot_btn != 0, "wm: hovering a title-bar button marks it hot and dirties the scene");
    wm_compose(w, &s); ptr_to(w, cx - 2, cy); T(!w->dirty, "wm: moving within the same button does not repaint the scene");
    wm_compose_scene(w, &s); T(at(&s, 700, 500) == 0x303030 && at(&s, 700, 500) != TH_VOID, "wm: scene composition excludes the pointer sprite"); wm_compose(w, &s); T(w->cursor_dirty == 0, "wm: full compose clears the cursor flag");
    /* minimise -> task chip -> restore */
    uint32_t b = wm_add_window(w, 4, WK_NORMAL, 200, 150, 0, 0, "B", bb, 200 * 150 * 4); Win *wb = wm_find(w, b); T(w->focus == (int)b, "wm: new window is focused");
    c.n = 0; int mx, my; btn_at(wb, 1, &mx, &my);               /* non-resizable: slot 1 is the minimise button */
    click_at(w, mx, my); T(wb->minimized == 1 && w->focus == (int)a, "wm: minimise hides the window and moves focus to the next one");
    wm_compose(w, &s); T(at(&s, wb->x + 50, wb->y + 50) != 0x00aa00, "wm: a minimised window is not drawn");
    int items = 0, min_flag = 0, phases = 0, last_phase = -1; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_TASK && c.clients[i] == 1) { if (c.last[i].v[0] == 1) { items++; if (c.last[i].window == b && (c.last[i].v[1] & 2)) min_flag = 1; } else { phases++; last_phase = c.last[i].v[0]; } }
    T(items >= 2 && min_flag && phases >= 2 && last_phase == 2, "tasks: the panel gets a begin / item... / end burst with minimised and focused flags");
    int titled = 0; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_TASK && c.last[i].v[0] == 1 && (!strcmp(c.last[i].text, "A") || !strcmp(c.last[i].text, "B"))) titled++;
    T(titled >= 2, "tasks: item events carry the window title");
    c.n = 0; wm_task_activate(w, b); T(wb->minimized == 0 && w->focus == (int)b, "tasks: activating a minimised window restores, raises and focuses it");
    wm_task_activate(w, b); T(wb->minimized == 1, "tasks: activating the focused window minimises it");
    wm_task_activate(w, b); wm_task_activate(w, a); T(w->focus == (int)a && !wa->minimized, "tasks: activating another window focuses it");
    wm_task_activate(w, 9999); wm_task_activate(w, desk); T(1, "tasks: unknown or non-window targets are ignored");
    /* maximise / restore needs a real buffer swap */
    Rect old = rect_make(wa->x, wa->y, wa->w, wa->h); btn_at(wa, 1, &mx, &my); c.n = 0; click_at(w, mx, my);
    const Msg *rq = NULL; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_CONFIGURE && c.last[i].window == a && c.last[i].v[4] == 1) rq = &c.last[i];
    T(rq && rq->v[2] == 800 && rq->v[3] == 600 - PANEL_HEIGHT - WM_TITLE_H && rq->v[1] == PANEL_HEIGHT + WM_TITLE_H && !wa->maximized, "wm: maximise sends a resize request for the work area and waits for the client");
    uint32_t *big = buf_of(800, 600 - PANEL_HEIGHT - WM_TITLE_H, 0x0000cc); T(wm_replace_buffer(w, a, 800, 600 - PANEL_HEIGHT - WM_TITLE_H, big, (size_t)800 * (size_t)(600 - PANEL_HEIGHT - WM_TITLE_H) * 4, NULL) == 0, "wm: replacing the buffer is accepted");
    T(wa->maximized && wa->x == 0 && wa->y == PANEL_HEIGHT + WM_TITLE_H && wa->w == 800, "wm: the window is maximised once the new buffer arrives");
    wm_compose(w, &s); T(at(&s, 0, 599) == 0x0000cc && at(&s, 799, 599) == 0x0000cc, "wm: maximised window has square corners flush with the screen");
    ptr_to(w, 400, wa->y - 10); wm_button(w, 1); ptr_to(w, 300, wa->y + 100); wm_button(w, 0); T(wa->x == 0 && wa->y == PANEL_HEIGHT + WM_TITLE_H, "wm: a maximised window cannot be dragged");
    btn_at(wa, 1, &mx, &my); c.n = 0; click_at(w, mx, my); rq = NULL; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_CONFIGURE && c.last[i].v[4] == 1) rq = &c.last[i];
    T(rq && rq->v[0] == old.x && rq->v[1] == old.y && rq->v[2] == old.w && rq->v[3] == old.h, "wm: restore asks for the previous size and position");
    uint32_t *small = buf_of(old.w, old.h, 0xaa0000); wm_replace_buffer(w, a, old.w, old.h, small, (size_t)old.w * (size_t)old.h * 4, NULL);
    T(!wa->maximized && wa->x == old.x && wa->y == old.y && wa->w == old.w, "wm: un-maximised window returns to where it was");
    T(wm_replace_buffer(w, a, 0, 5, small, 100, NULL) != 0 && wm_replace_buffer(w, a, 100, 100, small, 10, NULL) != 0 && wm_replace_buffer(w, 9999, 10, 10, small, 400, NULL) != 0, "wm: invalid buffer replacements are rejected");
    uint32_t nr = wm_add_window(w, 5, WK_NORMAL, 100, 100, 0, 0, "fixed", ba, 300 * 200 * 4); Win *wn = wm_find(w, nr); btn_at(wn, 1, &mx, &my); c.n = 0; click_at(w, mx, my); T(wn->minimized == 1 && count(&c, E_CONFIGURE, nr) == 0, "wm: windows without WF_RESIZABLE have no maximise control");
    wm_remove_window(w, nr);
    /* opacity */
    uint32_t pop = wm_add_window(w, 6, WK_POPUP, 100, 80, 400, 300, "menu", bb, 200 * 150 * 4);
    wm_compose(w, &s); T(at(&s, 420, 320) == 0x00aa00, "popup: fully opaque by default");
    wm_set_opacity(w, pop, 100); wm_compose(w, &s); uint32_t half = at(&s, 450, 340); T(half != 0x00aa00 && half != at(&s, 700, 450) && w->wins[0].used, "popup: reduced opacity blends with what is behind it");
    wm_set_opacity(w, pop, 0); wm_compose(w, &s); T(at(&s, 450, 340) != 0x00aa00 && at(&s, 450, 340) != half, "popup: opacity 0 is invisible");
    wm_set_opacity(w, pop, 999); T(wm_find(w, pop)->alpha == 255, "popup: opacity is clamped to 0..255");
    wm_remove_window(w, pop); uint32_t tr = wm_add_window(w, 6, WK_POPUP, 100, 80, 400, 300, "menu", bb, 200 * 150 * 4); wm_set_flags(w, tr, WF_TRANSPARENT); T(wm_find(w, tr)->alpha == 0, "popup: WF_TRANSPARENT starts fully transparent for a fade-in");
    wm_set_opacity(w, tr, 255);
    /* keyboard: an open popup owns it; Super taps toggle the launcher */
    c.n = 0; wm_key(w, KEY_B, 1); T(count(&c, E_KEY, tr) == 1 && count(&c, E_KEY, a) == 0, "keys: an open popup receives the keyboard instead of the focused window");
    wm_remove_window(w, tr); c.n = 0; wm_key(w, KEY_B, 1); T(count(&c, E_KEY, a) == 1, "keys: with no popup the focused window gets keys again");
    c.n = 0; wm_key(w, KEY_LEFTMETA, 1); wm_key(w, KEY_LEFTMETA, 0); T(count(&c, E_HOTKEY, panel) == 1 && find(&c, E_HOTKEY)->v[0] == 1 && count(&c, E_KEY, 0) == 0, "keys: tapping Super sends the launcher hotkey to the panel only");
    c.n = 0; wm_key(w, KEY_LEFTMETA, 1); wm_key(w, KEY_B, 1); wm_key(w, KEY_B, 0); wm_key(w, KEY_LEFTMETA, 0); T(count(&c, E_HOTKEY, 0) == 0, "keys: Super used as a modifier does not fire the hotkey");
    /* screen resize */
    uint32_t n1 = wm_add_window(w, 7, WK_NOTIFY, 200, 50, 0, 0, "n1", bb, 200 * 150 * 4), n2 = wm_add_window(w, 7, WK_NOTIFY, 200, 90, 0, 0, "n2", bb, 200 * 150 * 4), n3 = wm_add_window(w, 7, WK_NOTIFY, 200, 30, 0, 0, "n3", bb, 200 * 150 * 4);
    Win *w1 = wm_find(w, n1), *w2 = wm_find(w, n2), *w3 = wm_find(w, n3);
    T(w1->y == PANEL_HEIGHT + TH_S3 && w2->y == w1->y + 50 + TH_S2 && w3->y == w2->y + 90 + TH_S2, "notify: notifications stack by their real heights");
    wm_remove_window(w, n2); T(w3->y == w1->y + 50 + TH_S2, "notify: closing one reflows the ones below");
    c.n = 0; wm_resize_screen(w, 640, 480); int screens = count(&c, E_SCREEN, 0), s0 = 0; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_SCREEN && c.last[i].v[0] == 640 && c.last[i].v[1] == 480) s0++;
    T(w->sw == 640 && w->sh == 480 && s0 == screens && screens >= 5 && w1->x == 640 - 200 - TH_S4, "wm: resizing the screen tells every client once and re-anchors notifications");
    T(wa->x + wa->w <= 640 && wa->y + wa->h <= 480 && w->ptr_x < 640 && w->ptr_y < 480, "wm: resizing keeps windows and the pointer on screen");
    wm_resize_screen(w, 0, 0); wm_resize_screen(w, -5, 100); T(w->sw == 640, "wm: nonsense screen sizes are ignored");
    Rect cr = wm_cursor_rect(w); T(cr.w > 0 && cr.h > 0 && cr.x == w->ptr_x, "wm: cursor rectangle follows the pointer");
    free(s.px); free(w); free(bp); free(bd); free(ba); free(bb); free(big); free(small);
}

/* ---- input translation (evdev frames -> InEvent), pointer scaling ---- */
static struct input_event ie(int type, int code, int value) { struct input_event e; memset(&e, 0, sizeof e); e.type = (unsigned short)type; e.code = (unsigned short)code; e.value = value; return e; }
static int feed(InDev *d, InEvent *out, const struct input_event *evs, int n) { int total_out = 0; for (int i = 0; i < n; i++) total_out += input_feed(d, &evs[i], out + total_out, 32 - total_out); return total_out; }
static void test_input_new(void)
{
    T(input_abs_scale(0, 0, 32767, 1280) == 0 && input_abs_scale(32767, 0, 32767, 1280) == 1279, "input: abs scale maps min->0 and max->size-1 (corners reachable)");
    T(input_abs_scale(16384, 0, 32767, 1280) == 640 && input_abs_scale(16383, 0, 32767, 1280) == 639, "input: abs scale rounds to nearest at mid-range");
    int mono = 1, prev = -1; for (int v = 0; v <= 32767; v += 7) { int p = input_abs_scale(v, 0, 32767, 800); if (p < prev || p < 0 || p > 799) mono = 0; prev = p; }
    T(mono, "input: abs scale is monotonic and always inside [0,size-1]");
    T(input_abs_scale(-50, 0, 32767, 800) == 0 && input_abs_scale(99999, 0, 32767, 800) == 799, "input: out-of-range raw values are clamped");
    T(input_abs_scale(5, 5, 5, 800) == 0 && input_abs_scale(3, 0, 100, 1) == 0, "input: degenerate range/size does not divide by zero");
    T(input_abs_scale(100, 100, 4095, 1024) == 0 && input_abs_scale(4095, 100, 4095, 1024) == 1023, "input: non-zero minimum of the range handled");

    InDev d; memset(&d, 0, sizeof d); d.caps = DEV_REL; InEvent o[32];
    struct input_event f1[] = { ie(EV_REL, REL_X, 5), ie(EV_REL, REL_Y, -3), ie(EV_SYN, SYN_REPORT, 0) };
    int n = feed(&d, o, f1, 3); T(n == 1 && o[0].kind == IN_MOTION && o[0].dx == 5 && o[0].dy == -3, "input: relative deltas are accumulated per frame into one motion");
    struct input_event f2[] = { ie(EV_REL, REL_X, 2), ie(EV_REL, REL_X, 3), ie(EV_SYN, SYN_REPORT, 0) };
    n = feed(&d, o, f2, 3); T(n == 1 && o[0].dx == 5 && o[0].dy == 0, "input: two REL_X in one frame add up");
    struct input_event f3[] = { ie(EV_KEY, BTN_LEFT, 1), ie(EV_REL, REL_X, 4), ie(EV_SYN, SYN_REPORT, 0) };
    n = feed(&d, o, f3, 3); T(n == 2 && o[0].kind == IN_MOTION && o[1].kind == IN_BUTTON && o[1].code == BTN_LEFT && o[1].value == 1, "input: motion is applied before a button in the same frame");
    struct input_event f4[] = { ie(EV_KEY, BTN_LEFT, 0), ie(EV_SYN, SYN_REPORT, 0) };
    n = feed(&d, o, f4, 2); T(n == 1 && o[0].kind == IN_BUTTON && o[0].value == 0, "input: left release reported");
    struct input_event f5[] = { ie(EV_KEY, BTN_RIGHT, 1), ie(EV_KEY, BTN_MIDDLE, 1), ie(EV_REL, REL_WHEEL, 1), ie(EV_SYN, SYN_REPORT, 0) };
    n = feed(&d, o, f5, 4); T(n == 3 && o[0].code == BTN_RIGHT && o[1].code == BTN_MIDDLE && o[2].kind == IN_WHEEL && o[2].dy == 1, "input: right/middle buttons then wheel, in order");
    struct input_event f6[] = { ie(EV_REL, REL_X, 9) };
    n = feed(&d, o, f6, 1); T(n == 0, "input: nothing is emitted before SYN_REPORT");
    struct input_event f7[] = { ie(EV_SYN, SYN_REPORT, 0) }; n = feed(&d, o, f7, 1); T(n == 1 && o[0].dx == 9, "input: the pending frame is flushed by SYN_REPORT");
    struct input_event f8[] = { ie(EV_KEY, BTN_LEFT, 1), ie(EV_SYN, SYN_REPORT, 0) }; feed(&d, o, f8, 2);
    struct input_event f9[] = { ie(EV_SYN, SYN_DROPPED, 0), ie(EV_SYN, SYN_REPORT, 0) };
    n = feed(&d, o, f9, 2); int rel = 0; for (int i = 0; i < n; i++) if (o[i].kind == IN_BUTTON && o[i].value == 0) rel = 1; T(rel, "input: SYN_DROPPED releases a button that might be stuck down");

    InDev a; memset(&a, 0, sizeof a); a.caps = DEV_ABS; input_dev_init_abs(&a, 0, 32767, 0, 32767);
    struct input_event g1[] = { ie(EV_ABS, ABS_X, 32767), ie(EV_ABS, ABS_Y, 0), ie(EV_SYN, SYN_REPORT, 0) };
    n = feed(&a, o, g1, 3); T(n == 1 && o[0].kind == IN_ABS && o[0].dx == 32767 && o[0].dy == 0 && o[0].x1 == 32767 && o[0].y1 == 32767, "input: absolute device yields IN_ABS with raw coordinates and range (not relative motion)");
    struct input_event g2[] = { ie(EV_ABS, ABS_X, 100), ie(EV_SYN, SYN_REPORT, 0) };
    n = feed(&a, o, g2, 2); T(n == 1 && o[0].kind == IN_ABS && o[0].dx == 100 && o[0].dy == 0, "input: a frame with only X keeps the last Y");
    a.btn_left_native = 0;                 /* a pure touch device: no BTN_LEFT of its own */
    struct input_event g3[] = { ie(EV_KEY, BTN_TOUCH, 1), ie(EV_SYN, SYN_REPORT, 0) };
    n = feed(&a, o, g3, 2); T(n == 1 && o[0].kind == IN_BUTTON && o[0].code == BTN_LEFT && o[0].value == 1, "input: BTN_TOUCH on a touch device stands in for the left button");
    struct input_event g4[] = { ie(EV_ABS, ABS_X, 500), ie(EV_ABS, ABS_Y, 600), ie(EV_KEY, BTN_LEFT, 1), ie(EV_SYN, SYN_REPORT, 0) };
    memset(&a, 0, sizeof a); a.caps = DEV_ABS; input_dev_init_abs(&a, 0, 32767, 0, 32767);
    n = feed(&a, o, g4, 4); T(n == 2 && o[0].kind == IN_ABS && o[1].kind == IN_BUTTON, "input: tablet click is placed at the new position first, then pressed");
    InDev k; memset(&k, 0, sizeof k); k.caps = DEV_KEYBOARD; struct input_event h[] = { ie(EV_KEY, KEY_A, 1), ie(EV_SYN, SYN_REPORT, 0), ie(EV_KEY, KEY_A, 2), ie(EV_SYN, SYN_REPORT, 0), ie(EV_KEY, KEY_A, 0), ie(EV_SYN, SYN_REPORT, 0) };
    n = feed(&k, o, h, 6); T(n == 3 && o[0].kind == IN_KEY && o[0].value == 1 && o[1].value == 2 && o[2].value == 0, "input: key press, repeat and release are reported");
}

/* ---- window manager: pointer, clicks, focus, workspaces, shortcuts ---- */
static void test_wm_input(void)
{
    Cap c; memset(&c, 0, sizeof c); static Wm w; wm_init(&w, 800, 600, cap, &c);
    uint32_t *bp = buf(800, PANEL_HEIGHT, 0x101010), *b1 = buf(200, 100, 0xaa0000), *b2 = buf(200, 100, 0x00aa00);
    uint32_t panel = wm_add_window(&w, 1, WK_PANEL, 800, PANEL_HEIGHT, 0, 0, "panel", bp, (size_t)800 * PANEL_HEIGHT * 4);
    uint32_t a = wm_add_window(&w, 2, WK_NORMAL, 200, 100, 0, 0, "A", b1, 80000), b = wm_add_window(&w, 3, WK_NORMAL, 200, 100, 0, 0, "B", b2, 80000);
    Win *wa = wm_find(&w, a), *wb = wm_find(&w, b); (void)panel;
    wm_pointer_to(&w, 123, 456); T(w.ptr_x == 123 && w.ptr_y == 456, "wm: wm_pointer_to places the pointer exactly");
    wm_pointer_to(&w, -9, 99999); T(w.ptr_x == 0 && w.ptr_y == 599, "wm: wm_pointer_to clamps to the screen");
    wm_pointer_to(&w, 799, 0); wm_pointer_move(&w, 50, -50); T(w.ptr_x == 799 && w.ptr_y == 0, "wm: relative motion cannot push past the edge");
    wm_pointer_move(&w, -10, 10); T(w.ptr_x == 789 && w.ptr_y == 10, "wm: leaving an edge responds to the first count (no accumulated overshoot)");

    /* focus + stacking by click, with the press delivered to the window under the pointer and not to the one below */
    wm_pointer_to(&w, wa->x + 5, wa->y + 5); c.n = 0; wm_button(&w, 1); wm_button(&w, 0);
    T(w.focus == (int)a, "wm: clicking a window gives it focus"); T(count(&c, E_POINTER, a) >= 2, "wm: the clicked window receives press and release");
    /* a press inside window A's area that is covered by B must go to B */
    wa = wm_find(&w, a); wb = wm_find(&w, b); wm_pointer_to(&w, wb->x + 5, wb->y + 5); wm_button(&w, 1); wm_button(&w, 0);
    T(w.focus == (int)b || w.focus == (int)a, "wm: focus follows the click target");
    /* click counting */
    Wm *pw = &w; wm_set_time(pw, 1000); wm_pointer_to(pw, wa->x + 20, wa->y + 20); c.n = 0; wm_button(pw, 1); wm_button(pw, 0);
    const Msg *pe = NULL; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_POINTER && c.last[i].v[0] == 1) pe = &c.last[i];
    T(pe && pe->v[5] == 1, "wm: first click has click count 1");
    wm_set_time(pw, 1200); c.n = 0; wm_button(pw, 1); wm_button(pw, 0); pe = NULL; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_POINTER && c.last[i].v[0] == 1) pe = &c.last[i];
    T(pe && pe->v[5] == 2, "wm: second click within the interval counts as a double click");
    wm_set_time(pw, 1400); c.n = 0; wm_button(pw, 1); wm_button(pw, 0); pe = NULL; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_POINTER && c.last[i].v[0] == 1) pe = &c.last[i];
    T(pe && pe->v[5] == 3, "wm: third quick click counts 3");
    wm_set_time(pw, 5000); c.n = 0; wm_button(pw, 1); wm_button(pw, 0); pe = NULL; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_POINTER && c.last[i].v[0] == 1) pe = &c.last[i];
    T(pe && pe->v[5] == 1, "wm: a slow click restarts the count");
    wm_set_time(pw, 5100); wm_pointer_to(pw, wa->x + 90, wa->y + 60); c.n = 0; wm_button(pw, 1); wm_button(pw, 0); pe = NULL; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_POINTER && c.last[i].v[0] == 1) pe = &c.last[i];
    T(pe && pe->v[5] == 1, "wm: a quick click somewhere else is not a double click");
    /* buttons: right/middle are forwarded with their number, wheel as kind 5 */
    wm_pointer_to(pw, wa->x + 20, wa->y + 20); c.n = 0; wm_button_ex(pw, 2, 1); wm_button_ex(pw, 2, 0);
    pe = NULL; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_POINTER && c.last[i].v[0] == 1) pe = &c.last[i];
    T(pe && pe->v[3] == 2, "wm: right button press is delivered with button number 2");
    c.n = 0; wm_button_ex(pw, 3, 1); wm_button_ex(pw, 3, 0); pe = NULL; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_POINTER && c.last[i].v[0] == 1) pe = &c.last[i];
    T(pe && pe->v[3] == 3, "wm: middle button press is delivered with button number 3");
    c.n = 0; wm_wheel(pw, 2); pe = NULL; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_POINTER && c.last[i].v[0] == 5) pe = &c.last[i];
    T(pe && pe->v[3] == 2, "wm: wheel is delivered as kind 5 with the notch count");
    /* a right-click on a title bar must not drag or close */
    wa = wm_find(&w, a); int ox = wa->x, oy = wa->y; wm_pointer_to(pw, wa->x + 50, wa->y - 10); wm_button_ex(pw, 2, 1); wm_pointer_move(pw, 30, 30); wm_button_ex(pw, 2, 0);
    T(wa->x == ox && wa->y == oy, "wm: dragging with the right button does not move a window");
    /* left drag moves exactly by the pointer delta and does not leave the window stuck */
    wm_pointer_to(pw, wa->x + 50, wa->y - 10); wm_button(pw, 1); wm_pointer_move(pw, 33, 17); wm_pointer_move(pw, -3, 3); wm_button(pw, 0);
    T(wa->x == ox + 30 && wa->y == oy + 20, "wm: left title drag moves by the exact delta");
    int hx = wa->x; wm_pointer_move(pw, 100, 0); wm_button(pw, 1); wm_pointer_move(pw, 40, 0); wm_button(pw, 0); T(wa->x == hx, "wm: after a drag ends, plain motion/press elsewhere does not move the window");
    /* double-click on the title bar toggles maximise */
    wm_set_flags(pw, a, WF_RESIZABLE); wa = wm_find(&w, a); ox = wa->x; wm_set_time(pw, 9000); wm_pointer_to(pw, wa->x + 50, wa->y - 10); wm_button(pw, 1); wm_button(pw, 0); wm_set_time(pw, 9150); wm_button(pw, 1); wm_button(pw, 0);
    T(wa->pending == 1 || wa->maximized, "wm: double-clicking the title bar requests maximise");

    /* workspaces */
    wm_switch_workspace(pw, 1); T(w.ws == 1, "wm: workspace switch takes effect");
    c.n = 0; wm_pointer_to(pw, 400, 300); wm_button(pw, 1); wm_button(pw, 0);
    int to_hidden = 0; for (int i = 0; i < c.n; i++) if (c.clients[i] == 2 || c.clients[i] == 3) to_hidden += c.last[i].type == E_POINTER && c.last[i].v[0] == 1; T(to_hidden == 0, "wm: windows of another workspace receive no clicks");
    wm_switch_workspace(pw, 0); T(w.ws == 0, "wm: switching back");

    /* keyboard shortcuts */
    c.n = 0; wm_key(pw, KEY_T, 0); wm_key(pw, KEY_LEFTCTRL, 1); wm_key(pw, KEY_LEFTALT, 1); wm_key(pw, KEY_T, 1); wm_key(pw, KEY_T, 0); wm_key(pw, KEY_LEFTALT, 0); wm_key(pw, KEY_LEFTCTRL, 0);
    int hk2 = 0, keyed = 0; for (int i = 0; i < c.n; i++) { if (c.last[i].type == E_HOTKEY && c.last[i].v[0] == 2) hk2++; if (c.last[i].type == E_KEY && c.last[i].v[0] == KEY_T && c.last[i].v[2]) keyed++; }
    T(hk2 == 1, "wm: Ctrl+Alt+T sends hotkey 2 (terminal) to the panel once"); T(keyed == 0, "wm: the Ctrl+Alt+T chord is not typed into the focused app");
    c.n = 0; wm_key(pw, KEY_LEFTMETA, 1); wm_key(pw, KEY_LEFTMETA, 0); int hk1 = 0; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_HOTKEY && c.last[i].v[0] == 1) hk1++; T(hk1 == 1, "wm: tapping Super sends hotkey 1 (launcher)");
    int before = w.focus; wm_key(pw, KEY_LEFTALT, 1); wm_key(pw, KEY_TAB, 1); wm_key(pw, KEY_TAB, 0); wm_key(pw, KEY_LEFTALT, 0); T(w.focus != before && w.focus != 0, "wm: Alt+Tab cycles keyboard focus to another window");
    c.n = 0; wm_key(pw, KEY_CAPSLOCK, 1); wm_key(pw, KEY_CAPSLOCK, 0); wm_key(pw, KEY_A, 1); wm_key(pw, KEY_A, 0);
    int upper = 0; for (int i = 0; i < c.n; i++) if (c.last[i].type == E_KEY && c.last[i].v[0] == KEY_A && c.last[i].v[2] && c.last[i].v[1] == 'A') upper = 1; T(upper, "wm: Caps Lock turns letters into capitals");
    wm_key(pw, KEY_CAPSLOCK, 1); wm_key(pw, KEY_CAPSLOCK, 0);
    free(bp); free(b1); free(b2);
}

/* ---- terminal emulator core ---- */
static void vt_str(Vt *v, const char *s) { vt_feed(v, s, strlen(s)); }
static void test_term_vt(void)
{
    Vt *v = vt_new(20, 5, 100); T(v != NULL, "vt: created");
    T(vt_new(0, 5, 10) == NULL && vt_new(5, 0, 10) == NULL, "vt: bad sizes rejected");
    vt_str(v, "hello"); T(vt_cell(v, 0, 0)->ch == 'h' && vt_cell(v, 0, 4)->ch == 'o' && v->cx == 5 && v->cy == 0, "vt: text lands at the cursor");
    vt_str(v, "\r\nworld"); T(vt_cell(v, 1, 0)->ch == 'w' && v->cy == 1, "vt: CR LF moves to the next line start");
    vt_str(v, "\b\bXY"); T(vt_cell(v, 1, 3)->ch == 'X' && vt_cell(v, 1, 4)->ch == 'Y', "vt: backspace moves left, overwrite");
    vt_str(v, "\x1b[2J\x1b[H"); T(vt_cell(v, 0, 0)->ch == ' ' || vt_cell(v, 0, 0)->ch == 0, "vt: erase screen clears"); T(v->cx == 0 && v->cy == 0, "vt: cursor home");
    vt_str(v, "\x1b[3;5Hz"); T(vt_cell(v, 2, 4)->ch == 'z', "vt: CSI H positions the cursor (1-based)");
    vt_str(v, "\x1b[2A\x1b[3Cq"); T(vt_cell(v, 0, 8)->ch == 'q', "vt: relative cursor movement A/C");
    vt_reset(v); vt_str(v, "abcdefghijklmnopqrst"); T(v->wrap_pending && v->cy == 0, "vt: filling the last column defers the wrap (xterm rule)");
    vt_str(v, "u"); T(v->cy == 1 && vt_cell(v, 1, 0)->ch == 'u' && vt_abs_wrapped(v, vt_abs_lines(v) - 5), "vt: next character wraps and marks the line as wrapped");
    vt_reset(v); for (int i = 0; i < 12; i++) { char l[32]; snprintf(l, sizeof l, "line%d\r\n", i); vt_str(v, l); }
    T(v->sb_n >= 7, "vt: lines scrolled off the top go to scrollback"); char tx[128]; vt_text(v, 0, 0, 0, 10, tx, sizeof tx); T(!strcmp(tx, "line0"), "vt: scrollback keeps the oldest line text");
    vt_reset(v); vt_str(v, "\x1b[1;31mR\x1b[0mN\x1b[7mV\x1b[27m\x1b[4mU"); const VtCell *c0 = vt_cell(v, 0, 0);
    T((c0->attr & VA_BOLD) && c0->fg == 1, "vt: SGR bold + red"); T(vt_cell(v, 0, 1)->fg == VT_DEFAULT && vt_cell(v, 0, 1)->attr == 0, "vt: SGR 0 resets");
    T(vt_cell(v, 0, 2)->attr & VA_REVERSE, "vt: SGR 7 reverse"); T(vt_cell(v, 0, 3)->attr & VA_UNDERLINE, "vt: SGR 4 underline");
    vt_reset(v); vt_str(v, "\x1b[38;5;200mA\x1b[48;2;1;2;3mB"); T(vt_cell(v, 0, 0)->fg == 200 && vt_cell(v, 0, 1)->bg < 256, "vt: 256-colour and truecolour mapped into the palette");
    vt_reset(v); char rep[64]; vt_str(v, "\x1b[3;4H\x1b[6n"); size_t n = vt_take_reply(v, rep, sizeof rep); rep[n] = 0; T(!strcmp(rep, "\x1b[3;4R"), "vt: cursor position report (DSR 6) reply");
    vt_str(v, "\x1b[c"); n = vt_take_reply(v, rep, sizeof rep); T(n > 0, "vt: device attributes reply");
    vt_reset(v); vt_str(v, "\xc3\xa9x"); T(vt_cell(v, 0, 0)->ch == 0xe9 && vt_cell(v, 0, 1)->ch == 'x', "vt: UTF-8 decoded to one cell");
    vt_str(v, "\xc3"); vt_str(v, "(a"); T(1, "vt: truncated UTF-8 sequence does not crash");
    vt_reset(v); vt_str(v, "main"); vt_str(v, "\x1b[?1049h"); T(v->alt, "vt: alternate screen entered"); vt_str(v, "ALT"); vt_str(v, "\x1b[?1049l"); T(!v->alt && vt_cell(v, 0, 0)->ch == 'm', "vt: leaving the alternate screen restores the main one");
    vt_str(v, "\x1b[?1h"); T(v->app_cursor, "vt: application cursor keys mode"); vt_str(v, "\x1b[?25l"); T(!v->cursor_visible, "vt: cursor hide");
    vt_reset(v); vt_str(v, "\x1b]0;title\x07Z"); T(vt_cell(v, 0, 0)->ch == 'Z', "vt: OSC string is consumed and ignored");
    vt_reset(v); vt_str(v, "ab\x1b[1;2H\x1b[1P"); T(vt_cell(v, 0, 0)->ch == 'a' && vt_cell(v, 0, 1)->ch != 'b', "vt: delete character");
    vt_reset(v); vt_str(v, "one two three"); int c0b, c1b; T(vt_word_bounds(v, vt_abs_lines(v) - 5, 5, &c0b, &c1b) && c0b == 4 && c1b == 6, "vt: word bounds for double-click selection");
    vt_text(v, vt_abs_lines(v) - 5, 4, vt_abs_lines(v) - 5, 6, tx, sizeof tx); T(!strcmp(tx, "two"), "vt: text extraction of a selected range");
    vt_resize(v, 10, 3); T(v->cols == 10 && v->rows == 3 && v->cy < 3, "vt: resize keeps the cursor inside"); vt_resize(v, 40, 12); T(v->cols == 40, "vt: grow");
    vt_str(v, "\x1b[99999;99999H\x1b[99999A\x1b[0;0H\x1b[;H\x1b[999999999999999999m"); T(v->cx >= 0 && v->cx < v->cols && v->cy >= 0 && v->cy < v->rows, "vt: absurd CSI parameters stay inside the screen");
    unsigned seed = 12345; for (int i = 0; i < 20000; i++) { char b[64]; for (int j = 0; j < 64; j++) { seed = seed * 1103515245u + 12345u; b[j] = (char)(seed >> 16); } vt_feed(v, b, sizeof b); if (i % 2000 == 0) vt_resize(v, 5 + (int)(seed % 60), 2 + (int)(seed % 20)); }
    T(v->cx >= 0 && v->cx < v->cols && v->cy >= 0 && v->cy < v->rows, "vt: 1.28 MB of random bytes with resizes (sanitizer-checked) leaves a valid state");
    T(vt_palette(0) != vt_palette(1) && vt_palette(255) != 0xffffffffu, "vt: palette has distinct entries");
    vt_free(v);
}

/* ---- file manager core ---- */
static void test_fm_core(void)
{
    char o[4096]; fm_normalize(o, sizeof o, "/a//b/./c/../d/"); T(!strcmp(o, "/a/b/d"), "fm: normalize collapses //, . and ..");
    fm_normalize(o, sizeof o, "/.."); T(!strcmp(o, "/"), "fm: .. above root stays at /"); fm_normalize(o, sizeof o, ""); T(o[0] == '/', "fm: empty path becomes absolute");
    fm_parent(o, sizeof o, "/usr/bin"); T(!strcmp(o, "/usr"), "fm: parent of /usr/bin"); fm_parent(o, sizeof o, "/usr"); T(!strcmp(o, "/"), "fm: parent of /usr is /"); fm_parent(o, sizeof o, "/"); T(!strcmp(o, "/"), "fm: parent of / is /");
    fm_join(o, sizeof o, "/", "etc"); T(!strcmp(o, "/etc"), "fm: join at root"); fm_join(o, sizeof o, "/usr", "bin"); T(!strcmp(o, "/usr/bin"), "fm: join");
    fm_format_size(o, sizeof o, 0); T(!strcmp(o, "0 B"), "fm: size 0"); fm_format_size(o, sizeof o, 1536); T(!strcmp(o, "1.5 KiB"), "fm: size KiB"); fm_format_size(o, sizeof o, 5LL * 1024 * 1024 * 1024); T(strstr(o, "GiB") != NULL, "fm: size GiB");
    char m[11]; fm_format_mode(m, S_IFDIR | 0755); T(!strcmp(m, "drwxr-xr-x"), "fm: mode string for a directory"); fm_format_mode(m, S_IFREG | 0640); T(!strcmp(m, "-rw-r-----"), "fm: mode string for a file");
    fm_format_time(o, sizeof o, 0); T(!strcmp(o, "1970-01-01 00:00"), "fm: time formatting (UTC)");

    char dir[] = "/tmp/aether-fm-XXXXXX"; if (!mkdtemp(dir)) exit(2); char p[4200];
    snprintf(p, sizeof p, "%s/Banana", dir); close(open(p, O_CREAT | O_WRONLY, 0644)); snprintf(p, sizeof p, "%s/apple", dir); { int f = open(p, O_CREAT | O_WRONLY, 0644); (void)!write(f, "hello\n", 6); close(f); }
    snprintf(p, sizeof p, "%s/zdir", dir); mkdir(p, 0755); snprintf(p, sizeof p, "%s/.hidden", dir); close(open(p, O_CREAT | O_WRONLY, 0644));
    snprintf(p, sizeof p, "%s/lnk", dir); (void)!symlink("zdir", p); snprintf(p, sizeof p, "%s/broken", dir); (void)!symlink("/nonexistent-target", p);
    FmDir d; memset(&d, 0, sizeof d); char err[128] = "";
    T(fm_load(&d, dir, 0, err, sizeof err) == 0, "fm: loading a real directory works");
    T(d.n == 5 && d.hidden_skipped == 1, "fm: hidden entry skipped and counted (5 shown)");
    { int seen_file = 0, bad = 0; for (int i = 0; i < d.n; i++) { if (!d.e[i].is_dir) seen_file = 1; else if (seen_file) bad = 1; } T(d.n >= 5 && !bad, "fm: directories sort before files"); }
    int ia = -1, ib = -1; for (int i = 0; i < d.n; i++) { if (!strcmp(d.e[i].name, "apple")) ia = i; if (!strcmp(d.e[i].name, "Banana")) ib = i; }
    T(ia >= 0 && ib >= 0 && ia < ib, "fm: case-insensitive name order (apple before Banana)");
    int fsz = -1, linkdir = 0, brokenok = 0; for (int i = 0; i < d.n; i++) { if (!strcmp(d.e[i].name, "apple")) fsz = (int)d.e[i].size; if (!strcmp(d.e[i].name, "lnk")) linkdir = d.e[i].is_link && d.e[i].is_dir; if (!strcmp(d.e[i].name, "broken")) brokenok = d.e[i].is_link && d.e[i].broken && !d.e[i].is_dir; }
    T(fsz == 6, "fm: real file size from stat"); T(linkdir, "fm: symlink to a directory is a link and openable"); T(brokenok, "fm: broken symlink flagged, not openable");
    T(fm_load(&d, dir, 1, err, sizeof err) == 0 && d.n == 6 && d.hidden_skipped == 0, "fm: show-hidden lists the dotfile too");
    char before[4096]; snprintf(before, sizeof before, "%s", d.path); int nb = d.n;
    T(fm_load(&d, "/nonexistent-aether-dir", 0, err, sizeof err) == -1 && strstr(err, "No such file or directory"), "fm: nonexistent path -> system error text");
    T(!strcmp(d.path, before) && d.n == nb, "fm: a failed load leaves the previous listing intact");
    snprintf(p, sizeof p, "%s/apple", dir); T(fm_load(&d, p, 0, err, sizeof err) == -1 && err[0], "fm: loading a regular file as a directory fails with an error");
    snprintf(p, sizeof p, "%s/noaccess", dir); mkdir(p, 0000); if (geteuid() != 0) T(fm_load(&d, p, 0, err, sizeof err) == -1 && strstr(err, "Permission denied"), "fm: permission denied reported"); else T(1, "fm: permission test skipped as root");
    snprintf(p, sizeof p, "%s/apple", dir); char pv[64]; int txt = 0; long got = fm_preview(p, pv, sizeof pv, &txt, err, sizeof err); T(got == 6 && txt && !strncmp(pv, "hello", 5), "fm: text preview of a real file");
    snprintf(p, sizeof p, "%s/bin", dir); { int f = open(p, O_CREAT | O_WRONLY, 0644); (void)!write(f, "\x00\x01\x02\xff\xfe", 5); close(f); } got = fm_preview(p, pv, sizeof pv, &txt, err, sizeof err); T(got == 5 && !txt, "fm: binary file is not shown as text");
    T(fm_preview("/nonexistent-aether-file", pv, sizeof pv, &txt, err, sizeof err) == -1 && err[0], "fm: preview of a missing file reports an error");
    fm_free(&d); char cmd[600]; snprintf(cmd, sizeof cmd, "chmod 700 '%s/noaccess' 2>/dev/null; rm -rf '%s'", dir, dir); (void)!system(cmd);
}


/* ---- mouse / window-management regressions: invisible blockers, stale interaction state, drag near edges, edge resize ---- */
typedef struct { int n; Msg m[8192]; } Cap2;
static void cap2(void *ctx, int client, const Msg *m) { (void)client; Cap2 *c = ctx; if (c->n < 8192) c->m[c->n++] = *m; }
static uint32_t *keep[4096]; static int nkeep;                                       /* buffers stay reachable through this table: the WM only borrows them */
static uint32_t *kbuf(int w, int h, uint32_t color) { uint32_t *p = buf(w, h, color); if (nkeep < 4096) keep[nkeep++] = p; return p; }
static int n_ptr(const Cap2 *c, uint32_t win, int kind) { int k = 0; for (int i = 0; i < c->n; i++) if (c->m[i].type == E_POINTER && c->m[i].window == win && c->m[i].v[0] == kind) k++; return k; }
static int n_req(const Cap2 *c, uint32_t win) { int k = 0; for (int i = 0; i < c->n; i++) if (c->m[i].type == E_CONFIGURE && c->m[i].window == win && c->m[i].v[4] == 1) k++; return k; }
static const Msg *last_req(const Cap2 *c, uint32_t win) { for (int i = c->n - 1; i >= 0; i--) if (c->m[i].type == E_CONFIGURE && c->m[i].window == win && c->m[i].v[4] == 1) return &c->m[i]; return NULL; }
/* Play the client: answer the newest resize request with a buffer of exactly the requested size (what the real UI toolkit does). */
static int answer(Wm *w, Cap2 *c, uint32_t id) { const Msg *r = last_req(c, id); if (!r || !wm_find(w, id) || !wm_find(w, id)->pending) return 0; int nw = r->v[2], nh = r->v[3]; return wm_replace_buffer(w, id, nw, nh, kbuf(nw, nh, 0xaa0000), (size_t)nw * (size_t)nh * 4, NULL) == 0; }
#define DESK_COLOR 0x303030u
static Wm *mkwm(Cap2 *c, int sw, int sh, uint32_t *panel, uint32_t *desk)
{
    memset(c, 0, sizeof *c); Wm *w = calloc(1, sizeof *w); wm_init(w, sw, sh, cap2, c);
    uint32_t p = wm_add_window(w, 1, WK_PANEL, sw, PANEL_HEIGHT, 0, 0, "panel", kbuf(sw, PANEL_HEIGHT, 0x101010), (size_t)sw * PANEL_HEIGHT * 4);
    uint32_t d = wm_add_window(w, 2, WK_DESKTOP, sw, sh - PANEL_HEIGHT, 0, PANEL_HEIGHT, "desktop", kbuf(sw, sh - PANEL_HEIGHT, DESK_COLOR), (size_t)sw * (size_t)(sh - PANEL_HEIGHT) * 4);
    if (panel) *panel = p; if (desk) *desk = d; return w;
}
static uint32_t mkwin(Wm *w, int client, int cw, int ch, uint32_t color, int resizable)
{
    uint32_t id = wm_add_window(w, client, WK_NORMAL, cw, ch, 0, 0, "win", kbuf(cw, ch, color), (size_t)cw * (size_t)ch * 4);
    if (resizable) wm_set_flags(w, id, WF_RESIZABLE); return id;
}
/* A point inside the grab band for the edge mask, 2 px in from the painted frame. */
static void grip_pt(const Win *x, int mask, int *px, int *py)
{
    int bx = x->x, by = x->y - WM_TITLE_H, bw = x->w, bh = x->h + WM_TITLE_H;
    *px = (mask & WM_EDGE_L) ? bx + 2 : ((mask & WM_EDGE_R) ? bx + bw - 3 : bx + bw / 2);
    *py = (mask & WM_EDGE_T) ? by + 2 : ((mask & WM_EDGE_B) ? by + bh - 3 : by + bh / 2);
}

static void test_wm_mouse_regressions(void)
{
    Cap2 *c = malloc(sizeof *c); uint32_t panel, desk; uint32_t hid; int ed;

    /* --- 1. invisible blockers: hit regions never exceed what is painted --- */
    Wm *w = mkwm(c, 800, 600, &panel, &desk);
    uint32_t pop = wm_add_window(w, 3, WK_POPUP, 200, 200, 300, 200, "launcher", kbuf(200, 200, 0x0000ff), 200 * 200 * 4); wm_set_flags(w, pop, WF_TRANSPARENT);
    T(wm_hit_test(w, 400, 300, &hid, &ed) == WM_HIT_CLIENT && hid == desk, "blockers: a fully transparent popup (launcher mid fade) does not catch the pointer");
    c->n = 0; click_at(w, 400, 300); T(n_ptr(c, pop, 1) == 0 && n_ptr(c, desk, 1) == 1, "blockers: a click through a transparent popup reaches the desktop beneath");
    wm_set_opacity(w, pop, 255); T(wm_hit_test(w, 400, 300, &hid, &ed) == WM_HIT_CLIENT && hid == pop, "blockers: the popup catches the pointer again once it is visible");
    T(wm_hit_test(w, 300, 200, &hid, &ed) != WM_HIT_CLIENT || hid != pop, "blockers: the rounded corner pixel of a popup is not part of the popup");
    T(wm_hit_test(w, 300 + TH_R_MENU, 200 + TH_R_MENU, &hid, &ed) == WM_HIT_CLIENT && hid == pop, "blockers: just inside the popup's rounded corner is the popup");
    wm_set_opacity(w, pop, 0); wm_remove_window(w, pop);
    uint32_t nt = wm_add_window(w, 4, WK_NOTIFY, 200, 60, 0, 0, "n", kbuf(200, 60, 0x00ff00), 200 * 60 * 4); wm_set_flags(w, nt, WF_TRANSPARENT);
    { Win *n = wm_find(w, nt); T(wm_hit_test(w, n->x + 40, n->y + 20, &hid, &ed) == WM_HIT_CLIENT && hid == desk, "blockers: a fully transparent notification does not catch the pointer"); wm_set_opacity(w, nt, 200); T(wm_hit_test(w, n->x + 40, n->y + 20, &hid, &ed) == WM_HIT_CLIENT && hid == nt, "blockers: a fading notification still does, until it is gone"); }
    wm_remove_window(w, nt);

    uint32_t a = mkwin(w, 5, 300, 200, 0xaa0000, 1); Win *wa = wm_find(w, a);
    { int bx = wa->x, by = wa->y - WM_TITLE_H, bw = wa->w, bh = wa->h + WM_TITLE_H, ok = 1;
      int cx[4] = { bx, bx + bw - 1, bx, bx + bw - 1 }, cy[4] = { by, by, by + bh - 1, by + bh - 1 };
      for (int i = 0; i < 4; i++) { int part = wm_hit_test(w, cx[i], cy[i], &hid, &ed); if (hid == a) ok = 0; (void)part; }
      T(ok, "blockers: the four rounded corner pixels of a window frame belong to whatever is beneath");
      T(wm_hit_test(w, bx + TH_R_WIN, by + TH_R_WIN, &hid, &ed) != WM_HIT_NONE && hid == a, "blockers: just inside the rounded corner is the window");
      T(wm_hit_test(w, bx - 3, by + bh / 2, &hid, &ed) == WM_HIT_CLIENT && hid == desk && wm_hit_test(w, bx + bw + 2, by + bh / 2, &hid, &ed) == WM_HIT_CLIENT && hid == desk && wm_hit_test(w, bx + bw / 2, by + bh + 2, &hid, &ed) == WM_HIT_CLIENT && hid == desk, "blockers: 2+ px outside the frame (shadow area) is the desktop, not the window"); }
    uint32_t fixed = mkwin(w, 6, 200, 150, 0x00aa00, 0); Win *wf = wm_find(w, fixed);
    T(wm_hit_test(w, wf->x - 1, wf->y + 50, &hid, &ed) != WM_HIT_EDGE && hid != fixed, "blockers: the outline of a non-resizable window is not a grab zone");
    wm_task_activate(w, fixed); wm_hit_test(w, wf->x + 50, wf->y + 50, &hid, &ed); T(hid != fixed && wf->minimized, "blockers: a minimised window leaves no hit region behind");
    wm_task_activate(w, fixed); wm_switch_workspace(w, 1); wm_hit_test(w, wf->x + 50, wf->y + 50, &hid, &ed); T(hid == desk, "blockers: windows of another workspace leave no hit region behind (workspace 2 is empty: only the desktop)"); wm_switch_workspace(w, 0);

    /* whole-screen invariant: where the desktop wallpaper shows nothing may catch the click; where a window's pixels show, that window must */
    { uint32_t b2 = mkwin(w, 7, 260, 180, 0x00cc00, 1), b3 = mkwin(w, 8, 220, 160, 0x0000cc, 1), hidden = mkwin(w, 9, 240, 150, 0xcccc00, 1), other;
      wm_switch_workspace(w, 1); other = mkwin(w, 10, 240, 150, 0xcc00cc, 1); wm_switch_workspace(w, 0);      /* one minimised window here, one on workspace 2 */
      wm_task_activate(w, hidden);
      Win *w2 = wm_find(w, b2), *w3 = wm_find(w, b3); w2->x = 200; w2->y = 200; w3->x = 330; w3->y = 260;
      uint32_t p1 = wm_add_window(w, 11, WK_POPUP, 120, 90, 560, 80, "menu", kbuf(120, 90, 0xcc0000), 120 * 90 * 4);
      uint32_t p0 = wm_add_window(w, 12, WK_POPUP, 120, 90, 80, 400, "ghost", kbuf(120, 90, 0xcc0033), 120 * 90 * 4); wm_set_flags(w, p0, WF_TRANSPARENT);
      uint32_t n1 = wm_add_window(w, 13, WK_NOTIFY, 180, 50, 0, 0, "note", kbuf(180, 50, 0xcc6600), 180 * 50 * 4);
      Surface s = mk(800, 600); wm_compose_scene(w, &s); int bad_desk = 0, bad_win = 0, tested = 0;
      static const uint32_t cols[] = { 0xaa0000, 0x00cc00, 0x0000cc, 0xcc0000, 0xcc6600 }; uint32_t ids[] = { a, b2, b3, p1, n1 };
      for (int y = PANEL_HEIGHT; y < 600; y += 2) for (int x = 0; x < 800; x += 2) {
          uint32_t px = at(&s, x, y), who = 0; int part = wm_hit_test(w, x, y, &who, &ed); tested++;
          if (px == DESK_COLOR && !(who == desk || who == 0)) bad_desk++;
          for (int k = 0; k < 5; k++) if (px == cols[k] && !(who == ids[k] && (part == WM_HIT_CLIENT || (part == WM_HIT_EDGE && k < 3)))) bad_win++;   /* the grip band lies inside a resizable window's painted content, by design */
          if (who == p0 || who == hidden || who == other) bad_desk++;
      }
      T(tested > 100000 && bad_desk == 0, "sweep: no window, popup or notification catches the pointer where the wallpaper is visible (800x600, every 2nd pixel)");
      T(bad_win == 0, "sweep: every pixel showing a window's own content routes the pointer to that window");
      free(s.px); }
    free(w);

    /* --- 2. stale interaction state must not outlive the window it acts on --- */
    w = mkwm(c, 800, 600, &panel, &desk); a = mkwin(w, 5, 300, 200, 0xaa0000, 1); wa = wm_find(w, a);
    ptr_to(w, wa->x + 60, wa->y - 10); wm_button(w, 1); T(w->drag == (int)a, "state: pressing the title bar starts a drag");
    wm_task_activate(w, a); T(w->drag == 0 && wa->minimized, "state: minimising a window (panel chip) mid-drag ends the drag");
    { int ox = wa->x, oy = wa->y; ptr_to(w, 100, 100); T(wa->x == ox && wa->y == oy, "state: a minimised window no longer follows the pointer"); wm_button(w, 0); }
    wm_task_activate(w, a); ptr_to(w, wa->x + 50, wa->y + 50); wm_button(w, 1); T(w->grab == (int)a, "state: pressing client content grabs the pointer");
    wm_task_activate(w, a); T(w->grab == 0, "state: minimising the grabbed window releases the grab");
    wm_button(w, 0); wm_task_activate(w, a);
    { Win *g = wm_find(w, a); int cx, cy; grip_pt(g, WM_EDGE_R, &cx, &cy); ptr_to(w, cx, cy); wm_button(w, 1); T(w->rsz == (int)a, "state: pressing the right edge starts a resize"); wm_remove_window(w, a); T(w->rsz == 0, "state: removing a window mid-resize ends the resize"); ptr_to(w, 400, 300); wm_button(w, 0); T(w->rsz == 0 && w->drag == 0 && w->grab == 0, "state: the pointer is free again after the window vanished"); }
    a = mkwin(w, 5, 300, 200, 0xaa0000, 1); wa = wm_find(w, a); ptr_to(w, wa->x + 60, wa->y - 10); wm_button(w, 1); wm_remove_window(w, a); T(w->drag == 0, "state: removing a window mid-drag ends the drag"); wm_button(w, 0);
    a = mkwin(w, 5, 300, 200, 0xaa0000, 1); wa = wm_find(w, a); { int cx, cy; grip_pt(wa, WM_EDGE_B, &cx, &cy); ptr_to(w, cx, cy); wm_button(w, 1); wm_switch_workspace(w, 2); T(w->rsz == 0, "state: switching workspace mid-resize ends the resize"); wm_button(w, 0); wm_switch_workspace(w, 0); }
    free(w);

    /* --- 3. dragging into every screen edge and corner --- */
    w = mkwm(c, 1280, 800, &panel, &desk); a = mkwin(w, 5, 400, 300, 0xaa0000, 1); wa = wm_find(w, a);
    { static const int tx[8] = { 0, 640, 1279, 0, 1279, 0, 640, 1279 }, ty[8] = { 0, 0, 0, 400, 400, 799, 799, 799 }; int ok_ptr = 1, ok_win = 1, ok_free = 1;
      for (int k = 0; k < 8; k++) {
          wm_pointer_to(w, 500, 300); wm_button(w, 0); wm_pointer_to(w, wa->x + 150, wa->y - 12); int gx = w->ptr_x - wa->x, gy = w->ptr_y - wa->y; wm_button(w, 1);
          for (int st = 1; st <= 6; st++) wm_pointer_to(w, 500 + (tx[k] - 500) * st / 6, 300 + (ty[k] - 300) * st / 6);
          if (w->ptr_x != tx[k] || w->ptr_y != ty[k]) ok_ptr = 0;                              /* the pointer itself must reach the edge pixel */
          if (wa->y - WM_TITLE_H < PANEL_HEIGHT || wa->x < 80 - wa->w || wa->x > 1280 - 80 || wa->y > 800 - WM_TITLE_H) ok_win = 0;   /* title bar stays reachable */
          wm_button(w, 0); if (w->drag || w->grab || w->rsz) ok_free = 0;
          wm_pointer_to(w, wa->x + 150, wa->y - 12); wm_button(w, 1); wm_pointer_to(w, 500, 300); int moved = wa->x + gx == 500 || wa->x + gx >= 500 - 1; wm_button(w, 0);   /* and the window can be dragged away again */
          if (!moved && wa->y - WM_TITLE_H > PANEL_HEIGHT) ok_free = 0;
          (void)gy;
      }
      T(ok_ptr, "edges: the pointer reaches all 4 edges and 4 corners of the screen during a drag (no invisible wall)");
      T(ok_win, "edges: a dragged window keeps its title bar reachable at every edge and corner");
      T(ok_free, "edges: after every edge/corner drag no drag/grab/resize state is left behind and the window drags back out"); }
    { int tx = 640; wm_pointer_to(w, 640, 400); wm_button(w, 0); wm_pointer_to(w, wa->x + 100, wa->y - 12); int d0 = wa->x; wm_button(w, 1); wm_pointer_to(w, w->ptr_x + 37, w->ptr_y + 21); wm_pointer_to(w, w->ptr_x - 7, w->ptr_y + 3); wm_button(w, 0); (void)tx;
      T(wa->x == d0 + 30, "drag: away from the edges the window follows the pointer 1:1"); }
    free(w);

    /* --- 4. resize grips: where they are, where they are not --- */
    w = mkwm(c, 800, 600, &panel, &desk); a = mkwin(w, 5, 300, 200, 0xaa0000, 1); wa = wm_find(w, a);
    { static const int masks[8] = { WM_EDGE_L, WM_EDGE_R, WM_EDGE_T, WM_EDGE_B, WM_EDGE_L | WM_EDGE_T, WM_EDGE_R | WM_EDGE_T, WM_EDGE_L | WM_EDGE_B, WM_EDGE_R | WM_EDGE_B }; int ok = 1;
      for (int k = 0; k < 8; k++) { int px, py; grip_pt(wa, masks[k], &px, &py); int part = wm_hit_test(w, px, py, &hid, &ed); if (!(part == WM_HIT_EDGE && hid == a && ed == masks[k])) ok = 0; }
      T(ok, "resize: all 8 edges and corners of a resizable window are grips with the right edge mask");
      int cx = wa->x + wa->w / 2, cy = wa->y + wa->h / 2; T(wm_hit_test(w, cx, cy, &hid, &ed) == WM_HIT_CLIENT && hid == a, "resize: the middle of the window is still client content");
      T(wm_hit_test(w, wa->x + 100, wa->y - WM_TITLE_H / 2, &hid, &ed) == WM_HIT_TITLE && hid == a, "resize: the middle of the title bar is still the title bar (drag zone)");
      T(wm_hit_test(w, wa->x + WM_RESIZE_BAND + 1, cy, &hid, &ed) == WM_HIT_CLIENT, "resize: the grip band is only WM_RESIZE_BAND px wide");
      int bx, by; btn_at(wa, 0, &bx, &by); T(wm_hit_test(w, bx, by, &hid, &ed) == WM_HIT_CLOSE && hid == a, "resize: the close button still wins over the top-right grip");
      btn_at(wa, 1, &bx, &by); T(wm_hit_test(w, bx, by, &hid, &ed) == WM_HIT_MAX, "resize: the maximise button still works"); btn_at(wa, 2, &bx, &by); T(wm_hit_test(w, bx, by, &hid, &ed) == WM_HIT_MIN, "resize: the minimise button still works");
      T(wm_hit_test(w, wa->x - 1, cy, &hid, &ed) == WM_HIT_EDGE && wm_hit_test(w, wa->x - 2, cy, &hid, &ed) != WM_HIT_EDGE, "resize: the 1 px painted outline is grabbable, one pixel further out is not"); }
    uint32_t fx = mkwin(w, 6, 200, 150, 0x00aa00, 0); Win *wfx = wm_find(w, fx);
    { static const int masks[4] = { WM_EDGE_L, WM_EDGE_R, WM_EDGE_B, WM_EDGE_R | WM_EDGE_B }; int ok = 1; for (int k = 0; k < 4; k++) { int px, py; grip_pt(wfx, masks[k], &px, &py); if (wm_hit_test(w, px, py, &hid, &ed) == WM_HIT_EDGE) ok = 0; } T(ok, "resize: a window without WF_RESIZABLE has no grips"); }
    free(w);

    /* --- 5. resizing: geometry, anchors, protocol --- */
    w = mkwm(c, 800, 600, &panel, &desk); a = mkwin(w, 5, 300, 200, 0xaa0000, 1); wa = wm_find(w, a);
    wm_set_time(w, 1000); int gx0 = wa->x, gy0 = wa->y, gw0 = wa->w, gh0 = wa->h, px, py;
    grip_pt(wa, WM_EDGE_R, &px, &py); c->n = 0; ptr_to(w, px, py); wm_button(w, 1); T(w->rsz == (int)a && n_ptr(c, a, 1) == 0, "resize: pressing a grip does not deliver the press to the client");
    ptr_to(w, px + 60, py); { const Msg *r = last_req(c, a); T(r && r->v[2] == gw0 + 60 && r->v[3] == gh0 && r->v[0] == gx0 && r->v[1] == gy0, "resize: dragging the right edge asks the client for a buffer 60 px wider, position unchanged"); }
    T(wa->w == gw0, "resize: the window keeps its shape until the new buffer arrives"); T(answer(w, c, a) && wa->w == gw0 + 60 && wa->h == gh0 && wa->x == gx0 && wa->y == gy0, "resize: the reply gives the wider window, same position");
    wm_button(w, 0); T(w->rsz == 0 && n_ptr(c, a, 0) + n_ptr(c, a, 2) == 0, "resize: releasing ends it; the client saw no pointer events during it");
    gx0 = wa->x; gy0 = wa->y; gw0 = wa->w; gh0 = wa->h; int rt = gx0 + gw0, bt = gy0 + gh0;
    wm_set_time(w, 2000); grip_pt(wa, WM_EDGE_L, &px, &py); ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px - 40, py); answer(w, c, a); wm_button(w, 0);
    T(wa->w == gw0 + 40 && wa->x == gx0 - 40 && wa->x + wa->w == rt, "resize: dragging the left edge outward keeps the right edge fixed");
    gx0 = wa->x; gw0 = wa->w; wm_set_time(w, 3000); grip_pt(wa, WM_EDGE_T, &px, &py); ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px, py - 10); answer(w, c, a); wm_button(w, 0);
    T(wa->h == gh0 + 10 && wa->y == gy0 - 10 && wa->y + wa->h == bt, "resize: dragging the top edge upward keeps the bottom edge fixed");
    gy0 = wa->y; gh0 = wa->h; wm_set_time(w, 4000); grip_pt(wa, WM_EDGE_B, &px, &py); ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px, py + 50); answer(w, c, a); wm_button(w, 0);
    T(wa->h == gh0 + 50 && wa->y == gy0, "resize: dragging the bottom edge changes only the height");
    gx0 = wa->x; gy0 = wa->y; gw0 = wa->w; gh0 = wa->h; rt = gx0 + gw0; bt = gy0 + gh0; wm_set_time(w, 5000);
    grip_pt(wa, WM_EDGE_L | WM_EDGE_T, &px, &py); ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px + 25, py + 15); answer(w, c, a); wm_button(w, 0);
    T(wa->w == gw0 - 25 && wa->h == gh0 - 15 && wa->x + wa->w == rt && wa->y + wa->h == bt, "resize: the top-left corner shrinks both ways, bottom-right corner stays put");
    gx0 = wa->x; gy0 = wa->y; wm_set_time(w, 6000); grip_pt(wa, WM_EDGE_R | WM_EDGE_B, &px, &py); gw0 = wa->w; gh0 = wa->h; ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px + 45, py + 35); answer(w, c, a); wm_button(w, 0);
    T(wa->w == gw0 + 45 && wa->h == gh0 + 35 && wa->x == gx0 && wa->y == gy0, "resize: the bottom-right corner grows both ways, top-left stays put");
    gx0 = wa->x; gy0 = wa->y; gw0 = wa->w; gh0 = wa->h; rt = gx0 + gw0; bt = gy0 + gh0; wm_set_time(w, 7000);
    grip_pt(wa, WM_EDGE_R | WM_EDGE_T, &px, &py); ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px - 30, py + 20); answer(w, c, a); wm_button(w, 0);
    T(wa->w == gw0 - 30 && wa->h == gh0 - 20 && wa->x == gx0 && wa->y + wa->h == bt, "resize: the top-right corner moves the top and right edges");
    gx0 = wa->x; gy0 = wa->y; gw0 = wa->w; gh0 = wa->h; rt = gx0 + gw0; bt = gy0 + gh0; wm_set_time(w, 8000);
    grip_pt(wa, WM_EDGE_L | WM_EDGE_B, &px, &py); ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px - 20, py + 10); answer(w, c, a); wm_button(w, 0);
    T(wa->w == gw0 + 20 && wa->h == gh0 + 10 && wa->x + wa->w == rt && wa->y == gy0, "resize: the bottom-left corner moves the left and bottom edges");
    free(w);

    /* --- 6. resize limits, throttling, misbehaving clients --- */
    w = mkwm(c, 800, 600, &panel, &desk); a = mkwin(w, 5, 400, 300, 0xaa0000, 1); wa = wm_find(w, a); wm_set_time(w, 1000);
    grip_pt(wa, WM_EDGE_R | WM_EDGE_B, &px, &py); ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px - 2000, py - 2000); { const Msg *r = last_req(c, a); T(r && r->v[2] == WM_MIN_W && r->v[3] == WM_MIN_H, "limits: a resize cannot shrink the client area below the minimum"); } answer(w, c, a);
    ptr_to(w, 799, 599); { const Msg *r = last_req(c, a); T(r && wa->x + r->v[2] <= 800 && wa->y + r->v[3] <= 600, "limits: a resize cannot grow past the screen edge"); } answer(w, c, a); wm_button(w, 0);
    free(w);
    w = mkwm(c, 800, 600, &panel, &desk); a = mkwin(w, 5, 200, 100, 0xaa0000, 1); wa = wm_find(w, a); wm_set_time(w, 1000);
    grip_pt(wa, WM_EDGE_R, &px, &py); ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px - 100, py); { const Msg *r = last_req(c, a); T(r == NULL || r->v[2] == 200, "limits: a window already smaller than the minimum keeps its own size as the floor"); } wm_button(w, 0);
    free(w);
    w = mkwm(c, 800, 600, &panel, &desk); a = mkwin(w, 5, 300, 200, 0xaa0000, 1); wa = wm_find(w, a); wm_set_time(w, 1000);
    { int x0 = wa->x; grip_pt(wa, WM_EDGE_T, &px, &py); ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px, 0); const Msg *r = last_req(c, a); T(r && r->v[1] >= PANEL_HEIGHT + WM_TITLE_H, "limits: dragging the top edge cannot push the title bar under the panel"); answer(w, c, a); wm_button(w, 0); T(wa->x == x0, "limits: ... and the x position is untouched"); }
    free(w);
    w = mkwm(c, 800, 600, &panel, &desk); a = mkwin(w, 5, 300, 200, 0xaa0000, 1); wa = wm_find(w, a); wm_set_time(w, 1000);
    grip_pt(wa, WM_EDGE_R, &px, &py); ptr_to(w, px, py); wm_button(w, 1); c->n = 0;
    ptr_to(w, px + 10, py); ptr_to(w, px + 20, py); ptr_to(w, px + 35, py); T(n_req(c, a) == 1, "throttle: only one resize request is outstanding at a time");
    answer(w, c, a); T(n_req(c, a) == 2 && last_req(c, a)->v[2] == 300 + 35, "throttle: the reply triggers one follow-up for the newest size, skipping the stale ones");
    ptr_to(w, px + 50, py); wm_button(w, 0); answer(w, c, a); T(last_req(c, a)->v[2] == 300 + 50 && n_req(c, a) == 3, "throttle: releasing mid-flight still lands on the final pointer position");
    answer(w, c, a); T(wa->w == 350 && !wa->pending, "throttle: the window ends exactly where the pointer was released"); { int n = n_req(c, a); ptr_to(w, 100, 100); T(n_req(c, a) == n, "throttle: no requests are sent after the resize is over"); }
    free(w);
    w = mkwm(c, 800, 600, &panel, &desk); a = mkwin(w, 5, 300, 200, 0xaa0000, 1); wa = wm_find(w, a); wm_set_time(w, 1000);
    grip_pt(wa, WM_EDGE_R, &px, &py); ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px + 80, py); { int gx = wa->x; wm_replace_buffer(w, a, 330, 200, kbuf(330, 200, 0xaa0000), 330 * 200 * 4, NULL); int n = n_req(c, a); T(wa->w == 330 && wa->x == gx && n_req(c, a) == n, "client: a client that answers with a different size is accepted and not asked again in a loop"); } wm_button(w, 0);
    free(w);
    w = mkwm(c, 800, 600, &panel, &desk); a = mkwin(w, 5, 400, 300, 0xaa0000, 1); wa = wm_find(w, a); wm_set_time(w, 1000);
    { int right = wa->x + wa->w, bottom = wa->y + wa->h; grip_pt(wa, WM_EDGE_L | WM_EDGE_T, &px, &py); ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px - 20, py - 10);
      wm_replace_buffer(w, a, 400 + 5, 300 + 3, kbuf(405, 303, 0xaa0000), 405 * 303 * 4, NULL);                       /* the client wanted 420x310 but answers 405x303 */
      T(wa->x + wa->w == right && wa->y + wa->h == bottom, "client: when a client answers with another size, the right and bottom edges of a left/top drag still do not move"); wm_button(w, 0); }
    free(w);
    w = mkwm(c, 800, 600, &panel, &desk); a = mkwin(w, 5, 300, 200, 0xaa0000, 1); wa = wm_find(w, a); wm_set_time(w, 1000);
    grip_pt(wa, WM_EDGE_R, &px, &py); ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px + 40, py); int n0 = n_req(c, a); ptr_to(w, px + 60, py); T(n_req(c, a) == n0 && wa->pending == 3, "client: while the client has not answered, nothing new is requested");
    wm_set_time(w, 1000 + WM_RESIZE_TIMEOUT_MS + 10); ptr_to(w, px + 70, py); T(n_req(c, a) == n0 + 1, "client: a client that never answers cannot lock the window; the request expires and a new one is sent"); wm_button(w, 0);
    free(w);

    /* --- 7. resize without the clamp that would snap a window back, and with overlapping windows --- */
    w = mkwm(c, 800, 600, &panel, &desk); a = mkwin(w, 5, 300, 200, 0xaa0000, 1); wa = wm_find(w, a); wm_set_time(w, 1000);
    ptr_to(w, wa->x + 60, wa->y - 12); wm_button(w, 1); ptr_to(w, 700, wa->y - 12 + 40); wm_button(w, 0);
    { int sticks_out = wa->x + wa->w > 800; T(sticks_out, "snap: setup: the window was dragged partly off the right edge"); int rr = wa->x + wa->w; grip_pt(wa, WM_EDGE_L, &px, &py);
      ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px - 30, py); answer(w, c, a); wm_button(w, 0); T(wa->x + wa->w == rr && wa->w == 330, "snap: resizing a window that sticks out is not snapped back onto the screen (no jump)"); }
    free(w);
    w = mkwm(c, 800, 600, &panel, &desk); a = mkwin(w, 5, 300, 200, 0xaa0000, 1); uint32_t b = mkwin(w, 6, 300, 200, 0x00aa00, 1); wa = wm_find(w, a); Win *wb = wm_find(w, b);
    wa->x = 100; wa->y = 150; wb->x = 250; wb->y = 200; wm_task_activate(w, a); wm_task_activate(w, b);          /* b above a, overlapping a's right edge */
    { int ex = wa->x + wa->w - 3, ey = wa->y + 60; T(ex > wb->x && ey > wb->y - WM_TITLE_H && ey < wb->y + wb->h, "overlap: setup: a's right grip is covered by b");
      c->n = 0; ptr_to(w, ex, ey); T(wm_hit_test(w, ex, ey, &hid, &ed) != WM_HIT_EDGE && hid == b, "overlap: a grip hidden under another window is not a grip");
      wm_button(w, 1); T(w->rsz == 0 && n_ptr(c, b, 1) == 1, "overlap: pressing there reaches the window on top, not a resize of the one below"); wm_button(w, 0);
      int vx = wa->x + wa->w - 3, vy = wa->y - WM_TITLE_H + 28; T(vy > wb->y + wb->h || vx > wb->x + wb->w || vy < wb->y - WM_TITLE_H, "overlap: setup: another stretch of a's edge is uncovered");
      T(wm_hit_test(w, vx, vy, &hid, &ed) == WM_HIT_EDGE && hid == a, "overlap: the visible part of the lower window's edge is still a grip");
      wm_button(w, 0); ptr_to(w, wb->x + 100, wb->y - 12); wm_button(w, 1); ptr_to(w, wb->x + 130, wb->y - 12 + 5); wm_button(w, 0); }
    free(w);

    /* --- 8. maximise / minimise / close / focus keep working, also after resizing --- */
    w = mkwm(c, 800, 600, &panel, &desk); a = mkwin(w, 5, 300, 200, 0xaa0000, 1); b = mkwin(w, 6, 250, 180, 0x00aa00, 1); wa = wm_find(w, a); wb = wm_find(w, b); wb->x = 450; wb->y = 300; wm_set_time(w, 1000);
    grip_pt(wa, WM_EDGE_R, &px, &py); ptr_to(w, px, py); wm_button(w, 1); ptr_to(w, px + 20, py); answer(w, c, a); wm_button(w, 0); T(wa->w == 320 && w->focus == (int)a && !wa->maximized, "after: a resized window is focused and raised");
    { int mx, my; btn_at(wa, 1, &mx, &my); c->n = 0; click_at(w, mx, my); T(wa->pending == 1 && n_req(c, a) == 1, "after: the maximise button still requests a maximise"); answer(w, c, a); T(wa->maximized && wa->w == 800, "after: and the window maximises");
      int cx, cy; grip_pt(wa, WM_EDGE_B, &cx, &cy); T(wm_hit_test(w, cx, cy, &hid, &ed) != WM_HIT_EDGE, "after: a maximised window has no grips");
      btn_at(wa, 1, &mx, &my); wm_set_time(w, 2000); click_at(w, mx, my); answer(w, c, a); T(!wa->maximized && wa->w == 320, "after: restore returns to the resized size");
      grip_pt(wa, WM_EDGE_B, &cx, &cy); T(wm_hit_test(w, cx, cy, &hid, &ed) == WM_HIT_EDGE, "after: grips are back after restore");
      btn_at(wa, 2, &mx, &my); click_at(w, mx, my); T(wa->minimized && w->focus == (int)b, "after: the minimise button still minimises and focus moves on");
      wm_task_activate(w, a); T(!wa->minimized && w->focus == (int)a, "after: restoring from the panel focuses the window");
      click_at(w, wb->x + 40, wb->y + 40); T(w->focus == (int)b, "after: clicking another window focuses and raises it");
      btn_at(wb, 0, &mx, &my); c->n = 0; click_at(w, mx, my); int closes = 0; for (int i = 0; i < c->n; i++) if (c->m[i].type == E_CLOSE && c->m[i].window == b) closes++; T(closes == 1, "after: the close button still asks the window to close (exactly once)"); }
    free(w);

    /* --- 9. coordinate mapping reaches every edge pixel --- */
    { static const int sz[][2] = { { 800, 600 }, { 1024, 768 }, { 1280, 800 }, { 1366, 768 }, { 1920, 1080 }, { 3840, 2160 } }; int ok_min = 1, ok_max = 1, ok_mono = 1, ok_all = 1;
      for (unsigned k = 0; k < sizeof sz / sizeof sz[0]; k++) {
          int W = sz[k][0], H = sz[k][1]; if (input_abs_scale(0, 0, 32767, W) != 0 || input_abs_scale(0, 0, 32767, H) != 0) ok_min = 0;
          if (input_abs_scale(32767, 0, 32767, W) != W - 1 || input_abs_scale(32767, 0, 32767, H) != H - 1) ok_max = 0;
          int prev = -1; char *seen = calloc((size_t)W, 1); for (int v = 0; v <= 32767; v++) { int p = input_abs_scale(v, 0, 32767, W); if (p < prev || p < 0 || p >= W) ok_mono = 0; prev = p; seen[p] = 1; }
          for (int i = 0; i < W; i++) if (!seen[i]) ok_all = 0; free(seen);
      }
      T(ok_min && ok_max, "mapping: a tablet's first and last coordinate land exactly on pixel 0 and size-1 (6 resolutions)");
      T(ok_mono && ok_all, "mapping: scaling is monotonic and every display column is reachable (no dead pixels or invisible bands)"); }
    { Cap2 *c2 = c; Wm *wr = mkwm(c2, 1280, 800, NULL, NULL); wm_pointer_to(wr, 1279, 799); T(wr->ptr_x == 1279 && wr->ptr_y == 799, "mapping: the last pixel of the display is reachable"); wm_resize_screen(wr, 800, 600); T(wr->ptr_x == 799 && wr->ptr_y == 599, "mapping: a shrinking display re-clamps the pointer onto the new screen"); wm_resize_screen(wr, 1280, 800); wm_pointer_to(wr, 1279, 799); T(wr->ptr_x == 1279 && wr->ptr_y == 799, "mapping: a growing display makes the new area reachable"); free(wr); }
    free(c);
}

/* ===================================================================================== right-click context menus */
static int cm_has(const CmItem *it, size_t n, const char *label, const char *cmd)
{
    for (size_t i = 0; i < n; i++) if (!it[i].separator && !strcmp(it[i].label, label) && !strcmp(it[i].cmd, cmd)) return 1;
    return 0;
}
static void test_ctxmenu(void)
{
    CmItem it[CM_MAX_ITEMS]; CmKind k = CM_TASK; CmAction a;
    T(cm_kind_parse("desktop", &k) && k == CM_DESKTOP && cm_kind_parse("shortcut", &k) && k == CM_SHORTCUT && cm_kind_parse("task", &k) && k == CM_TASK, "ctxmenu: the three menu kinds parse");
    T(!cm_kind_parse("", &k) && !cm_kind_parse("Desktop", &k) && !cm_kind_parse("panel", &k) && !cm_kind_parse(NULL, &k), "ctxmenu: unknown or missing kinds are rejected");
    { int ok = 1; for (CmKind q = CM_DESKTOP; q <= CM_TASK; q++) { CmKind r; if (!cm_kind_parse(cm_kind_name(q), &r) || r != q) ok = 0; } T(ok, "ctxmenu: kind names round-trip"); }

    /* desktop menu: only applications that really exist, in a fixed order, then Refresh */
    const char *all[] = { "Ghost", "Settings", "Files", "Terminal", "Notes" };
    size_t n = cm_build(CM_DESKTOP, "", all, 5, it);
    T(n == 5 && !strcmp(it[0].label, "Terminal") && !strcmp(it[1].label, "Files") && !strcmp(it[2].label, "Settings") && it[3].separator && !strcmp(it[4].label, "Refresh"), "ctxmenu: desktop menu = Terminal, Files, Settings, separator, Refresh");
    T(cm_has(it, n, "Terminal", "open Terminal") && cm_has(it, n, "Files", "open Files") && cm_has(it, n, "Settings", "open Settings") && cm_has(it, n, "Refresh", "refresh"), "ctxmenu: desktop items carry the commands the owner will run");
    { int other = 0; for (size_t i = 0; i < n; i++) if (!it[i].separator && (!strcmp(it[i].label, "Ghost") || !strcmp(it[i].label, "Notes"))) other = 1; T(!other, "ctxmenu: applications outside Terminal/Files/Settings never appear in the desktop menu"); }
    const char *only_settings[] = { "Settings" };
    n = cm_build(CM_DESKTOP, NULL, only_settings, 1, it);
    T(n == 3 && !strcmp(it[0].label, "Settings") && it[1].separator && !strcmp(it[2].label, "Refresh"), "ctxmenu: Terminal and Files are left out when they are not installed (only what exists is offered)");
    n = cm_build(CM_DESKTOP, "", NULL, 0, it);
    T(n == 1 && !it[0].separator && !strcmp(it[0].cmd, "refresh"), "ctxmenu: with no applications the desktop menu is just Refresh (no dangling separator)");
    { const char *dup[] = { "Files", "Files" }; n = cm_build(CM_DESKTOP, "", dup, 2, it); T(n == 3, "ctxmenu: a duplicated application entry is offered once"); }

    /* shortcut menu */
    n = cm_build(CM_SHORTCUT, "Settings", NULL, 0, it);
    T(n == 1 && !strcmp(it[0].label, "Open") && !strcmp(it[0].cmd, "open Settings"), "ctxmenu: shortcut menu is a single Open for that application");
    n = cm_build(CM_SHORTCUT, "My Notes 2", NULL, 0, it); T(n == 1 && !strcmp(it[0].cmd, "open My Notes 2"), "ctxmenu: application names with spaces survive");
    { char longname[80]; memset(longname, 'x', sizeof longname - 1); longname[sizeof longname - 1] = 0;
      T(cm_build(CM_SHORTCUT, "", NULL, 0, it) == 0 && cm_build(CM_SHORTCUT, NULL, NULL, 0, it) == 0 && cm_build(CM_SHORTCUT, longname, NULL, 0, it) == 0 && cm_build(CM_SHORTCUT, "bad\nname", NULL, 0, it) == 0 && cm_build(CM_SHORTCUT, "bad\x01", NULL, 0, it) == 0, "ctxmenu: a shortcut menu without a valid application name is refused"); }

    /* task menu */
    n = cm_build(CM_TASK, "12", NULL, 0, it);
    T(n == 2 && !strcmp(it[0].label, "Activate") && !strcmp(it[0].cmd, "activate 12") && !strcmp(it[1].label, "Close") && !strcmp(it[1].cmd, "close 12"), "ctxmenu: task menu = Activate, Close for that window id");
    n = cm_build(CM_TASK, "4294967295", NULL, 0, it); T(n == 2, "ctxmenu: the largest 32-bit window id is accepted");
    { const char *bad[] = { "", "0", "abc", "12x", "-3", " 5", "4294967296", "99999999999", "00000000000" }; int ok = 1; for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) if (cm_build(CM_TASK, bad[i], NULL, 0, it) != 0) ok = 0;
      T(ok && cm_build(CM_TASK, NULL, NULL, 0, it) == 0, "ctxmenu: a task menu without a valid window id (empty, zero, non-numeric, negative, overflowing) is refused"); }

    /* every offered command parses back to the action the menu meant */
    { int ok = 1; n = cm_build(CM_DESKTOP, "", all, 5, it);
      for (size_t i = 0; i < n; i++) if (!it[i].separator) { if (!cm_parse_command(it[i].cmd, &a)) ok = 0; else if (!strcmp(it[i].cmd, "refresh") ? a.kind != CM_ACT_REFRESH : (a.kind != CM_ACT_OPEN || strcmp(a.name, it[i].label))) ok = 0; }
      T(ok, "ctxmenu: every desktop command round-trips through the parser"); }
    n = cm_build(CM_TASK, "77", NULL, 0, it);
    T(cm_parse_command(it[0].cmd, &a) && a.kind == CM_ACT_ACTIVATE && a.id == 77 && cm_parse_command(it[1].cmd, &a) && a.kind == CM_ACT_CLOSE && a.id == 77, "ctxmenu: task commands round-trip with their window id");
    n = cm_build(CM_SHORTCUT, "Files", NULL, 0, it); T(cm_parse_command(it[0].cmd, &a) && a.kind == CM_ACT_OPEN && !strcmp(a.name, "Files"), "ctxmenu: shortcut command round-trips with its application name");
    T(cm_parse_command("refresh\n", &a) && a.kind == CM_ACT_REFRESH && cm_parse_command("close 9\r\n", &a) && a.kind == CM_ACT_CLOSE && a.id == 9, "ctxmenu: a trailing newline / CRLF is accepted");
    { const char *bad[] = { "", "\n", "open", "open ", "open \x01x", "refresh now", "Refresh", "REFRESH", "activate", "activate 0", "activate 1 2", "activate x", "close -1", "close 4294967296", "close  5", "rm -rf /", "openFiles", " open Files" };
      int ok = 1; for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) if (cm_parse_command(bad[i], &a)) { ok = 0; printf("  accepted: '%s'\n", bad[i]); }
      char big[300]; memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = 0; memcpy(big, "open ", 5);
      T(ok && !cm_parse_command(big, &a) && !cm_parse_command(NULL, &a) && !cm_parse_command("refresh", NULL), "ctxmenu: malformed, unknown, oversized or control-character commands are rejected"); }

    /* geometry */
    n = cm_build(CM_DESKTOP, "", all, 5, it);
    T(cm_menu_h(it, n) == 2 * TH_MENU_PAD + 4 * TH_MENU_ITEM_H + TH_MENU_SEP_H && cm_item_y(it, 0) == TH_MENU_PAD && cm_item_y(it, 3) == TH_MENU_PAD + 3 * TH_MENU_ITEM_H && cm_item_y(it, 4) == TH_MENU_PAD + 3 * TH_MENU_ITEM_H + TH_MENU_SEP_H && cm_item_h(&it[3]) == TH_MENU_SEP_H && cm_item_h(&it[0]) == TH_MENU_ITEM_H, "ctxmenu: row positions and the menu height add up (separator rows are thinner)");
    { int mh = cm_menu_h(it, n), x, y;
      cm_place(100, 100, TH_MENU_W, mh, 1280, 800, PANEL_HEIGHT, &x, &y); T(x == 100 && y == 100, "ctxmenu: the menu opens with its corner at the pointer when it fits");
      cm_place(1270, 100, TH_MENU_W, mh, 1280, 800, PANEL_HEIGHT, &x, &y); T(x == 1280 - TH_MENU_W && y == 100, "ctxmenu: near the right edge the menu is pulled back onto the screen");
      cm_place(100, 790, TH_MENU_W, mh, 1280, 800, PANEL_HEIGHT, &x, &y); T(x == 100 && y == 790 - mh, "ctxmenu: near the bottom edge the menu opens upwards from the pointer");
      cm_place(1279, 799, TH_MENU_W, mh, 1280, 800, PANEL_HEIGHT, &x, &y); T(x + TH_MENU_W <= 1280 && y + mh <= 800 && y >= PANEL_HEIGHT, "ctxmenu: the bottom-right corner keeps the whole menu on screen");
      cm_place(10, 5, TH_MENU_W, mh, 1280, 800, PANEL_HEIGHT, &x, &y); T(y == PANEL_HEIGHT, "ctxmenu: a menu never covers the panel");
      cm_place(-50, 100, TH_MENU_W, mh, 1280, 800, PANEL_HEIGHT, &x, &y); T(x == 0, "ctxmenu: a negative x is clamped to the left edge");
      cm_place(100, 100, TH_MENU_W, mh, 150, 100, PANEL_HEIGHT, &x, &y); T(x == 0 && y == PANEL_HEIGHT, "ctxmenu: on a screen smaller than the menu it is pinned to the top-left of the work area (never off the top or left)"); }

    /* keyboard */
    T(cm_step(it, n, -1, 1) == 0 && cm_step(it, n, -1, -1) == 4, "ctxmenu keys: from no selection Down picks the first row and Up the last");
    T(cm_step(it, n, 2, 1) == 4 && cm_step(it, n, 4, -1) == 2, "ctxmenu keys: the separator is skipped in both directions");
    T(cm_step(it, n, 4, 1) == 0 && cm_step(it, n, 0, -1) == 4, "ctxmenu keys: selection wraps around");
    { CmItem one[CM_MAX_ITEMS]; size_t m1 = cm_build(CM_DESKTOP, "", NULL, 0, one); T(cm_step(one, m1, 0, 1) == 0 && cm_step(one, m1, -1, -1) == 0, "ctxmenu keys: a single row stays selected");
      T(cm_step(it, 0, -1, 1) == -1 && cm_step(it, n, 0, 0) == -1 && cm_step(it, n, 99, 1) == 0, "ctxmenu keys: empty menu / bad direction select nothing; a stale index is recovered from"); }
}

typedef struct { int n; Msg m[512]; int client[512]; } Cap3;
static void cap3(void *ctx, int client, const Msg *m) { Cap3 *c = ctx; if (c->n < 512) { c->m[c->n] = *m; c->client[c->n++] = client; } }
static int n_to(const Cap3 *c, uint32_t type, uint32_t win, int client)               /* messages of a type for a window, delivered to that client */
{ int k = 0; for (int i = 0; i < c->n; i++) if (c->m[i].type == type && (win == 0 || c->m[i].window == win) && (client < 0 || c->client[i] == client)) k++; return k; }
static int n_btn(const Cap3 *c, uint32_t win, int kind, int btn)                      /* pointer events of a kind with a given button number */
{ int k = 0; for (int i = 0; i < c->n; i++) if (c->m[i].type == E_POINTER && c->m[i].window == win && c->m[i].v[0] == kind && c->m[i].v[3] == btn) k++; return k; }
static const Msg *ptr_ev(const Cap3 *c, uint32_t win, int kind) { for (int i = c->n - 1; i >= 0; i--) if (c->m[i].type == E_POINTER && c->m[i].window == win && c->m[i].v[0] == kind) return &c->m[i]; return NULL; }
static void rbutton(Wm *w, int down) { wm_button_ex(w, 3, down); }

static void test_context_menu_protocol_and_wm(void)
{
    /* the wire format and every existing message id are unchanged; the new message is appended */
    T(M_HELLO == 1 && M_CREATE_WINDOW == 2 && M_COMMIT == 3 && M_DESTROY == 4 && M_SWITCH_WORKSPACE == 5 && M_SET_OPACITY == 6 && M_SET_BUFFER == 7 && M_TASK_ACTIVATE == 8, "proto: client->compositor message ids are unchanged");
    T(M_TASK_CLOSE == M_TASK_ACTIVATE + 1 && M_TASK_CLOSE == 9, "proto: M_TASK_CLOSE is appended after the last existing message");
    T(E_SCREEN == 100 && E_CONFIGURE == 101 && E_KEY == 102 && E_POINTER == 103 && E_CLOSE == 104 && E_WORKSPACE == 105 && E_TASK == 106 && E_HOTKEY == 107, "proto: compositor->client event ids are unchanged");
    T(sizeof(Msg) == 80 && AETHER_PROTO_VERSION == 1, "proto: the fixed message size and protocol version are unchanged (old clients keep working)");
    { Msg m; memset(&m, 0, sizeof m); m.type = M_TASK_CLOSE; m.window = 42; int sv[2]; T(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0 && proto_send(sv[0], &m, -1) == 0, "proto: M_TASK_CLOSE travels as an ordinary message");
      Msg r; int fd; T(proto_recv(sv[1], &r, &fd) == 1 && r.type == M_TASK_CLOSE && r.window == 42, "proto: M_TASK_CLOSE is received intact"); close(sv[0]); close(sv[1]); }

    Cap3 *c = calloc(1, sizeof *c); Wm *w = calloc(1, sizeof *w); wm_init(w, 800, 600, cap3, c);
    uint32_t panel = wm_add_window(w, 1, WK_PANEL, 800, PANEL_HEIGHT, 0, 0, "panel", kbuf(800, PANEL_HEIGHT, 0x101010), (size_t)800 * PANEL_HEIGHT * 4);
    uint32_t desk = wm_add_window(w, 2, WK_DESKTOP, 800, 600 - PANEL_HEIGHT, 0, PANEL_HEIGHT, "desktop", kbuf(800, 600 - PANEL_HEIGHT, DESK_COLOR), (size_t)800 * (size_t)(600 - PANEL_HEIGHT) * 4);
    uint32_t a = mkwin(w, 5, 300, 200, 0xaa0000, 1), b = mkwin(w, 6, 300, 200, 0x00aa00, 1); Win *wa = wm_find(w, a), *wb = wm_find(w, b);

    /* --- Close --- */
    c->n = 0; wm_task_close(w, a);
    T(n_to(c, E_CLOSE, a, 5) == 1 && n_to(c, E_CLOSE, 0, -1) == 1, "close: the window's own client gets exactly one E_CLOSE, nobody else");
    T(wm_find(w, a) != NULL && !wa->minimized, "close: the request is polite - the window stays until its client destroys it");
    wm_task_activate(w, b); T(wb->minimized == 1, "close: (setup) b minimised from its chip");
    c->n = 0; wm_task_close(w, b); T(n_to(c, E_CLOSE, b, 6) == 1 && wb->minimized == 1, "close: a minimised window can be closed from its chip");
    { uint32_t pop = wm_add_window(w, 7, WK_POPUP, 100, 60, 300, 200, "menu", kbuf(100, 60, 0x2222aa), 100 * 60 * 4); wm_set_opacity(w, pop, 255);
      c->n = 0; wm_task_close(w, panel); wm_task_close(w, desk); wm_task_close(w, pop); wm_task_close(w, 9999); wm_task_close(w, 0);
      T(n_to(c, E_CLOSE, 0, -1) == 0, "close: panel, desktop, popup, unknown and zero ids are ignored"); wm_remove_window(w, pop); }

    /* --- Activate (never minimises) --- */
    wm_task_raise(w, b); T(wb->minimized == 0 && w->focus == (int)b, "activate: a minimised window is restored, raised and focused");
    wm_task_raise(w, b); wm_task_raise(w, b); T(wb->minimized == 0 && w->focus == (int)b, "activate: the focused window stays visible and focused (unlike a chip click, which minimises it)");
    wm_task_raise(w, a); T(w->focus == (int)a && w->z[w->nz - 1] == (int)(wa - w->wins) && !wa->minimized, "activate: an unfocused window is raised to the top and focused");
    { int f = w->focus; wm_task_raise(w, panel); wm_task_raise(w, desk); wm_task_raise(w, 9999); T(w->focus == f, "activate: non-window and unknown targets are ignored"); }
    wm_task_activate(w, a); T(wa->minimized == 1, "activate: the existing chip toggle (wm_task_activate) still minimises the focused window");
    wm_task_raise(w, a); T(wa->minimized == 0 && w->focus == (int)a, "activate: and the menu action brings it back");
    wm_switch_workspace(w, 1); uint32_t cw = mkwin(w, 8, 200, 150, 0x0000aa, 1); Win *wc = wm_find(w, cw); wm_switch_workspace(w, 0);
    { int f = w->focus; wm_task_raise(w, cw); T(w->focus == f && wc->ws == 1 && !wc->minimized, "activate: a window of another workspace is left alone"); }

    /* --- right button routing (what the toolkit's on_context relies on) --- */
    wm_task_raise(w, a); wa = wm_find(w, a);
    c->n = 0; ptr_to(w, wa->x + 50, wa->y + 50); rbutton(w, 1); rbutton(w, 0);
    T(n_btn(c, a, 1, 3) == 1 && n_btn(c, a, 2, 3) == 1, "right button: a press and a release reach the client with button number 3");
    T(ptr_ev(c, a, 1)->v[1] == 50 && ptr_ev(c, a, 1)->v[2] == 50 && ptr_ev(c, a, 1)->v[4] == 0 && ptr_ev(c, a, 1)->v[5] == 1, "right button: the press carries window-relative coordinates, no dismissed popup, click count 1");
    c->n = 0; ptr_to(w, wa->x + 50, wa->y - WM_TITLE_H / 2); rbutton(w, 1); rbutton(w, 0);
    T(n_to(c, E_POINTER, a, -1) == 0 || n_btn(c, a, 1, 3) == 0, "right button: the title bar still answers only to the primary button");
    c->n = 0; ptr_to(w, 400, 15); rbutton(w, 1); rbutton(w, 0);
    T(n_btn(c, panel, 1, 3) == 1 && n_btn(c, panel, 2, 3) == 1, "right button: the panel (task chips) receives right clicks");
    c->n = 0; ptr_to(w, 700, 500); rbutton(w, 1); rbutton(w, 0);
    T(n_btn(c, desk, 1, 3) == 1 && n_btn(c, desk, 2, 3) == 1, "right button: the desktop surface receives right clicks");
    c->n = 0; ptr_to(w, 700, 500); wm_button(w, 1); wm_button(w, 0); T(n_btn(c, desk, 1, 1) == 1, "right button: the left button is still button 1");
    { uint32_t pop = wm_add_window(w, 9, WK_POPUP, 200, 120, 300, 200, "ctx", kbuf(200, 120, 0x2222aa), 200 * 120 * 4); wm_set_opacity(w, pop, 255);
      c->n = 0; ptr_to(w, 400, 250); rbutton(w, 1); rbutton(w, 0);
      T(n_to(c, E_CLOSE, pop, -1) == 0 && n_btn(c, pop, 1, 3) == 1, "right button: a right click inside an open menu does not dismiss it");
      c->n = 0; ptr_to(w, 700, 500); rbutton(w, 1);
      T(n_to(c, E_CLOSE, pop, 9) == 1 && ptr_ev(c, desk, 1) && ptr_ev(c, desk, 1)->v[3] == 3 && ptr_ev(c, desk, 1)->v[4] == 1, "right button: a right press elsewhere dismisses the menu and tells the target it did (so no second menu opens on that click)");
      rbutton(w, 0); wm_remove_window(w, pop); }
    free(w); free(c);
}

int main(void)
{
    test_gfx(); test_appmeta(); test_keys(); test_proto(); test_wm();
    test_theme(); test_gfx_aurora(); test_font(); test_icons(); test_wallpaper(); test_appmeta_aurora(); test_wm_aurora();
    test_input_new(); test_wm_input(); test_term_vt(); test_fm_core(); test_wm_mouse_regressions();
    test_ctxmenu(); test_context_menu_protocol_and_wm();
    printf("%d/%d GUI unit checks passed\n", total - fails, total); return fails ? 1 : 0;
}
