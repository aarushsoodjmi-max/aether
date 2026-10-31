#define _GNU_SOURCE
#include "wm.h"
#include "font.h"
#include "icons.h"
#include "input.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P_NONE WM_HIT_NONE
#define P_CLIENT WM_HIT_CLIENT
#define P_TITLE WM_HIT_TITLE
#define P_EDGE WM_HIT_EDGE       /* resize band; kept below P_CLOSE so "part >= P_CLOSE" still means "title-bar control" */
#define P_CLOSE WM_HIT_CLOSE
#define P_MAX WM_HIT_MAX
#define P_MIN WM_HIT_MIN
typedef struct { Win *w; int part, edges; } Hit;

void wm_init(Wm *w, int sw, int sh, WmEmit emit, void *ctx)
{
    memset(w, 0, sizeof *w); w->sw = sw; w->sh = sh; w->nws = AETHER_WORKSPACES; w->next_id = 1;
    w->ptr_x = sw / 2; w->ptr_y = sh / 2; w->emit = emit; w->ctx = ctx; w->dirty = 1; w->cursor_dirty = 1;
}
Win *wm_find(Wm *w, uint32_t id) { for (int i = 0; i < WM_MAX_WINDOWS; i++) if (w->wins[i].used && w->wins[i].id == id) return &w->wins[i]; return NULL; }
static int idx_of(Wm *w, const Win *x) { return (int)(x - w->wins); }

static void send(Wm *w, const Win *x, uint32_t type, int a, int b, int c, int d)
{
    if (!w->emit) return;
    Msg m; memset(&m, 0, sizeof m); m.type = type; m.window = x ? x->id : 0; m.v[0] = a; m.v[1] = b; m.v[2] = c; m.v[3] = d; w->emit(w->ctx, x ? x->client : -1, &m);
}
static int visible(const Wm *w, const Win *x) { return x->used && (x->kind != WK_NORMAL || (x->ws == w->ws && !x->minimized)); }
static Rect client_rect(const Win *x) { return rect_make(x->x, x->y, x->w, x->h); }
static Rect title_rect(const Win *x) { return rect_make(x->x, x->y - WM_TITLE_H, x->w, WM_TITLE_H); }
static Rect frame_rect(const Win *x) { return rect_make(x->x, x->y - WM_TITLE_H, x->w, x->h + WM_TITLE_H); }       /* title bar + client: what draw_normal paints */
/* Is pixel (px,py) inside r with its corners rounded by rad? Same pixel-centre test the rasteriser uses for "at least half covered",
 * so the hit shape follows what is painted: a corner pixel that shows the desktop belongs to the desktop. */
static int rr_contains(Rect r, int rad, int px, int py)
{
    if (!rect_contains(r, px, py)) return 0;
    int m = (r.w < r.h ? r.w : r.h) / 2; if (rad > m) rad = m;
    if (rad <= 0) return 1;
    int lx = px - r.x, ly = py - r.y, i = lx < r.w - 1 - lx ? lx : r.w - 1 - lx, j = ly < r.h - 1 - ly ? ly : r.h - 1 - ly;
    if (i >= rad || j >= rad) return 1;
    int a = 2 * rad - 2 * i - 1, b = 2 * rad - 2 * j - 1; return a * a + b * b <= 4 * rad * rad;
}
/* Which resize edges does (px,py) select on frame b? Corners first (they win where the bands overlap). 0 = interior. */
static int edge_mask(Rect b, int px, int py)
{
    int r = b.x + b.w, bt = b.y + b.h;
    int l = px < b.x + WM_RESIZE_BAND, rt = px >= r - WM_RESIZE_BAND, t = py < b.y + WM_RESIZE_BAND, bm = py >= bt - WM_RESIZE_BAND;
    int cl = px < b.x + WM_RESIZE_CORNER, cr = px >= r - WM_RESIZE_CORNER, ct = py < b.y + WM_RESIZE_CORNER, cb = py >= bt - WM_RESIZE_CORNER;
    if (l && ct) return WM_EDGE_L | WM_EDGE_T;
    if (l && cb) return WM_EDGE_L | WM_EDGE_B;
    if (rt && ct) return WM_EDGE_R | WM_EDGE_T;
    if (rt && cb) return WM_EDGE_R | WM_EDGE_B;
    if (t && cl) return WM_EDGE_L | WM_EDGE_T;
    if (t && cr) return WM_EDGE_R | WM_EDGE_T;
    if (bm && cl) return WM_EDGE_L | WM_EDGE_B;
    if (bm && cr) return WM_EDGE_R | WM_EDGE_B;
    return (l ? WM_EDGE_L : 0) | (rt ? WM_EDGE_R : 0) | (t ? WM_EDGE_T : 0) | (bm ? WM_EDGE_B : 0);
}
static int has_max(const Win *x) { return (x->flags & WF_RESIZABLE) != 0; }
/* title-bar controls, right to left: close, maximise (resizable windows only), minimise */
static Rect btn_rect(const Win *x, int part)
{
    int slot = part == P_CLOSE ? 0 : (part == P_MAX ? 1 : (has_max(x) ? 2 : 1));
    return rect_make(x->x + x->w - TH_S2 - TH_TITLE_BTN - slot * (TH_TITLE_BTN + TH_S1), x->y - WM_TITLE_H + (WM_TITLE_H - TH_TITLE_BTN) / 2, TH_TITLE_BTN, TH_TITLE_BTN);
}
static int controls_w(const Win *x) { return TH_S2 + (has_max(x) ? 3 : 2) * (TH_TITLE_BTN + TH_S1); }

static void cancel_interaction(Wm *w, uint32_t id);
static void z_remove(Wm *w, int idx) { int o = 0; for (int i = 0; i < w->nz; i++) if (w->z[i] != idx) w->z[o++] = w->z[i]; w->nz = o; }
static void z_raise(Wm *w, int idx) { z_remove(w, idx); if (w->nz < WM_MAX_WINDOWS) w->z[w->nz++] = idx; }

/* Tell every panel which normal windows exist on this workspace, which is focused, which minimised. */
static void tasks(Wm *w)
{
    if (!w->emit) return;
    for (int p = 0; p < WM_MAX_WINDOWS; p++) {
        Win *pw = &w->wins[p]; if (!pw->used || pw->kind != WK_PANEL) continue;
        Msg m; memset(&m, 0, sizeof m); m.type = E_TASK; m.v[0] = 0; w->emit(w->ctx, pw->client, &m);
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            Win *x = &w->wins[i]; if (!x->used || x->kind != WK_NORMAL || x->ws != w->ws) continue;
            memset(&m, 0, sizeof m); m.type = E_TASK; m.window = x->id; m.v[0] = 1; m.v[1] = (w->focus == (int)x->id ? 1 : 0) | (x->minimized ? 2 : 0);
            snprintf(m.text, sizeof m.text, "%s", x->title); w->emit(w->ctx, pw->client, &m);
        }
        memset(&m, 0, sizeof m); m.type = E_TASK; m.v[0] = 2; w->emit(w->ctx, pw->client, &m);
    }
}
static void set_focus(Wm *w, Win *x)
{
    uint32_t id = x ? x->id : 0; if (w->focus == (int)id) return; w->focus = (int)id; w->dirty = 1; tasks(w);
}
static void focus_top(Wm *w) { for (int i = w->nz - 1; i >= 0; i--) { Win *x = &w->wins[w->z[i]]; if (visible(w, x)) { set_focus(w, x); return; } } set_focus(w, NULL); }

/* Notifications stack top-down from under the panel using their real heights; closing one reflows the rest. */
static void restack_notify(Wm *w)
{
    int y = PANEL_HEIGHT + TH_S3; uint32_t last = 0;
    for (;;) {
        Win *n = NULL;
        for (int i = 0; i < WM_MAX_WINDOWS; i++) { Win *c = &w->wins[i]; if (c->used && c->kind == WK_NOTIFY && c->id > last && (!n || c->id < n->id)) n = c; }
        if (!n) break;
        last = n->id; n->x = w->sw - n->w - TH_S4;
        if (n->y != y) { n->y = y; send(w, n, E_CONFIGURE, n->x, n->y, n->w, n->h); }
        y += n->h + TH_S2;
    }
    w->dirty = 1;
}
static void place_center(Wm *w, Win *x, int nnormal)
{
    int top = PANEL_HEIGHT + WM_TITLE_H, ah = w->sh - top, off = (nnormal % 8) * 28;
    x->x = (w->sw - x->w) / 2 + off; x->y = top + (ah - x->h) / 2 + off;
}
static void clamp_window(Wm *w, Win *x)
{
    if (x->kind != WK_NORMAL) return;
    if (x->x + x->w > w->sw) x->x = w->sw - x->w;
    if (x->x < 0) x->x = 0;
    if (x->y + x->h > w->sh) x->y = w->sh - x->h;
    if (x->y < PANEL_HEIGHT + WM_TITLE_H) x->y = PANEL_HEIGHT + WM_TITLE_H;
}

uint32_t wm_add_window(Wm *w, int client, int kind, int cw, int ch, int px, int py, const char *title, uint32_t *buf, size_t bufsize)
{
    if (kind < WK_NORMAL || kind > WK_DESKTOP || cw < 1 || ch < 1 || cw > AETHER_MAX_WIN_W || ch > AETHER_MAX_WIN_H) return 0;
    if (bufsize < (size_t)cw * (size_t)ch * 4) return 0;
    int slot = -1; for (int i = 0; i < WM_MAX_WINDOWS; i++) if (!w->wins[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    Win *x = &w->wins[slot]; memset(x, 0, sizeof *x);
    x->used = 1; x->id = w->next_id++; x->client = client; x->kind = kind; x->w = cw; x->h = ch; x->ws = w->ws; x->buf = buf; x->bufsize = bufsize; x->alpha = 255;
    snprintf(x->title, sizeof x->title, "%s", title ? title : "");
    int nnormal = 0, nnotify = 0; for (int i = 0; i < WM_MAX_WINDOWS; i++) if (w->wins[i].used && i != slot) { if (w->wins[i].kind == WK_NORMAL) nnormal++; if (w->wins[i].kind == WK_NOTIFY) nnotify++; }
    switch (kind) {
    case WK_PANEL: x->x = 0; x->y = 0; break;
    case WK_DESKTOP: x->x = 0; x->y = PANEL_HEIGHT; break;
    case WK_NOTIFY: x->x = w->sw - cw - TH_S4; x->y = PANEL_HEIGHT + TH_S3; (void)nnotify; break;
    case WK_POPUP: x->x = px; x->y = py; break;
    default: x->x = 40 + (nnormal % 8) * 28; x->y = PANEL_HEIGHT + WM_TITLE_H + 16 + (nnormal % 8) * 28; break;
    }
    if (kind != WK_DESKTOP) {
        if (x->x + x->w > w->sw) x->x = w->sw - x->w;
        if (x->x < 0) x->x = 0;
        if (x->y + x->h > w->sh) x->y = w->sh - x->h;
        if (x->y < (kind == WK_NORMAL ? PANEL_HEIGHT + WM_TITLE_H : 0)) x->y = kind == WK_NORMAL ? PANEL_HEIGHT + WM_TITLE_H : 0;
    }
    if (kind == WK_NOTIFY) { x->y = 1 << 20; restack_notify(w); }                       /* appended below the existing ones */
    if (kind == WK_NORMAL) { z_raise(w, slot); set_focus(w, x); tasks(w); }
    send(w, x, E_CONFIGURE, x->x, x->y, x->w, x->h); w->dirty = 1; return x->id;
}
void wm_set_flags(Wm *w, uint32_t id, int flags)
{
    Win *x = wm_find(w, id); if (!x) return; x->flags = flags;
    if (flags & WF_TRANSPARENT) x->alpha = 0;
    if (x->kind == WK_NORMAL && (flags & WF_CENTER)) {
        int n = 0; for (int i = 0; i < WM_MAX_WINDOWS; i++) if (w->wins[i].used && &w->wins[i] != x && w->wins[i].kind == WK_NORMAL) n++;
        place_center(w, x, n); clamp_window(w, x); send(w, x, E_CONFIGURE, x->x, x->y, x->w, x->h);
    }
    w->dirty = 1;
}
void wm_set_opacity(Wm *w, uint32_t id, int alpha) { Win *x = wm_find(w, id); if (!x) return; x->alpha = th_clamp(alpha, 0, 255); w->dirty = 1; }

void wm_remove_window(Wm *w, uint32_t id)
{
    Win *x = wm_find(w, id); if (!x) return; int idx = idx_of(w, x), normal = x->kind == WK_NORMAL, kind_notify = x->kind == WK_NOTIFY;
    cancel_interaction(w, id);
    if (w->hover == (int)id) w->hover = 0;
    z_remove(w, idx); int wasfocus = w->focus == (int)id; memset(x, 0, sizeof *x); if (wasfocus) { w->focus = 0; focus_top(w); } w->dirty = 1;
    if (normal) tasks(w);
    if (kind_notify) restack_notify(w);
}
int wm_remove_client(Wm *w, int client, void (*unmap)(uint32_t *, size_t))
{
    int n = 0; for (int i = 0; i < WM_MAX_WINDOWS; i++) if (w->wins[i].used && w->wins[i].client == client) { if (unmap) unmap(w->wins[i].buf, w->wins[i].bufsize); wm_remove_window(w, w->wins[i].id); n++; }
    return n;
}

/* ---- minimise / maximise ---- */
static Win *update_hover(Wm *w);
/* A window is being hidden or removed: nothing the pointer was doing to it (drag, resize, button press, grab) may continue invisibly. */
static void cancel_interaction(Wm *w, uint32_t id)
{
    if (w->drag == (int)id) w->drag = 0;
    if (w->rsz == (int)id) w->rsz = 0;
    if (w->grab == (int)id) w->grab = 0;
    if (w->press_win == (int)id) { w->press_win = 0; w->press_btn = 0; }
    if (w->hot_win == (int)id) { w->hot_win = 0; w->hot_btn = 0; }
}
static void minimize(Wm *w, Win *x) { x->minimized = 1; cancel_interaction(w, x->id); if (w->focus == (int)x->id) { w->focus = 0; focus_top(w); } w->dirty = 1; tasks(w); update_hover(w); }
/* A request the client never answered expires, so a hung client cannot make the window permanently un-resizable / un-maximisable. */
static int pending_busy(Wm *w, Win *x)
{
    if (!x->pending) return 0;
    if (w->clock_on && w->now_ms - x->pend_ms > WM_RESIZE_TIMEOUT_MS) { x->pending = 0; return 0; }
    return 1;
}
static void request_resize(Wm *w, Win *x, int pending, int tx, int ty, int tw, int th)
{
    x->pending = pending; x->pend_x = tx; x->pend_y = ty; x->pend_w = tw; x->pend_h = th; x->pend_ms = w->now_ms;
    if (!w->emit) return;
    Msg m; memset(&m, 0, sizeof m); m.type = E_CONFIGURE; m.window = x->id; m.v[0] = tx; m.v[1] = ty; m.v[2] = tw; m.v[3] = th; m.v[4] = 1;   /* v4 = resize request */
    w->emit(w->ctx, x->client, &m);
}
static void toggle_maximize(Wm *w, Win *x)
{
    if (!has_max(x) || pending_busy(w, x) || w->rsz == (int)x->id) return;
    int top = PANEL_HEIGHT + WM_TITLE_H;
    if (x->maximized) request_resize(w, x, 2, x->restore.x, x->restore.y, x->restore.w, x->restore.h);
    else request_resize(w, x, 1, 0, top, w->sw, w->sh - top);
}
/* ---- interactive resize ----
 * The pointer decides a wanted client rectangle (resize_update); the client owns its pixels, so the compositor asks it for a buffer of
 * that size (E_CONFIGURE v4=1) and the window changes shape only when the new buffer arrives (wm_replace_buffer). One request is in
 * flight at a time; when its reply lands, the newest wanted size is requested next. The edges opposite the dragged ones stay fixed. */
static void resize_pump(Wm *w, Win *x)
{
    if (pending_busy(w, x)) return;
    int nx = (x->rs_edges & WM_EDGE_L) ? x->rs_right - x->rs_want.w : x->rs_want.x, ny = (x->rs_edges & WM_EDGE_T) ? x->rs_bottom - x->rs_want.h : x->rs_want.y;
    if (x->rs_want.w == x->w && x->rs_want.h == x->h && nx == x->x && ny == x->y) { x->rs_follow = 0; return; }
    x->rs_follow = 1; request_resize(w, x, 3, nx, ny, x->rs_want.w, x->rs_want.h);
}
static void resize_begin(Wm *w, Win *x, int edges)
{
    if (x->pending == 1 || x->pending == 2) return;                                   /* a maximise/restore is in flight: not now */
    Rect o = x->pending == 3 ? rect_make(x->pend_x, x->pend_y, x->pend_w, x->pend_h) : rect_make(x->x, x->y, x->w, x->h);   /* continue from where the previous resize is heading */
    w->rsz = (int)x->id; w->rsz_edges = edges; w->rsz_px = w->ptr_x; w->rsz_py = w->ptr_y; w->rsz_orig = o;
    x->rs_edges = edges; x->rs_right = o.x + o.w; x->rs_bottom = o.y + o.h; x->rs_want = o;
    w->hot_win = w->hot_btn = 0; w->dirty = 1;
}
static void resize_update(Wm *w, Win *x)
{
    Rect o = w->rsz_orig; int e = w->rsz_edges, dx = w->ptr_x - w->rsz_px, dy = w->ptr_y - w->rsz_py;
    int l = o.x, r = o.x + o.w, t = o.y, b = o.y + o.h;
    int lo_x = th_min(0, o.x), hi_x = th_max(w->sw, o.x + o.w), lo_y = th_min(PANEL_HEIGHT + WM_TITLE_H, o.y), hi_y = th_max(w->sh, o.y + o.h);   /* stay on screen, but never pull in a window that already sticks out */
    int minw = th_min(WM_MIN_W, o.w), minh = th_min(WM_MIN_H, o.h);
    if (e & WM_EDGE_L) l = th_clamp(l + dx, lo_x, r - minw);
    if (e & WM_EDGE_R) r = th_clamp(r + dx, l + minw, hi_x);
    if (e & WM_EDGE_T) t = th_clamp(t + dy, lo_y, b - minh);
    if (e & WM_EDGE_B) b = th_clamp(b + dy, t + minh, hi_y);
    x->rs_want = rect_make(l, t, r - l, b - t); resize_pump(w, x);
}
static void resize_end(Wm *w)
{
    Win *x = wm_find(w, (uint32_t)w->rsz); w->rsz = 0;
    if (x) { x->rs_follow = 1; resize_pump(w, x); }                                   /* make sure the final pointer position is what the window ends up with */
    w->dirty = 1; update_hover(w);
}
int wm_replace_buffer(Wm *w, uint32_t id, int nw, int nh, uint32_t *buf, size_t size, void (*unmap)(uint32_t *, size_t))
{
    Win *x = wm_find(w, id); if (!x || nw < 1 || nh < 1 || nw > AETHER_MAX_WIN_W || nh > AETHER_MAX_WIN_H || size < (size_t)nw * (size_t)nh * 4) return -1;
    if (unmap) unmap(x->buf, x->bufsize);
    x->buf = buf; x->bufsize = size;
    if (x->pending == 3) {                                                           /* answer to an interactive resize request */
        int honoured = nw == x->pend_w && nh == x->pend_h;
        x->w = nw; x->h = nh; x->pending = 0;
        x->x = (x->rs_edges & WM_EDGE_L) ? x->rs_right - nw : x->pend_x;            /* the edges opposite the dragged ones do not move, whatever size the client chose */
        x->y = (x->rs_edges & WM_EDGE_T) ? x->rs_bottom - nh : x->pend_y;
        if (!honoured) x->rs_want = rect_make(x->x, x->y, nw, nh);                  /* the client refused that size: do not ask for it again in a loop */
        w->dirty = 1; if (x->rs_follow) resize_pump(w, x);
        if (!w->rsz && !w->drag) update_hover(w);
        return 0;
    }
    if (x->pending && nw == x->pend_w && nh == x->pend_h) {
        if (x->pending == 1) { if (!x->maximized) x->restore = rect_make(x->x, x->y, x->w, x->h); x->maximized = 1; } else x->maximized = 0;
        x->x = x->pend_x; x->y = x->pend_y;
    }
    x->pending = 0; x->w = nw; x->h = nh;
    if (x->kind == WK_NORMAL && !x->maximized) clamp_window(w, x);
    w->dirty = 1; if (!w->rsz && !w->drag) update_hover(w);                          /* the window changed shape under a stationary pointer */
    return 0;
}
void wm_task_activate(Wm *w, uint32_t id)
{
    Win *x = wm_find(w, id); if (!x || x->kind != WK_NORMAL) return;
    if (x->minimized) { x->minimized = 0; z_raise(w, idx_of(w, x)); w->focus = 0; set_focus(w, x); w->dirty = 1; tasks(w); }
    else if (w->focus == (int)id && x->ws == w->ws) minimize(w, x);
    else { z_raise(w, idx_of(w, x)); set_focus(w, x); w->dirty = 1; }
}
void wm_resize_screen(Wm *w, int sw, int sh)
{
    if (sw < 1 || sh < 1) return;
    w->sw = sw; w->sh = sh; w->ptr_x = th_clamp(w->ptr_x, 0, sw - 1); w->ptr_y = th_clamp(w->ptr_y, 0, sh - 1); w->rsz = 0;
    int told[WM_MAX_WINDOWS], nt = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        Win *x = &w->wins[i]; if (!x->used) continue;
        if (x->kind == WK_NORMAL) { if (x->maximized) request_resize(w, x, 1, 0, PANEL_HEIGHT + WM_TITLE_H, sw, sh - PANEL_HEIGHT - WM_TITLE_H); else clamp_window(w, x); }
        int seen = 0; for (int k = 0; k < nt; k++) if (told[k] == x->client) seen = 1;
        if (!seen && nt < WM_MAX_WINDOWS && w->emit) { told[nt++] = x->client; Msg m; memset(&m, 0, sizeof m); m.type = E_SCREEN; m.v[0] = sw; m.v[1] = sh; m.v[2] = w->ws; m.v[3] = w->nws; w->emit(w->ctx, x->client, &m); }
    }
    restack_notify(w); w->dirty = 1; w->cursor_dirty = 1;
}

/* ---- hit testing ---- */
static Hit hit(Wm *w, int px, int py)
{
    Hit h = { NULL, P_NONE, 0 };
    static const int order[3] = { WK_POPUP, WK_NOTIFY, WK_PANEL };
    for (int k = 0; k < 3; k++) for (int i = WM_MAX_WINDOWS - 1; i >= 0; i--) {
        Win *x = &w->wins[i]; if (!x->used || x->kind != order[k]) continue;
        if (x->kind != WK_PANEL && x->alpha <= 0) continue;                         /* a fully transparent popup/notification is not painted (see wm_compose_scene), so it must not catch the pointer either */
        if (rr_contains(client_rect(x), x->kind == WK_PANEL ? 0 : TH_R_MENU, px, py)) { h.w = x; h.part = P_CLIENT; return h; }
    }
    for (int i = w->nz - 1; i >= 0; i--) {
        Win *x = &w->wins[w->z[i]]; if (!visible(w, x)) continue;
        int rad = x->maximized ? 0 : TH_R_WIN, grips = has_max(x) && !x->maximized;
        Rect body = frame_rect(x), outer = rect_make(body.x - 1, body.y - 1, body.w + 2, body.h + 2);
        int in_body = rr_contains(body, rad, px, py), in_outline = grips && rr_contains(outer, rad + 1, px, py);   /* the 1 px border is painted, so it may carry the resize grip */
        if (!in_body && !in_outline) continue;                                      /* not over this window: whatever is beneath it gets the pointer */
        if (in_body) {
            static const int parts[3] = { P_CLOSE, P_MAX, P_MIN };
            for (int k = 0; k < 3; k++) { if (parts[k] == P_MAX && !has_max(x)) continue; if (rect_contains(btn_rect(x, parts[k]), px, py)) { h.w = x; h.part = parts[k]; return h; } }
        }
        if (grips) { int e = edge_mask(body, px, py); if (e) { h.w = x; h.part = P_EDGE; h.edges = e; return h; } }
        if (!in_body) continue;
        if (rect_contains(title_rect(x), px, py)) { h.w = x; h.part = P_TITLE; return h; }
        if (rect_contains(client_rect(x), px, py)) { h.w = x; h.part = P_CLIENT; return h; }
    }
    for (int i = 0; i < WM_MAX_WINDOWS; i++) { Win *x = &w->wins[i]; if (x->used && x->kind == WK_DESKTOP && rect_contains(client_rect(x), px, py)) { h.w = x; h.part = P_CLIENT; return h; } }
    return h;
}
int wm_hit_test(Wm *w, int px, int py, uint32_t *win, int *edges)
{
    Hit h = hit(w, px, py); if (win) *win = h.w ? h.w->id : 0; if (edges) *edges = h.edges; return h.part;
}
static void pointer_event(Wm *w, Win *x, int kind, int btn, int extra)
{
    if (!w->emit) return;
    Msg m; memset(&m, 0, sizeof m); m.type = E_POINTER; m.window = x->id; m.v[0] = kind; m.v[1] = w->ptr_x - x->x; m.v[2] = w->ptr_y - x->y; m.v[3] = btn; m.v[4] = extra; w->emit(w->ctx, x->client, &m);
}

/* Recompute which control is hot and which window the pointer is over (sending enter/leave) without moving the pointer.
 * Returns the window that should receive motion (the grab owner, or the client area under the pointer). */
static Win *update_hover(Wm *w)
{
    Hit h = hit(w, w->ptr_x, w->ptr_y);
    int hw = h.w && h.part >= P_CLOSE ? (int)h.w->id : 0, hb = hw ? h.part : 0;
    if (hw != w->hot_win || hb != w->hot_btn) { w->hot_win = hw; w->hot_btn = hb; w->dirty = 1; }
    Win *under = w->grab ? wm_find(w, (uint32_t)w->grab) : (h.part == P_CLIENT ? h.w : NULL);     /* title bar belongs to the compositor */
    uint32_t uid = under ? under->id : 0;
    if ((int)uid != w->hover) {
        Win *old = wm_find(w, (uint32_t)w->hover); if (old) pointer_event(w, old, 4, 0, 0);
        w->hover = (int)uid; if (under) pointer_event(w, under, 3, 0, 0);
    }
    return under;
}
/* The one place the pointer position changes. Relative devices call it with position+delta, absolute devices with the
 * already-scaled display pixel; the position is clamped to the display so it can never leave it or get stuck beyond an edge. */
void wm_pointer_to(Wm *w, int x, int y)
{
    w->ptr_x = th_clamp(x, 0, w->sw - 1); w->ptr_y = th_clamp(y, 0, w->sh - 1);
    w->cursor_dirty = 1;
    if (w->drag) {
        Win *x2 = wm_find(w, (uint32_t)w->drag);
        if (x2) {
            x2->x = th_clamp(w->ptr_x - w->drag_dx, 80 - x2->w, w->sw - 80);
            x2->y = th_clamp(w->ptr_y - w->drag_dy, PANEL_HEIGHT + WM_TITLE_H, th_max(PANEL_HEIGHT + WM_TITLE_H, w->sh - WM_TITLE_H));
            send(w, x2, E_CONFIGURE, x2->x, x2->y, x2->w, x2->h);
        }
        w->dirty = 1; return;
    }
    if (w->rsz) {                                                                    /* edge resize in progress: the compositor owns the pointer, clients see nothing */
        Win *rw = wm_find(w, (uint32_t)w->rsz);
        if (rw) resize_update(w, rw); else w->rsz = 0;
        return;
    }
    Win *under = update_hover(w);
    if (under) pointer_event(w, under, 0, 0, 0);
}
void wm_pointer_move(Wm *w, int dx, int dy)
{
    long nx = (long)w->ptr_x + dx, ny = (long)w->ptr_y + dy;          /* long: a huge delta must clamp, not overflow */
    wm_pointer_to(w, nx < -100000 ? -100000 : (nx > 100000 ? 100000 : (int)nx), ny < -100000 ? -100000 : (ny > 100000 ? 100000 : (int)ny));
}
void wm_set_time(Wm *w, long now_ms) { w->now_ms = now_ms; w->clock_on = 1; }
static int click_count(Wm *w, const Win *x)
{
    if (!w->clock_on) return 1;
    int n = 1;
    if (w->click_win == (int)x->id && w->now_ms - w->click_ms <= 400 && w->click_n < 3 && abs(w->ptr_x - w->click_x) <= 4 && abs(w->ptr_y - w->click_y) <= 4) n = w->click_n + 1;
    w->click_ms = w->now_ms; w->click_x = w->ptr_x; w->click_y = w->ptr_y; w->click_win = (int)x->id; w->click_n = n; return n;
}
void wm_wheel(Wm *w, int notches)
{
    if (!notches) return;
    Win *x = w->grab ? wm_find(w, (uint32_t)w->grab) : NULL;
    if (!x) { Hit h = hit(w, w->ptr_x, w->ptr_y); if (h.w && h.part == P_CLIENT) x = h.w; }
    if (x) pointer_event(w, x, 5, notches, 0);
}
void wm_button(Wm *w, int down) { wm_button_ex(w, 1, down); }
void wm_button_ex(Wm *w, int button, int down)
{
    if (button < 1 || button > 3) return;
    int bit = 1 << (button - 1);
    if (!down) {
        if (!(w->btn_mask & bit)) return;                                     /* release without a matching press (e.g. after a workspace switch) */
        w->btn_mask &= ~bit;
        if (button == 1) {
            if (w->press_btn) {
                Hit h = hit(w, w->ptr_x, w->ptr_y); Win *x = wm_find(w, (uint32_t)w->press_win); int part = w->press_btn;
                w->press_win = 0; w->press_btn = 0; w->dirty = 1;
                if (x && h.w == x && h.part == part) { if (part == P_CLOSE) send(w, x, E_CLOSE, 0, 0, 0, 0); else if (part == P_MIN) minimize(w, x); else if (part == P_MAX) toggle_maximize(w, x); }
                return;
            }
            if (w->drag) { w->drag = 0; w->dirty = 1; update_hover(w); return; }
            if (w->rsz) { resize_end(w); return; }
        }
        Win *g = wm_find(w, (uint32_t)w->grab); if (g) pointer_event(w, g, 2, button, 0);
        if (!w->btn_mask) w->grab = 0;
        return;
    }
    if (w->btn_mask & bit) return;                                            /* duplicate press */
    w->btn_mask |= bit;
    if (w->grab) {                                                            /* another button while one is held: same window, no re-hit-testing */
        Win *g = wm_find(w, (uint32_t)w->grab); if (g) pointer_event(w, g, 1, button, 0);
        return;
    }
    Hit h = hit(w, w->ptr_x, w->ptr_y); Win *x = h.w; int dismissed = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) { Win *p = &w->wins[i]; if (p->used && p->kind == WK_POPUP && p != x) { send(w, p, E_CLOSE, 0, 0, 0, 0); dismissed++; } }   /* click outside dismisses popups */
    if (!x) { w->btn_mask &= ~bit; return; }
    if (button == 1 && x->kind == WK_NORMAL) { z_raise(w, idx_of(w, x)); set_focus(w, x); w->dirty = 1; }
    else if (x->kind == WK_NORMAL && h.part == P_CLIENT) { z_raise(w, idx_of(w, x)); set_focus(w, x); w->dirty = 1; }   /* right/middle click on a client area focuses too */
    if (h.part == P_EDGE) {                                                  /* resize grip: compositor-owned, primary button only */
        if (button != 1) { w->btn_mask &= ~bit; return; }
        resize_begin(w, x, h.edges); if (!w->rsz) w->btn_mask &= ~bit; return;
    }
    if (h.part >= P_CLOSE || h.part == P_TITLE) {                            /* the frame belongs to the compositor and answers only to the primary button */
        if (button != 1) { w->btn_mask &= ~bit; return; }
        int n = click_count(w, x);
        if (h.part >= P_CLOSE) { w->press_win = (int)x->id; w->press_btn = h.part; w->dirty = 1; return; }
        if (n == 2 && has_max(x)) { toggle_maximize(w, x); w->click_n = 3; return; }       /* double-click the title bar: maximise / restore */
        if (!x->maximized) { w->drag = (int)x->id; w->drag_dx = w->ptr_x - x->x; w->drag_dy = w->ptr_y - x->y; }
        return;
    }
    w->grab = (int)x->id;
    { Msg m; memset(&m, 0, sizeof m); m.type = E_POINTER; m.window = x->id; m.v[0] = 1; m.v[1] = w->ptr_x - x->x; m.v[2] = w->ptr_y - x->y; m.v[3] = button; m.v[4] = dismissed; m.v[5] = button == 1 ? click_count(w, x) : 1; if (w->emit) w->emit(w->ctx, x->client, &m); }
}
void wm_switch_workspace(Wm *w, int index)
{
    if (index < 0 || index >= w->nws || index == w->ws) return;
    Win *oldh = wm_find(w, (uint32_t)w->hover); if (oldh) pointer_event(w, oldh, 4, 0, 0);       /* the window under the pointer is about to vanish: tell it, so no widget stays "pressed" */
    w->ws = index; w->drag = w->grab = w->hover = w->rsz = 0; w->btn_mask = 0; w->press_btn = w->press_win = 0; w->hot_win = w->hot_btn = 0; focus_top(w); w->dirty = 1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (w->wins[i].used && w->wins[i].kind == WK_PANEL) send(w, &w->wins[i], E_WORKSPACE, index, w->nws, 0, 0);
    tasks(w); update_hover(w);                                                                   /* whatever is now under the pointer gets its enter event */
}
static void hotkey(Wm *w, int id) { for (int i = 0; i < WM_MAX_WINDOWS; i++) if (w->wins[i].used && w->wins[i].kind == WK_PANEL) send(w, &w->wins[i], E_HOTKEY, id, 0, 0, 0); }
/* Alt+Tab: bring the bottom-most visible window of this workspace to the front (repeat to cycle); Shift reverses. */
static void cycle_windows(Wm *w, int reverse)
{
    int vis[WM_MAX_WINDOWS], nv = 0; for (int i = 0; i < w->nz; i++) if (visible(w, &w->wins[w->z[i]])) vis[nv++] = w->z[i];
    if (nv < 2) return;
    if (!reverse) z_raise(w, vis[0]);
    else { int top = vis[nv - 1]; z_remove(w, top); for (int i = w->nz; i > 0; i--) w->z[i] = w->z[i - 1]; w->z[0] = top; w->nz++; }
    focus_top(w); w->dirty = 1; tasks(w);
}
static int key_ascii(const Wm *w, int keycode)
{
    int shift = (w->mods & KMOD_SHIFT) != 0, a = keycode_to_ascii(keycode, 0);
    if (w->caps && a >= 'a' && a <= 'z') shift = !shift;                              /* Caps Lock affects letters only */
    return keycode_to_ascii(keycode, shift);
}
void wm_key(Wm *w, int keycode, int value)
{
    if (keycode == KEY_LEFTMETA || keycode == KEY_RIGHTMETA) {                       /* Super tapped on its own = launcher */
        if (value == 1) { w->super_down = 1; w->super_used = 0; w->mods |= KMOD_SUPER; }
        else if (value == 0) { w->mods &= ~KMOD_SUPER; if (w->super_down && !w->super_used) hotkey(w, 1); w->super_down = 0; }
        return;
    }
    if (keycode == KEY_CAPSLOCK) { if (value == 1) w->caps = !w->caps; return; }
    int mod; if (keycode_is_modifier(keycode, &mod)) { if (value) w->mods |= mod; else w->mods &= ~mod; return; }
    if (w->super_down && value == 1) w->super_used = 1;
    if ((w->mods & KMOD_ALT) && value == 1) {
        if (keycode >= KEY_1 && keycode < KEY_1 + w->nws) { wm_switch_workspace(w, keycode - KEY_1); return; }
        if (keycode == KEY_F4) { Win *f = wm_find(w, (uint32_t)w->focus);
        if (f) send(w, f, E_CLOSE, 0, 0, 0, 0);
        return; }
        if (keycode == KEY_TAB) { cycle_windows(w, (w->mods & KMOD_SHIFT) != 0); return; }
        if (keycode == KEY_T && (w->mods & KMOD_CTRL)) { hotkey(w, 2); return; }        /* Ctrl+Alt+T: terminal (the panel starts it) */
    }
    if (value == 2 && (w->mods & KMOD_ALT) && (keycode == KEY_TAB || keycode == KEY_F4 || (keycode >= KEY_1 && keycode < KEY_1 + w->nws) || keycode == KEY_T)) return;   /* shortcuts do not auto-repeat into the app */
    for (int i = WM_MAX_WINDOWS - 1; i >= 0; i--) {                                  /* an open popup (launcher, menu) owns the keyboard */
        Win *p = &w->wins[i]; if (p->used && p->kind == WK_POPUP) { send(w, p, E_KEY, keycode, key_ascii(w, keycode), value != 0, w->mods); return; }
    }
    Win *f = wm_find(w, (uint32_t)w->focus);
    if (f && f->kind == WK_NORMAL && visible(w, f)) send(w, f, E_KEY, keycode, key_ascii(w, keycode), value != 0, w->mods);
}

/* ---- drawing ---- */
void wm_draw_cursor(Surface *s, int px, int py)
{
    static const char *const spr[16] = {
        "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
        "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ", "X    X..X   ", "     XXX    " };
    for (int y = 0; y < 16; y++) for (int x = 0; x < 12; x++) {
        char c = spr[y][x]; if (c == ' ') continue; int dx = px + x, dy = py + y; if (dx < 0 || dy < 0 || dx >= s->w || dy >= s->h) continue;
        s->px[(size_t)dy * (size_t)s->stride + (size_t)dx] = c == 'X' ? TH_VOID : TH_TEXT;
    }
}
Rect wm_cursor_rect(const Wm *w) { return rect_make(w->ptr_x, w->ptr_y, 12, 16); }

static void draw_controls(const Wm *w, Surface *dst, const Win *x, int active)
{
    static const int parts[3] = { P_CLOSE, P_MAX, P_MIN };
    for (int k = 0; k < 3; k++) {
        int part = parts[k]; if (part == P_MAX && !has_max(x)) continue;
        Rect r = btn_rect(x, part);
        int hot = w->hot_win == (int)x->id && w->hot_btn == part, press = w->press_win == (int)x->id && w->press_btn == part;
        uint32_t icon = active ? TH_TEXT2 : TH_MUTED;
        if (press || hot) {
            if (part == P_CLOSE) { gfx_rrect(dst, r, TH_R_CTRL, press ? gfx_mix(TH_CLOSE_HOVER, TH_VOID, 50) : TH_CLOSE_HOVER); icon = TH_VOID; }
            else { gfx_rrect(dst, r, TH_R_CTRL, press ? TH_SURFACE2_PRESS : gfx_mix(active ? TH_SURFACE2 : TH_WINDOW, TH_TEXT, 30)); icon = TH_TEXT; }
        }
        IconId id = part == P_CLOSE ? ICON_CLOSE : (part == P_MAX ? (x->maximized ? ICON_RESTORE : ICON_MAXIMIZE) : ICON_MINIMIZE);
        icon_draw_ex(dst, id, r.x + (r.w - 18) / 2, r.y + (r.h - 18) / 2, 18, icon, icon, -1);
    }
}

static void draw_normal(Wm *w, Surface *dst, const Win *x)
{
    Surface src = { x->buf, x->w, x->h, x->w };
    int active = w->focus == (int)x->id, rad = x->maximized ? 0 : TH_R_WIN;
    Rect body = rect_make(x->x, x->y - WM_TITLE_H, x->w, x->h + WM_TITLE_H), outer = rect_make(body.x - 1, body.y - 1, body.w + 2, body.h + 2), tr = title_rect(x);
    if (!x->maximized) gfx_shadow(dst, body, rad, TH_SHADOW_BLUR, 4, active ? 150 : 96);
    gfx_rrect(dst, outer, rad ? rad + 1 : 0, active ? TH_BORDER_ACTIVE : TH_BORDER);
    gfx_rrect(dst, body, rad, TH_WINDOW);
    gfx_rrect_ex(dst, tr, rad, rad, 0, 0, active ? TH_TITLE_ACTIVE : TH_TITLE_INACTIVE, 255);
    gfx_fill(dst, rect_make(tr.x, tr.y + tr.h - 1, tr.w, 1), TH_BORDER);
    char t[64]; const Face *f = font_face(FW_MEDIUM, TY_TITLE_PX);
    font_ellipsize(t, sizeof t, x->title, f, th_max(0, x->w - controls_w(x) - TH_S4 - TH_S2));
    font_draw_box(dst, rect_make(tr.x + TH_S4, tr.y, tr.w - TH_S4, tr.h), t, f, active ? TH_TEXT : TH_TEXT2, FA_LEFT);
    draw_controls(w, dst, x, active);
    gfx_blit_rounded(dst, x->x, x->y, &src, rect_make(0, 0, x->w, x->h), 0, 0, rad, rad, 255);
}
static void draw_overlay(Surface *dst, const Win *x)       /* popups and notifications: rounded, bordered, shadowed, optionally faded */
{
    Surface src = { x->buf, x->w, x->h, x->w }; unsigned a = (unsigned)x->alpha;
    Rect r = client_rect(x);
    gfx_shadow(dst, r, TH_R_MENU, 10, 4, 140 * a / 255);
    gfx_rrect_a(dst, rect_make(r.x - 1, r.y - 1, r.w + 2, r.h + 2), TH_R_MENU + 1, TH_BORDER, a);
    gfx_blit_rounded(dst, x->x, x->y, &src, rect_make(0, 0, x->w, x->h), TH_R_MENU, TH_R_MENU, TH_R_MENU, TH_R_MENU, a);
}
void wm_compose_scene(Wm *w, Surface *dst)
{
    const Win *desk = NULL;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (w->wins[i].used && w->wins[i].kind == WK_DESKTOP) desk = &w->wins[i];
    gfx_gradient_v(dst, rect_make(0, 0, dst->w, dst->h), TH_DESKTOP, TH_VOID);          /* fallback until a desktop client paints the wallpaper */
    if (desk) { Surface s = { desk->buf, desk->w, desk->h, desk->w }; gfx_blit(dst, desk->x, desk->y, &s, rect_make(0, 0, desk->w, desk->h)); }
    for (int i = 0; i < w->nz; i++) { Win *x = &w->wins[w->z[i]]; if (visible(w, x)) draw_normal(w, dst, x); }
    for (int i = 0; i < WM_MAX_WINDOWS; i++) { const Win *x = &w->wins[i]; if (x->used && x->kind == WK_PANEL) { Surface s = { x->buf, x->w, x->h, x->w }; gfx_blit(dst, x->x, x->y, &s, rect_make(0, 0, x->w, x->h)); } }
    for (int k = WK_NOTIFY; k <= WK_POPUP; k++) for (int i = 0; i < WM_MAX_WINDOWS; i++) if (w->wins[i].used && w->wins[i].kind == k && w->wins[i].alpha > 0) draw_overlay(dst, &w->wins[i]);
    w->dirty = 0;
}
void wm_compose(Wm *w, Surface *dst) { wm_compose_scene(w, dst); wm_draw_cursor(dst, w->ptr_x, w->ptr_y); w->cursor_dirty = 0; }
