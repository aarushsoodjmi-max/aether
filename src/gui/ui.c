#define _GNU_SOURCE
#include "ui.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

static void seterr(char *e, size_t n, const char *m) { if (e) snprintf(e, n, "%s", m); }
static void send_simple(UiWindow *w, uint32_t type, int a, int b, int c, int d) { Msg m; memset(&m, 0, sizeof m); m.type = type; m.window = w->id; m.v[0] = a; m.v[1] = b; m.v[2] = c; m.v[3] = d; proto_send(w->sock, &m, -1); }
long ui_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

int ui_connect(UiWindow *w, char *err, size_t errlen)
{
    memset(w, 0, sizeof *w); w->sock = -1; w->memfd = -1; w->bg = TH_WINDOW; w->page = 0; w->focus = -1; w->watch_fd = -1;
    const char *path = getenv("AETHER_DISPLAY") ? getenv("AETHER_DISPLAY") : AETHER_DISPLAY_SOCK;
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) { seterr(err, errlen, "socket path too long"); return -1; }
    strcpy(a.sun_path, path);
    for (int tries = 0; tries < 50; tries++) {                                     /* the compositor may still be starting */
        w->sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (w->sock >= 0 && connect(w->sock, (struct sockaddr *)&a, sizeof a) == 0) break;
        if (w->sock >= 0) { close(w->sock); w->sock = -1; } usleep(100000);
    }
    if (w->sock < 0) { seterr(err, errlen, "cannot connect to the display server (is aether-compositor running?)"); return -1; }
    Msg m; memset(&m, 0, sizeof m); m.type = M_HELLO; m.v[0] = AETHER_PROTO_VERSION; if (proto_send(w->sock, &m, -1) != 0) { seterr(err, errlen, "hello failed"); return -1; }
    int fd; if (proto_recv(w->sock, &m, &fd) != 1 || m.type != E_SCREEN) { seterr(err, errlen, "no screen info from compositor"); return -1; }
    w->screen_w = m.v[0]; w->screen_h = m.v[1]; w->workspace = m.v[2]; w->nws = m.v[3]; return 0;
}

static int alloc_buffer(UiWindow *w, int cw, int ch, int *memfd, uint32_t **px, size_t *size)
{
    *size = (size_t)cw * (size_t)ch * 4;
    *memfd = memfd_create("aether-window", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (*memfd < 0 || ftruncate(*memfd, (off_t)*size) != 0 || fcntl(*memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) return -1;
    *px = mmap(NULL, *size, PROT_READ | PROT_WRITE, MAP_SHARED, *memfd, 0);
    if (*px == MAP_FAILED) { *px = NULL; return -1; }
    (void)w; return 0;
}
int ui_create(UiWindow *w, const char *title, int kind, int cw, int ch, int px, int py, char *err, size_t errlen)
{
    w->kind = kind; w->w = cw; w->h = ch; snprintf(w->title, sizeof w->title, "%s", title);
    if (alloc_buffer(w, cw, ch, &w->memfd, &w->px, &w->size) != 0) { seterr(err, errlen, "cannot allocate window memory"); return -1; }
    w->s.px = w->px; w->s.w = cw; w->s.h = ch; w->s.stride = cw;
    gfx_fill(&w->s, rect_make(0, 0, cw, ch), w->bg);
    Msg m; memset(&m, 0, sizeof m); m.type = M_CREATE_WINDOW; m.v[0] = cw; m.v[1] = ch; m.v[2] = kind; m.v[3] = px; m.v[4] = py; m.v[5] = w->flags; snprintf(m.text, sizeof m.text, "%s", title);
    if (proto_send(w->sock, &m, w->memfd) != 0) { seterr(err, errlen, "CREATE_WINDOW failed"); return -1; }
    for (;;) {                                                                       /* wait for the CONFIGURE that carries our window id */
        int fd; int r = proto_recv(w->sock, &m, &fd); if (r != 1) { seterr(err, errlen, "compositor rejected the window"); return -1; }
        if (m.type == E_CONFIGURE) { w->id = m.window; break; }
    }
    return 0;
}
int ui_resize(UiWindow *w, int nw, int nh, char *err, size_t errlen)
{
    int fd; uint32_t *px; size_t size;
    if (alloc_buffer(w, nw, nh, &fd, &px, &size) != 0) { seterr(err, errlen, "cannot allocate window memory"); return -1; }
    Msg m; memset(&m, 0, sizeof m); m.type = M_SET_BUFFER; m.window = w->id; m.v[0] = nw; m.v[1] = nh;
    if (proto_send(w->sock, &m, fd) != 0) { munmap(px, size); close(fd); seterr(err, errlen, "SET_BUFFER failed"); return -1; }
    if (w->px) munmap(w->px, w->size);
    if (w->memfd >= 0) close(w->memfd);
    w->memfd = fd; w->px = px; w->size = size; w->w = nw; w->h = nh; w->s.px = px; w->s.w = nw; w->s.h = nh; w->s.stride = nw;
    return 0;
}

static int add(UiWindow *w, UiType t, Rect r, const char *text, uint32_t fg, uint32_t bg, int page, UiCallback cb, void *user)
{
    if (w->nwid >= UI_MAX_WIDGETS) return -1;
    UiWidget *x = &w->wid[w->nwid]; memset(x, 0, sizeof *x); x->type = t; x->r = r; snprintf(x->text, sizeof x->text, "%s", text ? text : ""); x->fg = fg; x->bg = bg; x->page = page; x->cb = cb; x->user = user;
    x->weight = FW_REGULAR; x->px = TY_BODY_PX; x->align = FA_LEFT;
    return w->nwid++;
}
int ui_label_ex(UiWindow *w, int x, int y, const char *text, uint32_t fg, int weight, int px, int page)
{
    const Face *f = font_face(weight, px);
    int i = add(w, UI_LABEL, rect_make(x, y, font_width(f, text), font_line_h(f)), text, fg, 0, page, NULL, NULL);
    if (i >= 0) { w->wid[i].weight = weight; w->wid[i].px = px; }
    return i;
}
int ui_label(UiWindow *w, int x, int y, const char *text, uint32_t fg, int page) { return ui_label_ex(w, x, y, text, fg, FW_REGULAR, TY_BODY_PX, page); }
int ui_button(UiWindow *w, Rect r, const char *text, UiCallback cb, void *user, int page) { int i = add(w, UI_BUTTON, r, text, TH_TEXT, 0, page, cb, user); if (i >= 0) { w->wid[i].weight = FW_MEDIUM; w->wid[i].interactive = 1; } return i; }
int ui_button_primary(UiWindow *w, Rect r, const char *text, UiCallback cb, void *user, int page) { int i = ui_button(w, r, text, cb, user, page); if (i >= 0) w->wid[i].primary = 1; return i; }
int ui_toggle(UiWindow *w, int x, int y, const char *text, int checked, UiCallback cb, void *user, int page)
{
    int i = add(w, UI_TOGGLE, rect_make(x, y, 36 + 10 + font_width(font_face(FW_REGULAR, TY_BODY_PX), text), 20), text, TH_TEXT, 0, page, cb, user);
    if (i >= 0) { w->wid[i].checked = checked; w->wid[i].interactive = 1; }
    return i;
}
int ui_separator(UiWindow *w, Rect r, int page) { return add(w, UI_SEPARATOR, r, "", 0, TH_BORDER, page, NULL, NULL); }
int ui_entry(UiWindow *w, Rect r, const char *hint, UiCallback on_change, void *user, int page)
{
    int i = add(w, UI_ENTRY, r, "", TH_TEXT, 0, page, on_change, user);
    if (i >= 0) { w->wid[i].hint = hint; w->focus = i; }
    return i;
}
int ui_custom(UiWindow *w, Rect r, UiDraw draw, UiCallback on_click, void *user, int interactive, int page)
{
    int i = add(w, UI_CUSTOM, r, "", TH_TEXT, 0, page, on_click, user); if (i >= 0) { w->wid[i].draw = draw; w->wid[i].interactive = interactive; }
    return i;
}
void ui_clear(UiWindow *w) { w->nwid = 0; w->focus = -1; }
void ui_set_text(UiWindow *w, int i, const char *text)
{
    if (i < 0 || i >= w->nwid) return;
    UiWidget *x = &w->wid[i];
    snprintf(x->text, sizeof x->text, "%s", text);
    if (x->type == UI_LABEL) x->r.w = font_width(font_face(x->weight, x->px), text);
}
void ui_set_colors(UiWindow *w, int i, uint32_t fg, uint32_t bg) { if (i >= 0 && i < w->nwid) { w->wid[i].fg = fg; w->wid[i].bg = bg; } }
void ui_set_page(UiWindow *w, int page) { w->page = page; ui_redraw(w); }
static int shown(const UiWindow *w, const UiWidget *x) { return x->page == UI_ALL_PAGES || x->page == w->page; }

static void draw_button(UiWindow *w, const UiWidget *x, int idx)
{
    Surface *s = &w->s; uint32_t fill, text = x->disabled ? TH_MUTED : x->fg;
    if (w->focus == idx) gfx_rrect(s, rect_make(x->r.x - 2, x->r.y - 2, x->r.w + 4, x->r.h + 4), TH_R_CTRL + 2, TH_BLUE);
    if (x->primary) {
        fill = x->disabled ? TH_SURFACE2 : (x->pressed ? TH_BLUE_PRESS : (x->hover ? TH_BLUE_HOVER : TH_BLUE));
        gfx_rrect(s, x->r, TH_R_CTRL, fill); if (!x->disabled) text = TH_VOID;
    } else {
        fill = x->disabled ? TH_WINDOW : (x->pressed ? TH_SURFACE2_PRESS : (x->hover ? TH_SURFACE2_HOVER : (x->bg ? x->bg : TH_SURFACE2)));
        gfx_rrect(s, x->r, TH_R_CTRL, x->hover && !x->disabled ? gfx_mix(TH_BORDER, TH_BLUE, 120) : TH_BORDER);
        gfx_rrect(s, rect_make(x->r.x + 1, x->r.y + 1, x->r.w - 2, x->r.h - 2), TH_R_CTRL - 1, fill);
    }
    font_draw_box(s, x->r, x->text, font_face(x->weight, TY_BODY_PX), text, FA_CENTER);
}
static void draw_toggle(UiWindow *w, const UiWidget *x)
{
    Surface *s = &w->s; Rect t = rect_make(x->r.x, x->r.y, 36, 20);
    if (x->checked) gfx_rrect(s, t, 10, x->hover ? TH_BLUE_HOVER : TH_BLUE);
    else { gfx_rrect(s, t, 10, x->hover ? gfx_mix(TH_BORDER, TH_BLUE, 120) : TH_BORDER); gfx_rrect(s, rect_make(t.x + 1, t.y + 1, 34, 18), 9, TH_SURFACE2); }
    gfx_disc(s, (float)(x->checked ? t.x + 26 : t.x + 10), (float)(t.y + 10), 6.5f, x->checked ? TH_TEXT : TH_MUTED, 255);
    font_draw_box(s, rect_make(x->r.x + 46, x->r.y - 2, x->r.w - 46, 24), x->text, font_face(FW_REGULAR, TY_BODY_PX), TH_TEXT, FA_LEFT);
}
static void draw_entry(UiWindow *w, const UiWidget *x, int idx)
{
    Surface *s = &w->s; int focused = w->focus == idx;
    if (focused) gfx_rrect(s, rect_make(x->r.x - 1, x->r.y - 1, x->r.w + 2, x->r.h + 2), TH_R_CTRL + 1, TH_BLUE);
    else gfx_rrect(s, x->r, TH_R_CTRL, TH_BORDER);
    Rect in = rect_make(x->r.x + 1, x->r.y + 1, x->r.w - 2, x->r.h - 2); gfx_rrect(s, in, TH_R_CTRL - 1, TH_DESKTOP);
    icon_draw(s, ICON_SEARCH, in.x + TH_S3, in.y + (in.h - 16) / 2, 16, focused ? IS_ACTIVE : IS_MUTED);
    const Face *f = font_face(FW_REGULAR, TY_BODY_PX); int tx = in.x + TH_S3 + 16 + TH_S2; Rect box = rect_make(tx, in.y, in.w - (tx - in.x) - TH_S3, in.h);
    int tw = 0;
    if (x->text[0]) tw = font_draw_box(s, box, x->text, f, TH_TEXT, FA_LEFT);
    else if (x->hint) font_draw_box(s, box, x->hint, f, TH_MUTED, FA_LEFT);
    if (focused && (ui_now_ms() / 530) % 2 == 0) gfx_fill(s, rect_make(tx + tw + 1, in.y + (in.h - 16) / 2, 1, 16), TH_CYAN);
}
void ui_redraw(UiWindow *w)
{
    if (!w->px) return;
    gfx_fill(&w->s, rect_make(0, 0, w->w, w->h), w->bg);
    for (int i = 0; i < w->nwid; i++) {
        UiWidget *x = &w->wid[i]; if (!shown(w, x)) continue;
        switch (x->type) {
        case UI_LABEL: font_draw_box(&w->s, x->r, x->text, font_face(x->weight, x->px), x->fg, FA_LEFT); break;
        case UI_SEPARATOR: gfx_fill(&w->s, x->r, x->bg); break;
        case UI_BUTTON: draw_button(w, x, i); break;
        case UI_TOGGLE: draw_toggle(w, x); break;
        case UI_ENTRY: draw_entry(w, x, i); break;
        case UI_CUSTOM: if (x->draw) x->draw(w, i, &w->s); break;
        }
    }
    send_simple(w, M_COMMIT, 0, 0, w->w, w->h);
}
void ui_switch_workspace(UiWindow *w, int index) { send_simple(w, M_SWITCH_WORKSPACE, index, 0, 0, 0); }
void ui_set_opacity(UiWindow *w, int alpha) { send_simple(w, M_SET_OPACITY, alpha, 0, 0, 0); }
void ui_task_activate(UiWindow *w, uint32_t id) { Msg m; memset(&m, 0, sizeof m); m.type = M_TASK_ACTIVATE; m.window = id; proto_send(w->sock, &m, -1); }
void ui_task_raise(UiWindow *w, uint32_t id) { Msg m; memset(&m, 0, sizeof m); m.type = M_TASK_ACTIVATE; m.window = id; m.v[0] = 1; proto_send(w->sock, &m, -1); }
void ui_task_close(UiWindow *w, uint32_t id) { Msg m; memset(&m, 0, sizeof m); m.type = M_TASK_CLOSE; m.window = id; proto_send(w->sock, &m, -1); }

static int widget_at(UiWindow *w, int x, int y)
{
    for (int i = w->nwid - 1; i >= 0; i--) { UiWidget *k = &w->wid[i]; if (shown(w, k) && k->interactive && !k->disabled && rect_contains(k->r, x, y)) return i; }
    return -1;
}
static void handle_key(UiWindow *w, int key, int ascii, int pressed)
{
    if (pressed && w->focus >= 0 && w->focus < w->nwid && w->wid[w->focus].type == UI_ENTRY) {
        UiWidget *e = &w->wid[w->focus]; size_t n = strlen(e->text); int changed = 0;
        if (ascii >= 32 && ascii < 127 && n < sizeof e->text - 1) { e->text[n] = (char)ascii; e->text[n + 1] = 0; changed = 1; }
        else if (ascii == '\b' && n) { e->text[n - 1] = 0; changed = 1; }
        if (changed) { if (e->cb) e->cb(w, w->focus, e->user); ui_redraw(w); }
    }
    if (w->on_key) w->on_key(w, key, ascii, pressed);
}
static void handle_event(UiWindow *w, const Msg *m)
{
    int changed = 0;
    switch (m->type) {
    case E_CLOSE: w->closed = 1; break;
    case E_WORKSPACE: w->workspace = m->v[0]; w->nws = m->v[1]; if (w->on_workspace) w->on_workspace(w); ui_redraw(w); break;
    case E_KEY: w->mods = m->v[3]; handle_key(w, m->v[0], m->v[1], m->v[2]); break;
    case E_HOTKEY: if (w->on_hotkey) w->on_hotkey(w, m->v[0]); break;
    case E_TASK: if (w->on_task) w->on_task(w, m->v[0], m->window, m->v[1], m->text); break;
    case E_SCREEN: w->screen_w = m->v[0]; w->screen_h = m->v[1]; w->workspace = m->v[2]; w->nws = m->v[3]; if (w->on_screen) w->on_screen(w); break;
    case E_CONFIGURE: if (m->v[4] == 1 && w->on_resize) w->on_resize(w, m->v[2], m->v[3]); break;
    case E_POINTER: {
        int kind = m->v[0], px = m->v[1], py = m->v[2]; int over = (kind == 4) ? -1 : widget_at(w, px, py);
        if (kind == 5) { if (w->on_pointer) w->on_pointer(w, 5, px, py, m->v[3], 0); break; }      /* wheel: apps only */
        if (kind == 1) { w->press_dismissed = m->v[4]; w->clicks = m->v[5] > 0 ? m->v[5] : 1; }
        if (w->on_pointer) w->on_pointer(w, kind, px, py, m->v[3], m->v[5]);
        for (int k = 0; k < w->nwid; k++) { int h = (k == over); if (w->wid[k].hover != h) { w->wid[k].hover = h; changed = 1; if (h && w->on_hover) w->on_hover(w, k); } if (kind == 4 && w->wid[k].pressed) { w->wid[k].pressed = 0; changed = 1; } }
        int primary = m->v[3] == 1;                                    /* only the primary button presses and activates widgets; right/middle never do */
        if (kind == 1 && m->v[3] == 3) { w->ctx_armed = m->v[4] == 0; w->ctx_widget = over; }      /* right press: arm a context event unless it only dismissed a popup */
        if (kind == 1 && primary && over >= 0) { w->wid[over].pressed = 1; changed = 1; if (w->wid[over].type == UI_ENTRY) w->focus = over; }
        if (kind == 2 && m->v[3] == 3) {
            int armed = w->ctx_armed && w->ctx_widget == over; w->ctx_armed = 0;
            if (armed && w->on_context) w->on_context(w, over, px, py);
        }
        if (kind == 2 && primary) {
            for (int k = 0; k < w->nwid; k++) if (w->wid[k].pressed) {
                w->wid[k].pressed = 0; changed = 1;
                if (k == over) { if (w->wid[k].type == UI_TOGGLE) w->wid[k].checked = !w->wid[k].checked; if (w->wid[k].cb) w->wid[k].cb(w, k, w->wid[k].user); }
            }
        }
        if (changed) ui_redraw(w);
        break; }
    default: break;
    }
}
void ui_run(UiWindow *w)
{
    ui_redraw(w);
    while (!w->closed) {
        struct pollfd p[2] = { { w->sock, POLLIN, 0 }, { w->watch_fd, POLLIN, 0 } }; int r = poll(p, w->watch_fd >= 0 ? 2 : 1, w->tick_ms > 0 ? w->tick_ms : -1);
        if (r < 0 && errno != EINTR) break;
        if (r > 0 && (p[0].revents & (POLLIN | POLLHUP))) { Msg m; int fd; int rr = proto_recv(w->sock, &m, &fd); if (rr == 0) break; if (rr == 1) handle_event(w, &m); }
        if (r > 0 && w->watch_fd >= 0 && (p[1].revents & (POLLIN | POLLHUP)) && w->on_watch) w->on_watch(w);
        if (r == 0 && w->on_tick) w->on_tick(w);
    }
}
void ui_destroy(UiWindow *w)
{
    if (w->sock >= 0) { send_simple(w, M_DESTROY, 0, 0, 0, 0); close(w->sock); }
    if (w->px) munmap(w->px, w->size);
    if (w->memfd >= 0) close(w->memfd);
    w->sock = w->memfd = -1; w->px = NULL;
}
