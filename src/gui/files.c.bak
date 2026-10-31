/* aether-files: a file manager for the real guest filesystem. Every row is a real directory entry (lstat), every error is the
 * system's own message, and nothing is created, changed or deleted: this is a browser. (No copy/move/delete/rename yet.)
 *
 *   aether-files [DIR]        (default: $HOME if it exists, else /)
 * Mouse: click selects, double-click opens a folder, toolbar buttons, wheel scrolls, click the path bar to type a path.
 * Keyboard: Up/Down/PgUp/PgDn/Home/End select, Enter opens, Backspace or Alt+Up = parent, Alt+Left = back, Ctrl+L or '/' = type a path,
 * Ctrl+H = show hidden files, F5 = reload, Ctrl+T = open a terminal here, a letter jumps to the next entry starting with it. */
#define _GNU_SOURCE
#include "fm_core.h"
#include "input.h"
#include "ui.h"
#include <ctype.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define TOOL_H 52
#define HEAD_H 26
#define ROW_H 28
#define INFO_H 112
#define STATUS_H 24
enum { B_BACK, B_UP, B_HOME, B_HIDDEN, B_TERM, B_COUNT };
static const char *const kBtn[B_COUNT] = { "Back", "Up", "Home", "Hidden", "Terminal" };
static FmDir cur; static int sel = -1, scroll, show_hidden, hover_row = -1, hover_btn = -1, press_btn = -1;
static int editing; static char edit_buf[1024];
static char status[256] = "", hist[32][4096]; static int status_err, nhist;
static char preview[3][96]; static int npreview; static char preview_note[96];
static Rect btn_r[B_COUNT], path_r;
static int widget = -1;

static int list_h(const UiWindow *w) { int h = w->h - TOOL_H - HEAD_H - INFO_H - STATUS_H; return h < ROW_H ? ROW_H : h; }
static int visible_rows(const UiWindow *w) { return list_h(w) / ROW_H; }
static void set_status(int err, const char *fmt, const char *a, const char *b) { status_err = err; snprintf(status, sizeof status, fmt, a, b); }

static void update_preview(void)
{
    npreview = 0; preview_note[0] = 0;
    if (sel < 0 || sel >= cur.n) return;
    const FmEntry *e = &cur.e[sel]; if (e->is_dir || !S_ISREG(e->mode)) return;
    if (e->size == 0) { snprintf(preview_note, sizeof preview_note, "Empty file"); return; }
    char full[5000], buf[1024], err[96]; int text; fm_join(full, sizeof full, cur.path, e->name);
    long n = fm_preview(full, buf, sizeof buf, &text, err, sizeof err);
    if (n < 0) { snprintf(preview_note, sizeof preview_note, "Cannot read: %.80s", err); return; }
    if (!text) { snprintf(preview_note, sizeof preview_note, "Binary file (no preview)"); return; }
    char *p = buf;
    while (npreview < 3 && *p) {
        char *nl = strchr(p, '\n'); size_t l = nl ? (size_t)(nl - p) : strlen(p); if (l > 90) l = 90;
        for (size_t i = 0; i < l; i++) preview[npreview][i] = (p[i] == '\t' || p[i] == '\r') ? ' ' : p[i];
        preview[npreview][l] = 0; npreview++; if (!nl) break; p = nl + 1;
    }
}
static void select_row(UiWindow *w, int i)
{
    if (cur.n == 0) { sel = -1; return; }
    sel = th_clamp(i, 0, cur.n - 1); int vr = visible_rows(w);
    if (sel < scroll) scroll = sel; else if (sel >= scroll + vr) scroll = sel - vr + 1;
    update_preview();
}
static void navigate(UiWindow *w, const char *path, int push, const char *select_name)
{
    char norm[4096], err[128]; fm_normalize(norm, sizeof norm, path);
    char old[4096]; snprintf(old, sizeof old, "%s", cur.path);
    if (fm_load(&cur, norm, show_hidden, err, sizeof err) != 0) {
        set_status(1, "Cannot open %s: %s", norm, err); printf("aether-files: cannot open %s: %s\n", norm, err); return;
    }
    if (push && old[0] && strcmp(old, cur.path) != 0) { if (nhist == 32) { memmove(hist[0], hist[1], 31 * sizeof hist[0]); nhist--; } snprintf(hist[nhist++], sizeof hist[0], "%s", old); }
    sel = -1; scroll = 0; npreview = 0; preview_note[0] = 0;
    if (select_name) for (int i = 0; i < cur.n; i++) if (!strcmp(cur.e[i].name, select_name)) { select_row(w, i); break; }
    char cnt[32]; snprintf(cnt, sizeof cnt, "%d", cur.n);
    if (cur.hidden_skipped) { char h[32]; snprintf(h, sizeof h, "%d hidden not shown", cur.hidden_skipped); set_status(0, "%s items (%s)", cnt, h); }
    else set_status(0, "%s items%s", cnt, "");
    printf("aether-files: opened %s (%d entries)\n", cur.path, cur.n);
}
static void go_up(UiWindow *w)
{
    if (!strcmp(cur.path, "/")) { set_status(0, "Already at the root folder%s%s", "", ""); return; }
    char par[4096], name[256]; fm_parent(par, sizeof par, cur.path); const char *b = strrchr(cur.path, '/'); snprintf(name, sizeof name, "%s", b ? b + 1 : "");
    navigate(w, par, 1, name);
}
static void go_back(UiWindow *w)
{
    if (nhist == 0) { set_status(0, "No earlier folder%s%s", "", ""); return; }
    char p[4096]; snprintf(p, sizeof p, "%s", hist[nhist - 1]); nhist--; navigate(w, p, 0, NULL);
}
static void open_terminal(void)
{
    pid_t p = fork();
    if (p == 0) { setsid(); execl("/bin/aether-terminal", "aether-terminal", "--cwd", cur.path, (char *)NULL); _exit(127); }
    if (p < 0) set_status(1, "Cannot start a terminal: %s%s", strerror(errno), "");
}
static void open_selected(UiWindow *w)
{
    if (sel < 0 || sel >= cur.n) return;
    const FmEntry *e = &cur.e[sel]; char full[5000]; fm_join(full, sizeof full, cur.path, e->name);
    if (e->is_dir) navigate(w, full, 1, NULL);
    else if (e->broken) set_status(1, "%s is a broken link%s", e->name, "");
    else set_status(0, "No application is registered to open %s (preview shown below)%s", e->name, "");
}
static void toolbar_action(UiWindow *w, int b)
{
    switch (b) {
    case B_BACK: go_back(w); break;
    case B_UP: go_up(w); break;
    case B_HOME: { const char *h = getenv("HOME"); navigate(w, h && *h ? h : "/", 1, NULL); break; }
    case B_HIDDEN: { show_hidden = !show_hidden; char keep[256] = ""; if (sel >= 0 && sel < cur.n) snprintf(keep, sizeof keep, "%s", cur.e[sel].name); char p[4096]; snprintf(p, sizeof p, "%s", cur.path); navigate(w, p, 0, keep[0] ? keep : NULL); break; }
    case B_TERM: open_terminal(); break;
    default: break;
    }
}

/* ---- drawing ------------------------------------------------------------------------------------------------------ */
static void layout_toolbar(UiWindow *w)
{
    const Face *f = font_face(FW_MEDIUM, TY_BODY_PX); int x = TH_S3;
    for (int i = 0; i < B_COUNT; i++) { int bw = font_width(f, kBtn[i]) + 2 * TH_S3; btn_r[i] = rect_make(x, (TOOL_H - TH_CTRL_H) / 2, bw, TH_CTRL_H); x += bw + TH_S1; }
    path_r = rect_make(x + TH_S2, (TOOL_H - TH_CTRL_H) / 2, w->w - x - TH_S2 - TH_S3, TH_CTRL_H);
}
static void draw_icon(Surface *s, int x, int y, const FmEntry *e)
{
    if (e->is_dir) { gfx_rrect(s, rect_make(x, y + 1, 7, 5), 2, TH_BLUE); gfx_rrect(s, rect_make(x, y + 3, 18, 13), 3, e->is_link ? TH_CYAN : TH_BLUE_HOVER); }
    else { gfx_rrect(s, rect_make(x + 2, y, 13, 17), 2, e->broken ? TH_ERR : (S_ISREG(e->mode) ? TH_TEXT2 : TH_WARN)); gfx_fill(s, rect_make(x + 5, y + 5, 7, 1), TH_WINDOW); gfx_fill(s, rect_make(x + 5, y + 9, 7, 1), TH_WINDOW); }
    if (e->is_link) gfx_disc(s, (float)(x + 15), (float)(y + 15), 3.5f, TH_CYAN, 255);
}
static void text(Surface *s, Rect box, const char *t, int weight, int px, uint32_t col, int align)
{
    char b[300]; const Face *f = font_face(weight, px); font_ellipsize(b, sizeof b, t, f, box.w); font_draw_box(s, box, b, f, col, align);
}
static void draw_all(UiWindow *w, int i, Surface *s)
{
    (void)i; gfx_fill(s, rect_make(0, 0, w->w, w->h), TH_WINDOW);
    layout_toolbar(w);
    gfx_fill(s, rect_make(0, 0, w->w, TOOL_H), TH_PANEL); gfx_fill(s, rect_make(0, TOOL_H - 1, w->w, 1), TH_BORDER);
    for (int b = 0; b < B_COUNT; b++) {
        int dis = (b == B_BACK && nhist == 0), on = (b == B_HIDDEN && show_hidden), hot = hover_btn == b && !dis, pr = press_btn == b && hot;
        uint32_t fill = on ? gfx_mix(TH_SURFACE2, TH_BLUE, 70) : (pr ? TH_SURFACE2_PRESS : (hot ? TH_SURFACE2_HOVER : TH_SURFACE2));
        gfx_rrect(s, btn_r[b], TH_R_CTRL, hot ? gfx_mix(TH_BORDER, TH_BLUE, 120) : TH_BORDER); gfx_rrect(s, rect_make(btn_r[b].x + 1, btn_r[b].y + 1, btn_r[b].w - 2, btn_r[b].h - 2), TH_R_CTRL - 1, fill);
        font_draw_box(s, btn_r[b], kBtn[b], font_face(FW_MEDIUM, TY_BODY_PX), dis ? TH_MUTED : TH_TEXT, FA_CENTER);
    }
    gfx_rrect(s, path_r, TH_R_CTRL, editing ? TH_BLUE : TH_BORDER); gfx_rrect(s, rect_make(path_r.x + 1, path_r.y + 1, path_r.w - 2, path_r.h - 2), TH_R_CTRL - 1, TH_DESKTOP);
    { char shown[1100]; snprintf(shown, sizeof shown, "%.1000s", editing ? edit_buf : cur.path); Rect tb = rect_make(path_r.x + TH_S3, path_r.y, path_r.w - 2 * TH_S3, path_r.h);
      int tw = font_width(font_face(FW_REGULAR, TY_BODY_PX), shown); text(s, tb, shown, FW_REGULAR, TY_BODY_PX, TH_TEXT, FA_LEFT);
      if (editing && (ui_now_ms() / 530) % 2 == 0 && tw < tb.w) gfx_fill(s, rect_make(tb.x + tw + 1, path_r.y + 8, 1, 16), TH_CYAN); }
    /* column header */
    int sizex = w->w - 270, timex = w->w - 168;
    gfx_fill(s, rect_make(0, TOOL_H, w->w, HEAD_H), TH_PANEL);
    text(s, rect_make(44, TOOL_H, sizex - 52, HEAD_H), "Name", FW_MEDIUM, TY_SMALL_PX, TH_TEXT2, FA_LEFT);
    text(s, rect_make(sizex, TOOL_H, 90, HEAD_H), "Size", FW_MEDIUM, TY_SMALL_PX, TH_TEXT2, FA_RIGHT);
    text(s, rect_make(timex, TOOL_H, 160, HEAD_H), "Modified (UTC)", FW_MEDIUM, TY_SMALL_PX, TH_TEXT2, FA_LEFT);
    gfx_fill(s, rect_make(0, TOOL_H + HEAD_H - 1, w->w, 1), TH_BORDER);
    /* rows */
    int top = TOOL_H + HEAD_H, vr = visible_rows(w);
    for (int r = 0; r < vr; r++) {
        int k = scroll + r; if (k >= cur.n) break; const FmEntry *e = &cur.e[k]; Rect row = rect_make(TH_S1, top + r * ROW_H, w->w - 2 * TH_S1, ROW_H - 2);
        if (k == sel) { gfx_rrect(s, row, TH_R_CTRL - 2, gfx_mix(TH_WINDOW, TH_BLUE, 60)); gfx_rrect(s, rect_make(row.x, row.y + 5, 3, row.h - 10), 1, TH_BLUE); }
        else if (k == hover_row) gfx_rrect(s, row, TH_R_CTRL - 2, TH_SURFACE2);
        draw_icon(s, 16, row.y + 4, e);
        text(s, rect_make(44, row.y, sizex - 52, row.h), e->name, k == sel ? FW_MEDIUM : FW_REGULAR, TY_BODY_PX, e->broken ? TH_ERR : TH_TEXT, FA_LEFT);
        char sz[32] = ""; if (!e->is_dir && S_ISREG(e->mode)) fm_format_size(sz, sizeof sz, e->size); else if (e->is_dir) snprintf(sz, sizeof sz, "--");
        text(s, rect_make(sizex, row.y, 90, row.h), sz, FW_REGULAR, TY_SMALL_PX, TH_TEXT2, FA_RIGHT);
        char tm[32]; fm_format_time(tm, sizeof tm, e->mtime); text(s, rect_make(timex, row.y, 160, row.h), e->broken && !e->mtime ? "" : tm, FW_REGULAR, TY_SMALL_PX, TH_TEXT2, FA_LEFT);
    }
    if (cur.n == 0) text(s, rect_make(0, top + 20, w->w, 24), "This folder is empty", FW_REGULAR, TY_BODY_PX, TH_TEXT2, FA_CENTER);
    if (cur.n > vr) {                                                         /* scroll bar */
        int th = list_h(w); int bh = th_max(24, th * vr / cur.n), by = top + (th - bh) * scroll / th_max(1, cur.n - vr);
        gfx_rrect(s, rect_make(w->w - 7, by, 4, bh), 2, TH_BORDER);
    }
    /* info panel */
    int iy = w->h - INFO_H - STATUS_H; gfx_fill(s, rect_make(0, iy, w->w, INFO_H), TH_PANEL); gfx_fill(s, rect_make(0, iy, w->w, 1), TH_BORDER);
    if (sel >= 0 && sel < cur.n) {
        const FmEntry *e = &cur.e[sel]; char l[400], sz[32], md[11], tm[32];
        fm_format_size(sz, sizeof sz, e->size); fm_format_mode(md, e->mode); fm_format_time(tm, sizeof tm, e->mtime);
        text(s, rect_make(TH_S4, iy + 8, w->w - 2 * TH_S4, 20), e->name, FW_MEDIUM, TY_BODY_PX + 1, TH_TEXT, FA_LEFT);
        snprintf(l, sizeof l, "%s%s%s   %s   uid %u gid %u", fm_kind(e), S_ISREG(e->mode) ? "  -  " : "", S_ISREG(e->mode) ? sz : "", md, (unsigned)e->uid, (unsigned)e->gid);
        text(s, rect_make(TH_S4, iy + 30, w->w - 2 * TH_S4, 16), l, FW_REGULAR, TY_SMALL_PX, TH_TEXT2, FA_LEFT);
        snprintf(l, sizeof l, "Modified %s UTC%s%s", tm, e->is_link ? "   Links to " : "", e->is_link ? e->link : "");
        text(s, rect_make(TH_S4, iy + 47, w->w - 2 * TH_S4, 16), l, FW_REGULAR, TY_SMALL_PX, TH_TEXT2, FA_LEFT);
        if (npreview) for (int p = 0; p < npreview; p++) gfx_text_clipped(s, rect_make(TH_S4, iy + 66 + p * 14, w->w - 2 * TH_S4, 14), TH_S4, iy + 66 + p * 14 - 1, preview[p], TH_MUTED);
        else if (preview_note[0]) text(s, rect_make(TH_S4, iy + 66, w->w - 2 * TH_S4, 16), preview_note, FW_REGULAR, TY_SMALL_PX, TH_MUTED, FA_LEFT);
    } else {
        char l[300]; snprintf(l, sizeof l, "%d item%s in this folder%s", cur.n, cur.n == 1 ? "" : "s", cur.hidden_skipped ? " (hidden files are not shown; press Ctrl+H)" : "");
        text(s, rect_make(TH_S4, iy + 8, w->w - 2 * TH_S4, 20), cur.path, FW_MEDIUM, TY_BODY_PX + 1, TH_TEXT, FA_LEFT);
        text(s, rect_make(TH_S4, iy + 30, w->w - 2 * TH_S4, 16), l, FW_REGULAR, TY_SMALL_PX, TH_TEXT2, FA_LEFT);
        text(s, rect_make(TH_S4, iy + 47, w->w - 2 * TH_S4, 16), "Select a file or folder to see its details.", FW_REGULAR, TY_SMALL_PX, TH_MUTED, FA_LEFT);
    }
    gfx_fill(s, rect_make(0, w->h - STATUS_H, w->w, STATUS_H), TH_DESKTOP);
    text(s, rect_make(TH_S4, w->h - STATUS_H, w->w - 2 * TH_S4, STATUS_H), status, FW_REGULAR, TY_SMALL_PX, status_err ? TH_ERR : TH_TEXT2, FA_LEFT);
}

/* ---- input -------------------------------------------------------------------------------------------------------- */
static int btn_at(int x, int y) { for (int b = 0; b < B_COUNT; b++) if (rect_contains(btn_r[b], x, y)) return b; return -1; }
static int row_at(const UiWindow *w, int x, int y)
{
    (void)x; int top = TOOL_H + HEAD_H; if (y < top || y >= top + visible_rows(w) * ROW_H) return -1;
    int k = scroll + (y - top) / ROW_H; return k < cur.n ? k : -1;
}
static void on_pointer(UiWindow *w, int kind, int x, int y, int button, int clicks)
{
    int changed = 0;
    if (kind == 5) { int old = scroll; scroll = th_clamp(scroll - button * 3, 0, th_max(0, cur.n - visible_rows(w))); if (scroll != old) ui_redraw(w); return; }
    if (kind == 4) { hover_row = hover_btn = -1; press_btn = -1; ui_redraw(w); return; }
    int hb = btn_at(x, y), hr = row_at(w, x, y);
    if (hb != hover_btn || hr != hover_row) { hover_btn = hb; hover_row = hr; changed = 1; }
    if (kind == 1 && button == 1) {
        if (editing && !rect_contains(path_r, x, y)) editing = 0;
        if (hb >= 0) { press_btn = hb; changed = 1; }
        else if (rect_contains(path_r, x, y)) { editing = 1; snprintf(edit_buf, sizeof edit_buf, "%.1000s", cur.path); changed = 1; }
        else if (hr >= 0) { select_row(w, hr); changed = 1; if (clicks >= 2) { open_selected(w); changed = 1; } }
    } else if (kind == 2 && button == 1) {
        if (press_btn >= 0) { int pb = press_btn; press_btn = -1; changed = 1; if (pb == hb) toolbar_action(w, pb); }
    }
    if (changed) ui_redraw(w);
}
static void on_key(UiWindow *w, int key, int ascii, int pressed)
{
    if (!pressed) return;
    int ctrl = (w->mods & KMOD_CTRL) != 0, alt = (w->mods & KMOD_ALT) != 0;
    if (editing) {
        size_t n = strlen(edit_buf);
        if (key == KEY_ESC) editing = 0;
        else if (key == KEY_ENTER || key == KEY_KPENTER) { editing = 0; char p[1024]; snprintf(p, sizeof p, "%s", edit_buf); navigate(w, p, 1, NULL); }
        else if (key == KEY_BACKSPACE) { if (n) edit_buf[n - 1] = 0; }
        else if (ascii >= 32 && ascii < 127 && n < sizeof edit_buf - 1) { edit_buf[n] = (char)ascii; edit_buf[n + 1] = 0; }
        ui_redraw(w); return;
    }
    int vr = visible_rows(w);
    if (ctrl && key == KEY_L) { editing = 1; snprintf(edit_buf, sizeof edit_buf, "%.1000s", cur.path); }
    else if (ctrl && key == KEY_H) toolbar_action(w, B_HIDDEN);
    else if (ctrl && key == KEY_T) open_terminal();
    else if (alt && key == KEY_UP) go_up(w);
    else if (alt && key == KEY_LEFT) go_back(w);
    else if (key == KEY_BACKSPACE) go_up(w);
    else if (key == KEY_F5) { char k[256] = "", p[4096]; if (sel >= 0 && sel < cur.n) snprintf(k, sizeof k, "%s", cur.e[sel].name); snprintf(p, sizeof p, "%s", cur.path); navigate(w, p, 0, k[0] ? k : NULL); }
    else if (key == KEY_DOWN) select_row(w, sel < 0 ? 0 : sel + 1);
    else if (key == KEY_UP) select_row(w, sel < 0 ? cur.n - 1 : sel - 1);
    else if (key == KEY_HOME) select_row(w, 0);
    else if (key == KEY_END) select_row(w, cur.n - 1);
    else if (key == KEY_PAGEDOWN) select_row(w, (sel < 0 ? 0 : sel) + vr);
    else if (key == KEY_PAGEUP) select_row(w, (sel < 0 ? 0 : sel) - vr);
    else if (key == KEY_ENTER || key == KEY_KPENTER) open_selected(w);
    else if (ascii == '/' ) { editing = 1; snprintf(edit_buf, sizeof edit_buf, "/"); }
    else if (isalnum(ascii) && !ctrl && !alt) {
        for (int k = 1; k <= cur.n; k++) { int i = ((sel < 0 ? -1 : sel) + k) % cur.n; if (tolower((unsigned char)cur.e[i].name[0]) == tolower(ascii)) { select_row(w, i); break; } }
    }
    ui_redraw(w);
}
static void layout(UiWindow *w) { ui_clear(w); widget = ui_custom(w, rect_make(0, 0, w->w, w->h), draw_all, NULL, NULL, 0, UI_ALL_PAGES); }
static void on_resize(UiWindow *w, int nw, int nh)
{
    char err[96]; if (ui_resize(w, nw, nh, err, sizeof err) != 0) return;
    layout(w); select_row(w, sel); ui_redraw(w);
}
static void on_tick(UiWindow *w) { if (editing) ui_redraw(w); }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0); signal(SIGCHLD, SIG_IGN);
    UiWindow w; char err[160];
    if (ui_connect(&w, err, sizeof err) != 0) { fprintf(stderr, "aether-files: %s\n", err); return 1; }
    int cw = th_min(760, w.screen_w - 2 * TH_S6), ch = th_min(520, w.screen_h - PANEL_HEIGHT - TH_TITLE_H - TH_S6);
    w.flags = WF_RESIZABLE | WF_CENTER;
    if (ui_create(&w, "Files", WK_NORMAL, cw, ch, 0, 0, err, sizeof err) != 0) { fprintf(stderr, "aether-files: %s\n", err); return 1; }
    layout(&w); layout_toolbar(&w);
    const char *start = argc > 1 ? argv[1] : getenv("HOME"); if (!start || !*start || access(start, R_OK) != 0) start = "/";
    navigate(&w, start, 0, NULL);
    if (cur.path[0] == 0) navigate(&w, "/", 0, NULL);
    w.on_pointer = on_pointer; w.on_key = on_key; w.on_resize = on_resize; w.on_tick = on_tick; w.tick_ms = 530;
    printf("aether-files: window ready (%dx%d) in %s\n", w.w, w.h, cur.path);
    ui_run(&w); ui_destroy(&w); fm_free(&cur); return 0;
}
