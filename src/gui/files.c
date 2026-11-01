/* aether-files: a file manager for the real guest filesystem. Every row is a real directory entry (lstat), every error is the
 * system's own message, and nothing is created, changed or deleted: this is a browser. (No copy/move/delete/rename yet.)
 *
 *   aether-files [DIR]        (default: $HOME if it exists, else /)
 * Mouse: click selects, double-click opens a folder, toolbar buttons (Back and Home are icon buttons with tooltips), the Places sidebar
 * (Home, Desktop, Documents, Downloads, Pictures, Music, Videos, Root Filesystem; a place that cannot be opened is dimmed and says why),
 * wheel scrolls, click the path bar to type a path.
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
#define SIDE_ITEM_H 30
#define SIDE_PITCH 34
#define SIDE_ICON 18
#define TIP_DELAY_MS 450
enum { B_BACK, B_UP, B_HOME, B_HIDDEN, B_TERM, B_COUNT };
/* A NULL label marks an icon button (square, glyph centred); the tooltip names it. Text buttons keep their label and need no tooltip. */
static const char *const kBtn[B_COUNT] = { NULL, "Up", NULL, "Hidden", "Terminal" };
static const IconId kBtnIcon[B_COUNT] = { ICON_BACK, ICON_NONE, ICON_HOME, ICON_NONE, ICON_NONE };
static const char *const kBtnTip[B_COUNT] = { "Back (Alt+Left)", NULL, "Home folder", NULL, NULL };
static const IconId kPlaceIcon[FM_PLACE_COUNT] = { ICON_HOME, ICON_DISPLAY, ICON_DOCUMENT, ICON_DOWNLOAD, ICON_PICTURE, ICON_MUSIC, ICON_VIDEO, ICON_DRIVE };
static FmDir cur; static int sel = -1, scroll, show_hidden, hover_row = -1, hover_btn = -1, press_btn = -1;
static int hover_place = -1, press_place = -1, cur_place = FM_PLACE_HOME, place_ok[FM_PLACE_COUNT]; static char place_err[FM_PLACE_COUNT][96];
static long tip_since; static int tip_shown, last_blink = -1;
static Rect place_r[FM_PLACE_COUNT];
static int editing; static char edit_buf[1024];
static char status[256] = "", hist[32][4096]; static int status_err, nhist;
static char preview[3][96]; static int npreview; static char preview_note[96];
static Rect btn_r[B_COUNT], path_r;
static int widget = -1;

static int list_h(const UiWindow *w) { int h = w->h - TOOL_H - HEAD_H - INFO_H - STATUS_H; return h < ROW_H ? ROW_H : h; }
static int visible_rows(const UiWindow *w) { return list_h(w) / ROW_H; }
/* The status line holds one short sentence. File names and paths can be far longer than it, so they are clipped on purpose (ending in "...",
 * never inside a UTF-8 sequence) BEFORE formatting: the longest template plus two clipped arguments always fits, nothing is silently cut. */
#define STATUS_ARG 96
static void clip(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n < cap) { memcpy(dst, src, n + 1); return; }
    size_t k = cap - 4;                                   /* room for "..." and the terminator */
    while (k > 0 && ((unsigned char)src[k] & 0xC0) == 0x80) k--;
    memcpy(dst, src, k); memcpy(dst + k, "...", 4);
}
static void set_status(int err, const char *fmt, const char *a, const char *b)
{
    char ca[STATUS_ARG], cb[STATUS_ARG]; clip(ca, sizeof ca, a); clip(cb, sizeof cb, b);
    status_err = err; snprintf(status, sizeof status, fmt, ca, cb);
}

/* Re-check which sidebar places can be opened right now and which one the current folder belongs to (called after every successful navigation). */
static void refresh_places(void)
{
    const char *home = getenv("HOME");
    for (int i = 0; i < FM_PLACE_COUNT; i++) place_ok[i] = fm_place_check((FmPlaceId)i, home, place_err[i], sizeof place_err[i]) == 0;
    cur_place = (int)fm_place_for_path(cur.path, home);
}

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
    refresh_places();
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
static void go_home(UiWindow *w) { char h[4096]; fm_home_path(h, sizeof h, getenv("HOME")); navigate(w, h, 1, NULL); }
/* A sidebar click: the place is checked at click time (it may have appeared or vanished since it was drawn); an unavailable one changes nothing but the status line. */
static void go_place(UiWindow *w, int id)
{
    char p[4096], err[96]; const char *home = getenv("HOME"); if (id < 0 || id >= FM_PLACE_COUNT) return;
    const char *label = fm_place_label((FmPlaceId)id);
    if (fm_place_check((FmPlaceId)id, home, err, sizeof err) != 0) {
        set_status(1, "%s is not available: %s", label, err); printf("aether-files: place %s unavailable: %s\n", label, err); refresh_places(); return;
    }
    fm_place_path(p, sizeof p, (FmPlaceId)id, home); navigate(w, p, 1, NULL);
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
    case B_HOME: go_home(w); break;
    case B_HIDDEN: { show_hidden = !show_hidden; char keep[256] = ""; if (sel >= 0 && sel < cur.n) snprintf(keep, sizeof keep, "%s", cur.e[sel].name); char p[4096]; snprintf(p, sizeof p, "%s", cur.path); navigate(w, p, 0, keep[0] ? keep : NULL); break; }
    case B_TERM: open_terminal(); break;
    default: break;
    }
}

/* ---- drawing ------------------------------------------------------------------------------------------------------ */
static int side_w(const UiWindow *w) { return fm_sidebar_width(w->w); }
static void layout_toolbar(UiWindow *w)
{
    const Face *f = font_face(FW_MEDIUM, TY_BODY_PX); int x = TH_S3;
    for (int i = 0; i < B_COUNT; i++) { int bw = kBtn[i] ? font_width(f, kBtn[i]) + 2 * TH_S3 : TH_CTRL_H; btn_r[i] = rect_make(x, (TOOL_H - TH_CTRL_H) / 2, bw, TH_CTRL_H); x += bw + TH_S1; }
    path_r = rect_make(x + TH_S2, (TOOL_H - TH_CTRL_H) / 2, w->w - x - TH_S2 - TH_S3, TH_CTRL_H);
}
/* Sidebar rows from the top; the Root Filesystem sits a little apart. A row that does not fit above the status line is not laid out (empty rect, never hit). */
static void layout_sidebar(const UiWindow *w)
{
    int sw = side_w(w), y = TOOL_H + TH_S2;
    for (int i = 0; i < FM_PLACE_COUNT; i++) {
        if (i == FM_PLACE_ROOT) y += TH_S2;
        place_r[i] = y + SIDE_ITEM_H <= w->h - STATUS_H ? rect_make(TH_S2, y, sw - 2 * TH_S2, SIDE_ITEM_H) : rect_make(0, 0, 0, 0);
        y += SIDE_PITCH;
    }
}
/* One machine-readable line per layout: the headless test clicks exactly where the application says its controls are. */
static void report_layout(UiWindow *w)
{
    layout_toolbar(w); layout_sidebar(w);
    printf("aether-files: layout side=%d back=%d,%d,%d,%d home=%d,%d,%d,%d places=", side_w(w), btn_r[B_BACK].x, btn_r[B_BACK].y, btn_r[B_BACK].w, btn_r[B_BACK].h,
           btn_r[B_HOME].x, btn_r[B_HOME].y, btn_r[B_HOME].w, btn_r[B_HOME].h);
    for (int i = 0; i < FM_PLACE_COUNT; i++) printf("%s%d,%d,%d,%d", i ? ";" : "", place_r[i].x, place_r[i].y, place_r[i].w, place_r[i].h);
    printf("\n");
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
static void draw_sidebar(UiWindow *w, Surface *s)
{
    int sw = side_w(w), top = TOOL_H, bot = w->h - STATUS_H; layout_sidebar(w);
    gfx_fill(s, rect_make(0, top, sw, bot - top), TH_PANEL); gfx_fill(s, rect_make(sw - 1, top, 1, bot - top), TH_BORDER);
    if (place_r[FM_PLACE_ROOT].w > 0) gfx_fill(s, rect_make(TH_S3, place_r[FM_PLACE_ROOT].y - TH_S2 / 2 - 1, sw - 2 * TH_S3 - 1, 1), TH_BORDER);
    for (int i = 0; i < FM_PLACE_COUNT; i++) {
        Rect r = place_r[i]; if (r.w <= 0) continue;
        int ok = place_ok[i], on = i == cur_place, hot = hover_place == i && ok && !on, pr = press_place == i && hot;
        if (on) { gfx_rrect(s, r, TH_R_CTRL - 2, gfx_mix(TH_WINDOW, TH_BLUE, 60)); gfx_rrect(s, rect_make(r.x, r.y + 5, 3, r.h - 10), 1, TH_BLUE); }
        else if (pr) gfx_rrect(s, r, TH_R_CTRL - 2, TH_SURFACE2_PRESS);
        else if (hot) gfx_rrect(s, r, TH_R_CTRL - 2, TH_SURFACE2);
        icon_draw(s, kPlaceIcon[i], r.x + TH_S2, r.y + (r.h - SIDE_ICON) / 2, SIDE_ICON, !ok ? IS_DISABLED : (on ? IS_ACTIVE : (hot ? IS_HOVER : IS_DEFAULT)));
        int tx = r.x + TH_S2 + SIDE_ICON + TH_S2;
        text(s, rect_make(tx, r.y, r.x + r.w - TH_S2 - tx, r.h), fm_place_label((FmPlaceId)i), on ? FW_MEDIUM : FW_REGULAR, TY_BODY_PX, !ok ? TH_MUTED : TH_TEXT, FA_LEFT);
    }
}
/* Tooltip of an icon button: appears after the pointer has rested on it, below the toolbar, and never while the button is held down. */
static void draw_tooltip(UiWindow *w, Surface *s)
{
    if (hover_btn < 0 || press_btn >= 0 || !kBtnTip[hover_btn] || ui_now_ms() - tip_since < TIP_DELAY_MS) return;
    const Face *f = font_face(FW_REGULAR, TY_SMALL_PX); int bw = font_width(f, kBtnTip[hover_btn]) + 2 * TH_S2, bh = 24;
    int x = th_clamp(btn_r[hover_btn].x, TH_S1, th_max(TH_S1, w->w - bw - TH_S1)), y = TOOL_H + TH_S1;
    gfx_rrect(s, rect_make(x, y, bw, bh), 6, TH_BORDER); gfx_rrect(s, rect_make(x + 1, y + 1, bw - 2, bh - 2), 5, TH_SURFACE2);
    font_draw_box(s, rect_make(x, y, bw, bh), kBtnTip[hover_btn], f, TH_TEXT, FA_CENTER); tip_shown = 1;
}
static void draw_all(UiWindow *w, int i, Surface *s)
{
    (void)i; tip_shown = 0; gfx_fill(s, rect_make(0, 0, w->w, w->h), TH_WINDOW);
    layout_toolbar(w);
    int lx = side_w(w), lw = w->w - lx;                                         /* the file list, info panel and column header live right of the sidebar */
    gfx_fill(s, rect_make(0, 0, w->w, TOOL_H), TH_PANEL); gfx_fill(s, rect_make(0, TOOL_H - 1, w->w, 1), TH_BORDER);
    for (int b = 0; b < B_COUNT; b++) {
        int dis = (b == B_BACK && nhist == 0), on = (b == B_HIDDEN && show_hidden), hot = hover_btn == b && !dis, pr = press_btn == b && hot;
        uint32_t fill = on ? gfx_mix(TH_SURFACE2, TH_BLUE, 70) : (pr ? TH_SURFACE2_PRESS : (hot ? TH_SURFACE2_HOVER : TH_SURFACE2));
        gfx_rrect(s, btn_r[b], TH_R_CTRL, hot ? gfx_mix(TH_BORDER, TH_BLUE, 120) : TH_BORDER); gfx_rrect(s, rect_make(btn_r[b].x + 1, btn_r[b].y + 1, btn_r[b].w - 2, btn_r[b].h - 2), TH_R_CTRL - 1, fill);
        if (kBtn[b]) font_draw_box(s, btn_r[b], kBtn[b], font_face(FW_MEDIUM, TY_BODY_PX), dis ? TH_MUTED : TH_TEXT, FA_CENTER);
        else icon_draw(s, kBtnIcon[b], btn_r[b].x + (btn_r[b].w - 20) / 2, btn_r[b].y + (btn_r[b].h - 20) / 2, 20, dis ? IS_DISABLED : (pr ? IS_ACTIVE : (hot ? IS_HOVER : IS_DEFAULT)));
    }
    gfx_rrect(s, path_r, TH_R_CTRL, editing ? TH_BLUE : TH_BORDER); gfx_rrect(s, rect_make(path_r.x + 1, path_r.y + 1, path_r.w - 2, path_r.h - 2), TH_R_CTRL - 1, TH_DESKTOP);
    { char shown[1100]; snprintf(shown, sizeof shown, "%.1000s", editing ? edit_buf : cur.path); Rect tb = rect_make(path_r.x + TH_S3, path_r.y, path_r.w - 2 * TH_S3, path_r.h);
      int tw = font_width(font_face(FW_REGULAR, TY_BODY_PX), shown); text(s, tb, shown, FW_REGULAR, TY_BODY_PX, TH_TEXT, FA_LEFT);
      if (editing && (ui_now_ms() / 530) % 2 == 0 && tw < tb.w) gfx_fill(s, rect_make(tb.x + tw + 1, path_r.y + 8, 1, 16), TH_CYAN); }
    /* column header: Size/Modified are right of the name; a narrow list drops the Modified column rather than squeezing the name away */
    int wide = lw >= 440, sizex = lx + lw - (wide ? 270 : 110), timex = lx + lw - 168, namew = th_max(0, sizex - 8 - (lx + 44));
    gfx_fill(s, rect_make(lx, TOOL_H, lw, HEAD_H), TH_PANEL);
    text(s, rect_make(lx + 44, TOOL_H, namew, HEAD_H), "Name", FW_MEDIUM, TY_SMALL_PX, TH_TEXT2, FA_LEFT);
    text(s, rect_make(sizex, TOOL_H, 90, HEAD_H), "Size", FW_MEDIUM, TY_SMALL_PX, TH_TEXT2, FA_RIGHT);
    if (wide) text(s, rect_make(timex, TOOL_H, 160, HEAD_H), "Modified (UTC)", FW_MEDIUM, TY_SMALL_PX, TH_TEXT2, FA_LEFT);
    gfx_fill(s, rect_make(lx, TOOL_H + HEAD_H - 1, lw, 1), TH_BORDER);
    /* rows */
    int top = TOOL_H + HEAD_H, vr = visible_rows(w);
    for (int r = 0; r < vr; r++) {
        int k = scroll + r; if (k >= cur.n) break; const FmEntry *e = &cur.e[k]; Rect row = rect_make(lx + TH_S1, top + r * ROW_H, lw - 2 * TH_S1, ROW_H - 2);
        if (k == sel) { gfx_rrect(s, row, TH_R_CTRL - 2, gfx_mix(TH_WINDOW, TH_BLUE, 60)); gfx_rrect(s, rect_make(row.x, row.y + 5, 3, row.h - 10), 1, TH_BLUE); }
        else if (k == hover_row) gfx_rrect(s, row, TH_R_CTRL - 2, TH_SURFACE2);
        draw_icon(s, lx + 16, row.y + 4, e);
        text(s, rect_make(lx + 44, row.y, namew, row.h), e->name, k == sel ? FW_MEDIUM : FW_REGULAR, TY_BODY_PX, e->broken ? TH_ERR : TH_TEXT, FA_LEFT);
        char sz[32] = ""; if (!e->is_dir && S_ISREG(e->mode)) fm_format_size(sz, sizeof sz, e->size); else if (e->is_dir) snprintf(sz, sizeof sz, "--");
        text(s, rect_make(sizex, row.y, 90, row.h), sz, FW_REGULAR, TY_SMALL_PX, TH_TEXT2, FA_RIGHT);
        if (wide) { char tm[32]; fm_format_time(tm, sizeof tm, e->mtime); text(s, rect_make(timex, row.y, 160, row.h), e->broken && !e->mtime ? "" : tm, FW_REGULAR, TY_SMALL_PX, TH_TEXT2, FA_LEFT); }
    }
    if (cur.n == 0) text(s, rect_make(lx, top + 20, lw, 24), "This folder is empty", FW_REGULAR, TY_BODY_PX, TH_TEXT2, FA_CENTER);
    if (cur.n > vr) {                                                         /* scroll bar */
        int th = list_h(w); int bh = th_max(24, th * vr / cur.n), by = top + (th - bh) * scroll / th_max(1, cur.n - vr);
        gfx_rrect(s, rect_make(w->w - 7, by, 4, bh), 2, TH_BORDER);
    }
    /* info panel */
    int iy = w->h - INFO_H - STATUS_H; gfx_fill(s, rect_make(lx, iy, lw, INFO_H), TH_PANEL); gfx_fill(s, rect_make(lx, iy, lw, 1), TH_BORDER);
    int ix = lx + TH_S4, iw = lw - 2 * TH_S4;
    if (sel >= 0 && sel < cur.n) {
        const FmEntry *e = &cur.e[sel]; char l[400], sz[32], md[11], tm[32];
        fm_format_size(sz, sizeof sz, e->size); fm_format_mode(md, e->mode); fm_format_time(tm, sizeof tm, e->mtime);
        text(s, rect_make(ix, iy + 8, iw, 20), e->name, FW_MEDIUM, TY_BODY_PX + 1, TH_TEXT, FA_LEFT);
        snprintf(l, sizeof l, "%s%s%s   %s   uid %u gid %u", fm_kind(e), S_ISREG(e->mode) ? "  -  " : "", S_ISREG(e->mode) ? sz : "", md, (unsigned)e->uid, (unsigned)e->gid);
        text(s, rect_make(ix, iy + 30, iw, 16), l, FW_REGULAR, TY_SMALL_PX, TH_TEXT2, FA_LEFT);
        snprintf(l, sizeof l, "Modified %s UTC%s%s", tm, e->is_link ? "   Links to " : "", e->is_link ? e->link : "");
        text(s, rect_make(ix, iy + 47, iw, 16), l, FW_REGULAR, TY_SMALL_PX, TH_TEXT2, FA_LEFT);
        if (npreview) for (int p = 0; p < npreview; p++) gfx_text_clipped(s, rect_make(ix, iy + 66 + p * 14, iw, 14), ix, iy + 66 + p * 14 - 1, preview[p], TH_MUTED);
        else if (preview_note[0]) text(s, rect_make(ix, iy + 66, iw, 16), preview_note, FW_REGULAR, TY_SMALL_PX, TH_MUTED, FA_LEFT);
    } else {
        char l[300]; snprintf(l, sizeof l, "%d item%s in this folder%s", cur.n, cur.n == 1 ? "" : "s", cur.hidden_skipped ? " (hidden files are not shown; press Ctrl+H)" : "");
        text(s, rect_make(ix, iy + 8, iw, 20), cur.path, FW_MEDIUM, TY_BODY_PX + 1, TH_TEXT, FA_LEFT);
        text(s, rect_make(ix, iy + 30, iw, 16), l, FW_REGULAR, TY_SMALL_PX, TH_TEXT2, FA_LEFT);
        text(s, rect_make(ix, iy + 47, iw, 16), "Select a file or folder to see its details.", FW_REGULAR, TY_SMALL_PX, TH_MUTED, FA_LEFT);
    }
    draw_sidebar(w, s);
    gfx_fill(s, rect_make(0, w->h - STATUS_H, w->w, STATUS_H), TH_DESKTOP);
    text(s, rect_make(TH_S4, w->h - STATUS_H, w->w - 2 * TH_S4, STATUS_H), status, FW_REGULAR, TY_SMALL_PX, status_err ? TH_ERR : TH_TEXT2, FA_LEFT);
    draw_tooltip(w, s);
}

/* ---- input -------------------------------------------------------------------------------------------------------- */
static int btn_at(int x, int y) { for (int b = 0; b < B_COUNT; b++) if (rect_contains(btn_r[b], x, y)) return b; return -1; }
static int place_at(int x, int y) { for (int i = 0; i < FM_PLACE_COUNT; i++) if (rect_contains(place_r[i], x, y)) return i; return -1; }
static int row_at(const UiWindow *w, int x, int y)
{
    int top = TOOL_H + HEAD_H; if (x < side_w(w) || y < top || y >= top + visible_rows(w) * ROW_H) return -1;      /* the sidebar is not part of the list */
    int k = scroll + (y - top) / ROW_H; return k < cur.n ? k : -1;
}
static void on_pointer(UiWindow *w, int kind, int x, int y, int button, int clicks)
{
    int changed = 0;
    if (kind == 5) { int old = scroll; scroll = th_clamp(scroll - button * 3, 0, th_max(0, cur.n - visible_rows(w))); if (scroll != old) ui_redraw(w); return; }
    if (kind == 4) { hover_row = hover_btn = hover_place = -1; press_btn = press_place = -1; ui_redraw(w); return; }
    int hb = btn_at(x, y), hr = row_at(w, x, y), hp = place_at(x, y);
    if (hb != hover_btn || hr != hover_row || hp != hover_place) { if (hb != hover_btn) tip_since = ui_now_ms(); hover_btn = hb; hover_row = hr; hover_place = hp; changed = 1; }
    if (kind == 1 && button == 1) {
        if (editing && !rect_contains(path_r, x, y)) editing = 0;
        if (hb >= 0) { press_btn = hb; tip_since = ui_now_ms(); changed = 1; }
        else if (hp >= 0) { press_place = hp; changed = 1; }
        else if (rect_contains(path_r, x, y)) { editing = 1; snprintf(edit_buf, sizeof edit_buf, "%.1000s", cur.path); changed = 1; }
        else if (hr >= 0) { select_row(w, hr); changed = 1; if (clicks >= 2) { open_selected(w); changed = 1; } }
    } else if (kind == 2 && button == 1) {
        if (press_btn >= 0) { int pb = press_btn; press_btn = -1; changed = 1; tip_since = ui_now_ms(); if (pb == hb) toolbar_action(w, pb); }
        if (press_place >= 0) { int pp = press_place; press_place = -1; changed = 1; if (pp == hp) go_place(w, pp); }
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
    layout(w); select_row(w, sel); report_layout(w); ui_redraw(w);
}
/* Redraw only when something visible changes: the path-bar caret flips phase, or a tooltip is due. (Idle windows commit nothing.) */
static void on_tick(UiWindow *w)
{
    int blink = editing ? (int)((ui_now_ms() / 530) % 2) : -1;
    if (editing && blink != last_blink) { last_blink = blink; ui_redraw(w); }
    else if (!editing && last_blink != -1) last_blink = -1;
    else if (hover_btn >= 0 && press_btn < 0 && kBtnTip[hover_btn] && !tip_shown && ui_now_ms() - tip_since >= TIP_DELAY_MS) ui_redraw(w);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0); signal(SIGCHLD, SIG_IGN);
    UiWindow w; char err[160];
    if (ui_connect(&w, err, sizeof err) != 0) { fprintf(stderr, "aether-files: %s\n", err); return 1; }
    int cw = th_min(760, w.screen_w - 2 * TH_S6), ch = th_min(520, w.screen_h - PANEL_HEIGHT - TH_TITLE_H - TH_S6);
    w.flags = WF_RESIZABLE | WF_CENTER;
    if (ui_create(&w, "Files", WK_NORMAL, cw, ch, 0, 0, err, sizeof err) != 0) { fprintf(stderr, "aether-files: %s\n", err); return 1; }
    layout(&w); layout_toolbar(&w); layout_sidebar(&w);
    const char *start = argc > 1 ? argv[1] : getenv("HOME"); if (!start || !*start || access(start, R_OK) != 0) start = "/";
    navigate(&w, start, 0, NULL);
    if (cur.path[0] == 0) navigate(&w, "/", 0, NULL);
    w.on_pointer = on_pointer; w.on_key = on_key; w.on_resize = on_resize; w.on_tick = on_tick; w.tick_ms = 200;
    report_layout(&w);
    printf("aether-files: window ready (%dx%d) in %s\n", w.w, w.h, cur.path);
    ui_run(&w); ui_destroy(&w); fm_free(&cur); return 0;
}
