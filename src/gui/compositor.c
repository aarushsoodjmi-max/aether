/* aether-compositor: owns the display and input devices, runs the window manager, serves clients.
 * Protocol and window semantics: proto.h / wm.h. Runs as root in this prototype (no privilege separation yet).
 *
 *   aether-compositor                       KMS dumb-buffer display + evdev input (the real desktop)
 *   aether-compositor --headless WxH --control PATH
 *                                           software display + a text control socket for tests and screenshots:
 *                                           move dx dy | warp x y | abs VX VY MAX | button 0|1 | buttonx 1|2|3 0|1 | wheel N |
 *                                           key CODE 0|1 | shot FILE.ppm | windows | size | resize W H | quit.
 *                                           ('abs' feeds a raw tablet coordinate 0..MAX through the same scaling code as a real
 *                                           evdev tablet.)  Never available on the KMS path. */
#define _GNU_SOURCE
#include "display.h"
#include "input.h"
#include "proto.h"
#include "wm.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define MAX_CLIENTS 16
#define MAX_WINDOWS_PER_CLIENT 16
static int clients[MAX_CLIENTS];
static Wm wm;
static Display d;
static Surface scene, back;            /* scene = everything but the pointer; back = scene + pointer (what is presented) */
static Rect cur_rect;                  /* where the pointer was last drawn into back */
static volatile sig_atomic_t quit;
static long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
/* Apply one normalised input event to the window manager. Keyboard keys and pointer buttons are told apart by InKind, never by
 * code range; relative motion is added, absolute positions are scaled to the display exactly once and then placed. */
static void apply_input(const InEvent *e)
{
    wm_set_time(&wm, mono_ms());
    switch (e->kind) {
    case IN_MOTION: wm_pointer_move(&wm, e->dx, e->dy); break;
    case IN_ABS: wm_pointer_to(&wm, input_abs_scale(e->dx, e->x0, e->x1, wm.sw), input_abs_scale(e->dy, e->y0, e->y1, wm.sh)); break;
    case IN_BUTTON:
        if (e->code == BTN_LEFT) wm_button_ex(&wm, 1, e->value != 0);
        else if (e->code == BTN_MIDDLE) wm_button_ex(&wm, 2, e->value != 0);
        else if (e->code == BTN_RIGHT) wm_button_ex(&wm, 3, e->value != 0);
        break;
    case IN_WHEEL: wm_wheel(&wm, e->dy); break;
    case IN_KEY: wm_key(&wm, e->code, e->value); break;
    }
}

static void on_signal(int s) { (void)s; quit = 1; }
static void emit(void *ctx, int client, const Msg *m)
{
    (void)ctx;
    if (client < 0 || client >= MAX_CLIENTS || clients[client] < 0) return;
    if (proto_send(clients[client], m, -1) != 0 && errno != EPIPE) fprintf(stderr, "aether-compositor: send to client %d failed: %s\n", client, strerror(errno));
}
static void unmap_buf(uint32_t *buf, size_t size) { munmap(buf, size); }
static void drop_client(int c)
{
    int n = wm_remove_client(&wm, c, unmap_buf); close(clients[c]); clients[c] = -1;
    printf("aether-compositor: client %d disconnected (%d windows closed)\n", c, n);
}
static int client_windows(int c) { int n = 0; for (int i = 0; i < WM_MAX_WINDOWS; i++) if (wm.wins[i].used && wm.wins[i].client == c) n++; return n; }
static int client_is_panel(int c) { for (int i = 0; i < WM_MAX_WINDOWS; i++) if (wm.wins[i].used && wm.wins[i].client == c && wm.wins[i].kind == WK_PANEL) return 1; return 0; }

static int alloc_surfaces(int w, int h)
{
    free(scene.px); free(back.px);
    scene = (Surface){ calloc((size_t)w * (size_t)h, 4), w, h, w }; back = (Surface){ calloc((size_t)w * (size_t)h, 4), w, h, w };
    cur_rect = rect_make(0, 0, 0, 0);
    return scene.px && back.px ? 0 : -1;
}
static Rect rect_union(Rect a, Rect b)
{
    if (a.w <= 0 || a.h <= 0) return b;
    if (b.w <= 0 || b.h <= 0) return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y, x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}
/* Redraw only what changed: a full recompose when the scene changed, otherwise just move the pointer sprite. */
static void render(void)
{
    if (wm.dirty) {
        wm_compose_scene(&wm, &scene); gfx_blit(&back, 0, 0, &scene, rect_make(0, 0, scene.w, scene.h));
        wm_draw_cursor(&back, wm.ptr_x, wm.ptr_y); cur_rect = wm_cursor_rect(&wm); wm.cursor_dirty = 0;
        display_present(&d, &back);
    } else if (wm.cursor_dirty) {
        Rect old = cur_rect, now = wm_cursor_rect(&wm);
        gfx_blit(&back, old.x, old.y, &scene, old);
        wm_draw_cursor(&back, wm.ptr_x, wm.ptr_y); cur_rect = now; wm.cursor_dirty = 0;
        display_present_rect(&d, &back, rect_union(old, now));
    }
}

/* State log for diagnostics and the interactive tests: one line whenever a normal window's geometry/state or the focus changes
 * (not while a title-bar drag or an edge resize is in progress: the final geometry is logged on release). */
typedef struct { uint32_t id; int x, y, w, h, min, max, ws; } WinSnap;
static WinSnap snap[WM_MAX_WINDOWS]; static int last_focus = -1;
static void log_changes(void)
{
    if (wm.focus != last_focus) { last_focus = wm.focus; printf("aether-compositor: focus -> window %d\n", wm.focus); }
    if (wm.drag || wm.rsz) return;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        const Win *x = &wm.wins[i]; WinSnap *o = &snap[i];
        if (!x->used || x->kind != WK_NORMAL) { o->id = 0; continue; }
        if (o->id == x->id && o->x == x->x && o->y == x->y && o->w == x->w && o->h == x->h && o->min == x->minimized && o->max == x->maximized && o->ws == x->ws) continue;
        *o = (WinSnap){ x->id, x->x, x->y, x->w, x->h, x->minimized, x->maximized, x->ws };
        printf("aether-compositor: window %u '%s' at %d,%d size %dx%d minimized=%d maximized=%d workspace=%d\n", x->id, x->title, x->x, x->y, x->w, x->h, x->minimized, x->maximized, x->ws + 1);
    }
}
static void handle_message(int c, const Msg *m, int fd)
{
    switch (m->type) {
    case M_HELLO: {
        if (m->v[0] != AETHER_PROTO_VERSION) { fprintf(stderr, "aether-compositor: client %d speaks protocol %d; dropping\n", c, m->v[0]); drop_client(c); break; }
        Msg r; memset(&r, 0, sizeof r); r.type = E_SCREEN; r.v[0] = wm.sw; r.v[1] = wm.sh; r.v[2] = wm.ws; r.v[3] = wm.nws; proto_send(clients[c], &r, -1);
        break; }
    case M_CREATE_WINDOW: {
        int w = m->v[0], h = m->v[1], kind = m->v[2];
        if (fd < 0 || w < 1 || h < 1 || w > AETHER_MAX_WIN_W || h > AETHER_MAX_WIN_H || kind < WK_NORMAL || kind > WK_DESKTOP || client_windows(c) >= MAX_WINDOWS_PER_CLIENT) {
            fprintf(stderr, "aether-compositor: client %d: rejected CREATE_WINDOW (%dx%d kind %d)\n", c, w, h, kind); if (fd >= 0) close(fd); break; }
        size_t need = (size_t)w * (size_t)h * 4; struct stat st; int seals = fcntl(fd, F_GET_SEALS);
        if (fstat(fd, &st) != 0 || (size_t)st.st_size < need || seals < 0 || (seals & (F_SEAL_SHRINK | F_SEAL_GROW)) != (F_SEAL_SHRINK | F_SEAL_GROW)) {
            fprintf(stderr, "aether-compositor: client %d: buffer too small or not sealed against shrinking\n", c); close(fd); break; }
        void *buf = mmap(NULL, need, PROT_READ, MAP_SHARED, fd, 0); close(fd);
        if (buf == MAP_FAILED) break;
        uint32_t id = wm_add_window(&wm, c, kind, w, h, m->v[3], m->v[4], m->text, buf, need);
        if (!id) { munmap(buf, need); break; }
        if (m->v[5]) wm_set_flags(&wm, id, m->v[5]);
        printf("aether-compositor: window %u created kind=%d %dx%d title='%s'\n", id, kind, w, h, m->text); break; }
    case M_COMMIT: { Win *x = wm_find(&wm, m->window); if (x && x->client == c) wm.dirty = 1; break; }
    case M_DESTROY: { Win *x = wm_find(&wm, m->window); if (x && x->client == c) { unmap_buf(x->buf, x->bufsize); wm_remove_window(&wm, m->window); printf("aether-compositor: window %u destroyed\n", m->window); } break; }
    case M_SWITCH_WORKSPACE: wm_switch_workspace(&wm, m->v[0]); break;
    case M_SET_OPACITY: { Win *x = wm_find(&wm, m->window); if (x && x->client == c) wm_set_opacity(&wm, m->window, m->v[0]); break; }
    case M_TASK_ACTIVATE: if (client_is_panel(c)) { if (m->v[0] == 1) wm_task_raise(&wm, m->window); else wm_task_activate(&wm, m->window); } break;
    case M_TASK_CLOSE: if (client_is_panel(c)) wm_task_close(&wm, m->window); break;
    case M_SET_BUFFER: {
        Win *x = wm_find(&wm, m->window); int w = m->v[0], h = m->v[1];
        if (fd < 0) break;
        if (!x || x->client != c || w < 1 || h < 1 || w > AETHER_MAX_WIN_W || h > AETHER_MAX_WIN_H) { close(fd); break; }
        size_t need = (size_t)w * (size_t)h * 4; struct stat st; int seals = fcntl(fd, F_GET_SEALS);
        if (fstat(fd, &st) != 0 || (size_t)st.st_size < need || seals < 0 || (seals & (F_SEAL_SHRINK | F_SEAL_GROW)) != (F_SEAL_SHRINK | F_SEAL_GROW)) { close(fd); break; }
        void *buf = mmap(NULL, need, PROT_READ, MAP_SHARED, fd, 0); close(fd);
        if (buf == MAP_FAILED) break;
        if (wm_replace_buffer(&wm, m->window, w, h, buf, need, unmap_buf) != 0) munmap(buf, need);
        break; }
    default: break;
    }
}

/* ---- headless control channel ---- */
static int ctl_listen = -1, ctl_conn = -1; static char ctl_buf[512]; static size_t ctl_len;
static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", back.w, back.h); unsigned char *row = malloc((size_t)back.w * 3);
    for (int y = 0; row && y < back.h; y++) { for (int x = 0; x < back.w; x++) { uint32_t p = back.px[(size_t)y * (size_t)back.stride + (size_t)x]; row[x * 3] = (unsigned char)(p >> 16); row[x * 3 + 1] = (unsigned char)(p >> 8); row[x * 3 + 2] = (unsigned char)p; } fwrite(row, 3, (size_t)back.w, f); }
    free(row); fclose(f);
}
static void ctl_command(char *line)
{
    int a = 0, b = 0; char path[256];
    int c3 = 0; wm_set_time(&wm, mono_ms());
    if (sscanf(line, "move %d %d", &a, &b) == 2) wm_pointer_move(&wm, a, b);
    else if (sscanf(line, "warp %d %d", &a, &b) == 2) wm_pointer_to(&wm, a, b);
    else if (sscanf(line, "abs %d %d %d", &a, &b, &c3) == 3 && c3 > 0) { InEvent e; memset(&e, 0, sizeof e); e.kind = IN_ABS; e.dx = a; e.dy = b; e.x1 = e.y1 = c3; apply_input(&e); }
    else if (sscanf(line, "buttonx %d %d", &a, &b) == 2) wm_button_ex(&wm, a, b != 0);
    else if (sscanf(line, "button %d", &a) == 1) wm_button(&wm, a != 0);
    else if (sscanf(line, "wheel %d", &a) == 1) wm_wheel(&wm, a);
    else if (sscanf(line, "key %d %d", &a, &b) == 2) wm_key(&wm, a, b);
    else if (sscanf(line, "shot %255s", path) == 1) { render(); write_ppm(path); }
    else if (!strncmp(line, "size", 4)) { dprintf(ctl_conn, "size %d %d\n", wm.sw, wm.sh); }
    else if (sscanf(line, "resize %d %d", &a, &b) == 2) {
        if (display_resize_headless(&d, a, b) == 0 && alloc_surfaces(a, b) == 0) wm_resize_screen(&wm, a, b);
    }
    else if (!strncmp(line, "windows", 7)) {
        for (int i = 0; i < WM_MAX_WINDOWS; i++) { const Win *x = &wm.wins[i]; if (x->used) dprintf(ctl_conn, "win %u kind=%d x=%d y=%d w=%d h=%d ws=%d min=%d max=%d alpha=%d focus=%d title=%s\n", x->id, x->kind, x->x, x->y, x->w, x->h, x->ws, x->minimized, x->maximized, x->alpha, wm.focus == (int)x->id, x->title); }
        dprintf(ctl_conn, "ptr %d %d ws %d\n", wm.ptr_x, wm.ptr_y, wm.ws);
    }
    else if (!strncmp(line, "quit", 4)) quit = 1;
    dprintf(ctl_conn, "ok\n");
}
static void ctl_read(void)
{
    ssize_t r = read(ctl_conn, ctl_buf + ctl_len, sizeof ctl_buf - ctl_len - 1);
    if (r <= 0) { close(ctl_conn); ctl_conn = -1; ctl_len = 0; return; }
    ctl_len += (size_t)r; ctl_buf[ctl_len] = 0;
    char *nl; while ((nl = strchr(ctl_buf, '\n'))) { *nl = 0; ctl_command(ctl_buf); size_t rest = ctl_len - (size_t)(nl + 1 - ctl_buf); memmove(ctl_buf, nl + 1, rest); ctl_len = rest; ctl_buf[ctl_len] = 0; }
    if (ctl_len >= sizeof ctl_buf - 1) ctl_len = 0;
}

int main(int argc, char **argv)
{
    const char *sockpath = getenv("AETHER_DISPLAY") ? getenv("AETHER_DISPLAY") : AETHER_DISPLAY_SOCK, *ctlpath = NULL;
    int no_input = 0, hw = 0, hh = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-input")) no_input = 1;
        else if (!strcmp(argv[i], "--headless") && i + 1 < argc && sscanf(argv[++i], "%dx%d", &hw, &hh) != 2) { fprintf(stderr, "aether-compositor: --headless needs WxH\n"); return 2; }
        else if (!strcmp(argv[i], "--control") && i + 1 < argc) ctlpath = argv[++i];
    }
    int headless = hw > 0 && hh > 0; if (headless) no_input = 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i] = -1;
    signal(SIGPIPE, SIG_IGN); struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_signal; sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    char err[256];
    if (headless) { if (hw > AETHER_MAX_WIN_W || hh > AETHER_MAX_WIN_H || display_open_headless(&d, hw, hh) != 0) { fprintf(stderr, "aether-compositor: headless display failed\n"); return 1; } }
    else if (display_open(&d, err, sizeof err) != 0) { fprintf(stderr, "aether-compositor: display: %s\n", err); return 1; }
    printf("aether-compositor: display %dx%d via %s, driver=%s card=%s\n", d.w, d.h, headless ? "software framebuffer" : "KMS dumb buffer", d.driver, d.card);
    Input in; memset(&in, 0, sizeof in); int nin = no_input ? 0 : input_open(&in);
    printf("aether-compositor: %d input device(s)\n", nin);
    printf("aether-compositor: pointer: relative devices add counts, absolute devices (tablets) are scaled once to the %dx%d display\n", d.w, d.h);
    if (alloc_surfaces(d.w, d.h) != 0) return 1;
    wm_init(&wm, d.w, d.h, emit, NULL);
    char *dir = strdup(sockpath); char *sl = strrchr(dir, '/'); if (sl) { *sl = 0; mkdir(dir, 0755); } free(dir);
    int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0); struct sockaddr_un sun; memset(&sun, 0, sizeof sun); sun.sun_family = AF_UNIX;
    if (strlen(sockpath) >= sizeof sun.sun_path) { fprintf(stderr, "aether-compositor: socket path too long\n"); return 1; }
    strcpy(sun.sun_path, sockpath); unlink(sockpath);
    mode_t um = umask(0177);                                                         /* socket is root-only (0600) */
    if (bind(ls, (struct sockaddr *)&sun, sizeof sun) != 0 || listen(ls, 8) != 0) { fprintf(stderr, "aether-compositor: socket %s: %s\n", sockpath, strerror(errno)); return 1; }
    if (headless && ctlpath && strlen(ctlpath) < sizeof sun.sun_path) {
        struct sockaddr_un cs; memset(&cs, 0, sizeof cs); cs.sun_family = AF_UNIX; strcpy(cs.sun_path, ctlpath); unlink(ctlpath);
        ctl_listen = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (ctl_listen < 0 || bind(ctl_listen, (struct sockaddr *)&cs, sizeof cs) != 0 || listen(ctl_listen, 1) != 0) { fprintf(stderr, "aether-compositor: control socket %s: %s\n", ctlpath, strerror(errno)); return 1; }
        printf("aether-compositor: control channel on %s (headless only)\n", ctlpath);
    }
    umask(um); printf("aether-compositor: listening on %s\n", sockpath);
    render();
    time_t last_scan = time(NULL); int last_ws = wm.ws;
    while (!quit) {
        if (!no_input && time(NULL) != last_scan) {                                  /* hotplug: devices may appear later (psmouse probes asynchronously) */
            last_scan = time(NULL); int added = input_rescan(&in); if (added) printf("aether-compositor: %d input device(s) attached (now %d)\n", added, in.n);
        }
        struct pollfd pf[3 + MAX_CLIENTS + INPUT_MAX]; int n = 0, kind[3 + MAX_CLIENTS + INPUT_MAX], ref[3 + MAX_CLIENTS + INPUT_MAX];
        pf[n] = (struct pollfd){ ls, POLLIN, 0 }; kind[n] = 0; ref[n++] = 0;
        if (ctl_listen >= 0) { pf[n] = (struct pollfd){ ctl_listen, POLLIN, 0 }; kind[n] = 3; ref[n++] = 0; }
        if (ctl_conn >= 0) { pf[n] = (struct pollfd){ ctl_conn, POLLIN, 0 }; kind[n] = 4; ref[n++] = 0; }
        for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i] >= 0) { pf[n] = (struct pollfd){ clients[i], POLLIN, 0 }; kind[n] = 1; ref[n++] = i; }
        for (int i = 0; i < in.n; i++) { pf[n] = (struct pollfd){ in.d[i].fd, POLLIN, 0 }; kind[n] = 2; ref[n++] = in.d[i].fd; }
        if (poll(pf, (nfds_t)n, 250) < 0 && errno != EINTR) break;
        for (int i = 0; i < n; i++) {
            if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (kind[i] == 0) {
                int fd = accept4(ls, NULL, NULL, SOCK_CLOEXEC); if (fd < 0) continue; int slot = -1;
                for (int k = 0; k < MAX_CLIENTS; k++) if (clients[k] < 0) { slot = k; break; }
                if (slot < 0) { close(fd); continue; } clients[slot] = fd; printf("aether-compositor: client %d connected\n", slot);
            } else if (kind[i] == 3) {
                int fd = accept4(ctl_listen, NULL, NULL, SOCK_CLOEXEC); if (fd < 0) continue; if (ctl_conn >= 0) close(ctl_conn); ctl_conn = fd; ctl_len = 0;
            } else if (kind[i] == 4) { if (ctl_conn >= 0) ctl_read(); }
            else if (kind[i] == 1) {
                int c = ref[i]; if (clients[c] != pf[i].fd) continue;
                for (int burst = 0; burst < 16 && clients[c] >= 0; burst++) {            /* drain: keeps fades/animations from lagging behind */
                    Msg m; int fd = -1; int r = proto_recv(clients[c], &m, &fd);
                    if (r <= 0) { if (r < 0 && (errno == EAGAIN)) break; drop_client(c); break; }
                    handle_message(c, &m, fd); if (fd >= 0 && m.type != M_CREATE_WINDOW && m.type != M_SET_BUFFER) close(fd);
                    struct pollfd more = { clients[c], POLLIN, 0 }; if (poll(&more, 1, 0) <= 0 || !(more.revents & POLLIN)) break;
                }
            } else {
                int di = -1; for (int k = 0; k < in.n; k++) if (in.d[k].fd == ref[i]) di = k;
                if (di < 0) continue;
                InEvent ev[256]; int ne = input_dev_read(&in.d[di], ev, 256);
                if (ne < 0 || (pf[i].revents & (POLLERR | POLLHUP)) == (POLLERR | POLLHUP)) { input_remove(&in, di); continue; }   /* unplugged: never spin on a dead fd */
                for (int k = 0; k < ne; k++) apply_input(&ev[k]);
            }
        }
        if (wm.ws != last_ws) { last_ws = wm.ws; printf("aether-compositor: workspace -> %d\n", wm.ws + 1); }   /* covers panel clicks and Alt+N */
        log_changes(); render();
    }
    printf("aether-compositor: shutting down\n");
    unlink(sockpath); if (ctlpath) unlink(ctlpath); display_close(&d); return 0;
}
