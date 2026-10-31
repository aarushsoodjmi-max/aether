/* aether-shell: the desktop shell. Three modes, one binary:
 *   aether-shell                 -> panel: launcher button, workspaces, task chips, clock, real status indicators
 *   aether-shell --launcher X Y  -> application launcher popup at (X,Y) (spawned by the panel; search + grid)
 *   aether-shell --desktop       -> desktop surface: the Aurora wallpaper and shortcuts for the installed applications
 *   aether-shell --menu X Y KIND ARG -> right-click context menu popup (spawned by the panel or the desktop surface; ctxmenu.h)
 * Applications come from the .app files in /usr/share/aether/apps (appmeta.h); only entries whose executable exists
 * are shown. The shell knows nothing about packages. */
#define _GNU_SOURCE
#include "appmeta.h"
#include "ctxmenu.h"
#include "ui.h"
#include "wallpaper.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define APP_DIR_DEFAULT "/usr/share/aether/apps"
static const char *app_dir(void) { const char *e = getenv("AETHER_APP_DIR"); return e && *e ? e : APP_DIR_DEFAULT; }   /* override is for tests */
#define MAX_APPS 24
#define LAUNCHER_PIPE_FD 9
static AppEntry apps[MAX_APPS]; static size_t napps;

/* A process is "running" an app when its executable (/proc/PID/exe) is that app's exec path: accurate, and independent of the compositor. */
static int exec_running(const char *exec)
{
    char want[PATH_MAX]; if (!realpath(exec, want)) snprintf(want, sizeof want, "%s", exec);      /* /proc/PID/exe is the resolved path (/bin is a symlink to /usr/bin) */
    DIR *d = opendir("/proc"); if (!d) return 0; int found = 0;
    for (struct dirent *e; !found && (e = readdir(d));) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char p[64], t[256]; snprintf(p, sizeof p, "/proc/%.20s/exe", e->d_name);
        ssize_t n = readlink(p, t, sizeof t - 1); if (n <= 0) continue; t[n] = 0;
        if (!strcmp(t, want)) found = 1;
    }
    closedir(d); return found;
}
static void spawn(const char *path)
{
    pid_t p = fork();
    if (p == 0) { setsid(); execl(path, path, (char *)NULL); _exit(127); }
}
static void reap_children_automatically(void) { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = SIG_DFL; sa.sa_flags = SA_NOCLDWAIT; sigaction(SIGCHLD, &sa, NULL); }

/* =========================================================================================================== panel */
typedef struct { uint32_t id; int flags; char title[48]; } Task;
static Task tasks[16]; static int ntasks, tasks_building;
static Task tasks_next[16]; static int ntasks_next;
static pid_t popup_pid; static int popup_pipe = -1, popup_is_menu;       /* the one popup this process has open: launcher or context menu; the pipe carries a menu's chosen command back */
static int launcher_btn = -1;
static char clock_text[40]; static int net_state = -1 /* -1 none, 0 down, 1 up */, battery = -1;

static int popup_open(void) { return popup_pid > 0 && popup_pipe >= 0; }
static int launcher_open(void) { return popup_open() && !popup_is_menu; }       /* drives the launcher button's "open" look */
static void read_status(void)
{
    net_state = -1; battery = -1;
    DIR *d = opendir("/sys/class/net");
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
        char p[300], v[32] = ""; snprintf(p, sizeof p, "/sys/class/net/%s/operstate", e->d_name);
        FILE *f = fopen(p, "r"); if (f) { if (fgets(v, sizeof v, f)) {} fclose(f); }
        if (!strncmp(v, "up", 2)) net_state = 1; else if (net_state < 0) net_state = 0;
    }
    if (d) closedir(d);
    d = opendir("/sys/class/power_supply");
    for (struct dirent *e; d && (e = readdir(d));) {
        if (strncmp(e->d_name, "BAT", 3)) continue;
        char p[300]; snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
        FILE *f = fopen(p, "r"); int v = -1; if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
        if (v >= 0) { battery = v; break; }
    }
    if (d) closedir(d);
}
static void close_launcher(void) { if (popup_pid > 0) kill(popup_pid, SIGTERM); }
static int popup_spawn(UiWindow *w, char *const argv[], int is_menu)     /* fork+exec this binary as a popup, with the notification pipe on fd 9 */
{
    int fds[2]; if (pipe(fds) != 0) return -1;
    pid_t p = fork();
    if (p == 0) { close(fds[0]); dup2(fds[1], LAUNCHER_PIPE_FD); if (fds[1] != LAUNCHER_PIPE_FD) close(fds[1]); execv("/proc/self/exe", argv); execv("/bin/aether-shell", argv); _exit(127); }
    close(fds[1]);
    if (p < 0) { close(fds[0]); return -1; }
    popup_pid = p; popup_pipe = fds[0]; popup_is_menu = is_menu; w->watch_fd = fds[0]; fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    return 0;
}
static void open_launcher(UiWindow *w)
{
    char x[16], y[16]; snprintf(x, sizeof x, "%d", w->wid[launcher_btn].r.x); snprintf(y, sizeof y, "%d", PANEL_HEIGHT + TH_S1);
    char *const av[] = { "aether-shell", "--launcher", x, y, NULL };
    if (popup_spawn(w, av, 0) == 0) ui_redraw(w);
}
/* Owner side of a popup's pipe. EOF: the popup process has exited. A line: a command chosen in a context menu (ctxmenu.h), run by the
 * mode that owns the menu (panel: activate/close a window; desktop: open an application / refresh). The launcher never writes. */
static void (*popup_cmd)(UiWindow *w, const CmAction *a);
static char popup_line[128]; static size_t popup_len; static int popup_overflow;
static void on_popup_watch(UiWindow *w)
{
    char tmp[64]; ssize_t n = read(popup_pipe, tmp, sizeof tmp);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return;
    if (n <= 0) { close(popup_pipe); popup_pipe = -1; popup_pid = 0; popup_is_menu = 0; popup_len = 0; popup_overflow = 0; w->watch_fd = -1; ui_redraw(w); return; }
    for (ssize_t i = 0; i < n; i++) {
        if (tmp[i] == '\n') {
            popup_line[popup_len] = 0; CmAction a;
            if (!popup_overflow && popup_cmd && cm_parse_command(popup_line, &a)) popup_cmd(w, &a);
            popup_len = 0; popup_overflow = 0;
        } else if (popup_len < sizeof popup_line - 1) popup_line[popup_len++] = tmp[i];
        else popup_overflow = 1;
    }
}
static void toggle_launcher(UiWindow *w) { if (popup_open()) close_launcher(); else open_launcher(w); }       /* Super also dismisses an open context menu */
static void launcher_cb(UiWindow *w, int widget, void *user)
{
    (void)widget; (void)user;
    if (w->press_dismissed) return;                  /* this very press already dismissed the open menu: do not reopen it */
    toggle_launcher(w);
}
static void open_app_named(const char *name)
{
    AppEntry list[MAX_APPS]; size_t n = app_load_runnable(app_dir(), list, MAX_APPS);
    for (size_t i = 0; i < n; i++) if (!strcmp(list[i].name, name)) { spawn(list[i].exec); return; }
}
static void on_hotkey(UiWindow *w, int id) { if (id == 1) toggle_launcher(w); else if (id == 2) open_app_named("Terminal"); }

static uint32_t hover_fill(const UiWidget *x, uint32_t base) { return x->pressed ? TH_SURFACE2_PRESS : (x->hover ? TH_SURFACE2_HOVER : base); }
static void draw_launcher_btn(UiWindow *w, int i, Surface *s)
{
    const UiWidget *x = &w->wid[i]; int open = launcher_open();
    if (open) { gfx_rrect(s, x->r, TH_R_CTRL, gfx_mix(TH_BORDER, TH_BLUE, 150)); gfx_rrect(s, rect_make(x->r.x + 1, x->r.y + 1, x->r.w - 2, x->r.h - 2), TH_R_CTRL - 1, gfx_mix(TH_SURFACE2, TH_BLUE, 56)); }
    else if (x->hover || x->pressed) gfx_rrect(s, x->r, TH_R_CTRL, hover_fill(x, TH_PANEL));
    icon_draw(s, ICON_AETHER, x->r.x + TH_S3, x->r.y + (x->r.h - 20) / 2, 20, IS_DEFAULT);
    font_draw_box(s, rect_make(x->r.x + TH_S3 + 20 + TH_S2, x->r.y, x->r.w, x->r.h), "Aether", font_face(FW_MEDIUM, TY_PANEL_PX + 1), TH_TEXT, FA_LEFT);
}
static void draw_ws(UiWindow *w, int i, Surface *s)
{
    const UiWidget *x = &w->wid[i]; int idx = (int)(intptr_t)x->user, active = idx == w->workspace; char n[16]; snprintf(n, sizeof n, "%d", idx + 1);
    if (active) gfx_rrect(s, x->r, TH_R_CTRL, x->pressed ? TH_BLUE_PRESS : TH_BLUE);
    else if (x->hover || x->pressed) gfx_rrect(s, x->r, TH_R_CTRL, hover_fill(x, TH_PANEL));
    font_draw_box(s, x->r, n, font_face(active ? FW_MEDIUM : FW_REGULAR, TY_PANEL_PX), active ? TH_VOID : (x->hover ? TH_TEXT : TH_TEXT2), FA_CENTER);
}
static void ws_cb(UiWindow *w, int widget, void *user) { (void)widget; ui_switch_workspace(w, (int)(intptr_t)user); }
static void draw_chip(UiWindow *w, int i, Surface *s)
{
    const UiWidget *x = &w->wid[i]; const Task *t = &tasks[(int)(intptr_t)x->user]; int focused = t->flags & 1, mini = t->flags & 2;
    if (focused) { gfx_rrect(s, x->r, TH_R_CTRL, gfx_mix(TH_BORDER, TH_BLUE, 130)); gfx_rrect(s, rect_make(x->r.x + 1, x->r.y + 1, x->r.w - 2, x->r.h - 2), TH_R_CTRL - 1, gfx_mix(TH_SURFACE2, TH_BLUE, 34)); }
    else if (x->hover || x->pressed) gfx_rrect(s, x->r, TH_R_CTRL, hover_fill(x, TH_PANEL));
    char tt[64]; const Face *f = font_face(focused ? FW_MEDIUM : FW_REGULAR, TY_PANEL_PX + 1);
    font_ellipsize(tt, sizeof tt, t->title, f, x->r.w - 2 * TH_S3);
    font_draw_box(s, rect_make(x->r.x + TH_S3, x->r.y, x->r.w - 2 * TH_S3, x->r.h), tt, f, mini ? TH_MUTED : (focused ? TH_TEXT : TH_TEXT2), FA_CENTER);
}
static void chip_cb(UiWindow *w, int widget, void *user) { (void)widget; ui_task_activate(w, tasks[(int)(intptr_t)user].id); }
static void draw_clock(UiWindow *w, int i, Surface *s)
{
    const UiWidget *x = &w->wid[i]; const Face *f = font_face(FW_MEDIUM, TY_CLOCK_PX), *g = font_face(FW_REGULAR, TY_TINY_PX);
    int tw = font_width(f, clock_text);
    font_draw_box(s, rect_make(x->r.x, x->r.y, tw, x->r.h), clock_text, f, TH_TEXT, FA_LEFT);
    font_draw_box(s, rect_make(x->r.x + tw + TH_S1, x->r.y + 1, x->r.w - tw - TH_S1, x->r.h), "UTC", g, TH_MUTED, FA_LEFT);
}
static void draw_net(UiWindow *w, int i, Surface *s) { const UiWidget *x = &w->wid[i]; icon_draw_ex(s, ICON_NETWORK, x->r.x + 1, x->r.y + 1, 18, net_state ? TH_TEXT2 : TH_MUTED, net_state ? TH_BLUE_HOVER : TH_MUTED, net_state ? -1 : 0); }
static void draw_bat(UiWindow *w, int i, Surface *s) { const UiWidget *x = &w->wid[i]; icon_draw_ex(s, ICON_BATTERY, x->r.x, x->r.y + 1, 20, TH_TEXT2, TH_TEXT2, battery); }

static void build_panel(UiWindow *w)
{
    ui_clear(w);
    const Face *fm = font_face(FW_MEDIUM, TY_PANEL_PX + 1), *fc = font_face(FW_MEDIUM, TY_CLOCK_PX);
    int by = (PANEL_HEIGHT - 1 - TH_CTRL_H) / 2 + 1, x = TH_PANEL_PAD;
    ui_separator(w, rect_make(0, PANEL_HEIGHT - 1, w->w, 1), UI_ALL_PAGES);
    int lw = TH_S3 + 20 + TH_S2 + font_width(fm, "Aether") + TH_S3;
    launcher_btn = ui_custom(w, rect_make(x, by, lw, TH_CTRL_H), draw_launcher_btn, launcher_cb, NULL, 1, UI_ALL_PAGES); x += lw + TH_S3;
    for (int i = 0; i < AETHER_WORKSPACES; i++) { ui_custom(w, rect_make(x, by + (TH_CTRL_H - TH_WS_BTN) / 2, TH_WS_BTN, TH_WS_BTN), draw_ws, ws_cb, (void *)(intptr_t)i, 1, UI_ALL_PAGES); x += TH_WS_BTN + TH_S1; }
    int left_end = x + TH_S3;
    /* right cluster, laid out from the right edge */
    int cw = font_width(fc, clock_text) + TH_S1 + font_width(font_face(FW_REGULAR, TY_TINY_PX), "UTC"), rx = w->w - TH_PANEL_PAD - cw;
    ui_custom(w, rect_make(rx, by, cw, TH_CTRL_H), draw_clock, NULL, NULL, 0, UI_ALL_PAGES);
    if (battery >= 0) { rx -= TH_S4 + 22; ui_custom(w, rect_make(rx, by + 5, 22, 22), draw_bat, NULL, NULL, 0, UI_ALL_PAGES); }
    if (net_state >= 0) { rx -= TH_S4 + 20; ui_custom(w, rect_make(rx, by + 5, 20, 22), draw_net, NULL, NULL, 0, UI_ALL_PAGES); }
    /* centre: one chip per window on this workspace, centred in the free space */
    int x0 = left_end, x1 = rx - TH_S4, n = ntasks, free_w = x1 - x0;
    if (n > 0 && free_w > 60) {
        int cwid = th_min(180, (free_w - (n - 1) * TH_S1) / n); while (cwid < 72 && n > 1) { n--; cwid = th_min(180, (free_w - (n - 1) * TH_S1) / n); }
        int total = n * cwid + (n - 1) * TH_S1, cx = x0 + (free_w - total) / 2;
        for (int i = 0; i < n; i++) { ui_custom(w, rect_make(cx, by + 2, cwid, TH_CTRL_H - 4), draw_chip, chip_cb, (void *)(intptr_t)i, 1, UI_ALL_PAGES); cx += cwid + TH_S1; }
    }
}
static void format_clock(void)
{
    time_t t = time(NULL); struct tm tm; gmtime_r(&t, &tm); strftime(clock_text, sizeof clock_text, "%a %d %b  %H:%M", &tm);
}
static void panel_tick(UiWindow *w)
{
    static int n; char old[40]; snprintf(old, sizeof old, "%s", clock_text); int ons = net_state, obat = battery;
    format_clock(); if (++n % 5 == 0) read_status();
    if (strcmp(old, clock_text) || ons != net_state || obat != battery) { build_panel(w); ui_redraw(w); }
}
static void panel_task(UiWindow *w, int phase, uint32_t id, int flags, const char *title)
{
    if (phase == 0) { ntasks_next = 0; tasks_building = 1; }
    else if (phase == 1 && tasks_building && ntasks_next < 16) { tasks_next[ntasks_next].id = id; tasks_next[ntasks_next].flags = flags; snprintf(tasks_next[ntasks_next].title, sizeof tasks_next[0].title, "%s", title); ntasks_next++; }
    else if (phase == 2 && tasks_building) { memcpy(tasks, tasks_next, sizeof tasks); ntasks = ntasks_next; tasks_building = 0; build_panel(w); ui_redraw(w); }
}
static void panel_screen(UiWindow *w)
{
    char err[96]; if (ui_resize(w, w->screen_w, PANEL_HEIGHT, err, sizeof err) != 0) return;
    build_panel(w); ui_redraw(w);
}
static int open_menu(UiWindow *w, int x, int y, CmKind kind, const char *arg)       /* spawn the context-menu popup for a click at screen (x, y) */
{
    if (popup_open()) return -1;                                                     /* one popup at a time */
    char xs[16], ys[16]; snprintf(xs, sizeof xs, "%d", x); snprintf(ys, sizeof ys, "%d", y);
    char *const av[] = { "aether-shell", "--menu", xs, ys, (char *)cm_kind_name(kind), (char *)arg, NULL };
    return popup_spawn(w, av, 1);
}
static void panel_context(UiWindow *w, int widget, int x, int y)                    /* right-click on a task chip: Activate / Close */
{
    (void)y;
    if (widget < 0 || w->wid[widget].draw != draw_chip) return;
    int t = (int)(intptr_t)w->wid[widget].user; if (t < 0 || t >= ntasks) return;
    char id[16]; snprintf(id, sizeof id, "%u", (unsigned)tasks[t].id);
    open_menu(w, x, PANEL_HEIGHT + TH_S1, CM_TASK, id);
}
static void panel_command(UiWindow *w, const CmAction *a)                            /* a command chosen in the task menu; only the panel may address windows */
{
    if (a->kind == CM_ACT_ACTIVATE) ui_task_raise(w, a->id);
    else if (a->kind == CM_ACT_CLOSE) ui_task_close(w, a->id);
}
static int run_panel(void)
{
    UiWindow w; char err[128]; reap_children_automatically();
    if (ui_connect(&w, err, sizeof err) != 0) { fprintf(stderr, "aether-shell: %s\n", err); return 1; }
    w.bg = TH_PANEL;
    if (ui_create(&w, "panel", WK_PANEL, w.screen_w, PANEL_HEIGHT, 0, 0, err, sizeof err) != 0) { fprintf(stderr, "aether-shell: %s\n", err); return 1; }
    format_clock(); read_status(); build_panel(&w);
    w.on_tick = panel_tick; w.tick_ms = 1000; w.on_task = panel_task; w.on_hotkey = on_hotkey; w.on_watch = on_popup_watch; w.on_screen = panel_screen; w.on_context = panel_context; popup_cmd = panel_command;
    printf("aether-shell: panel running on %dx%d screen\n", w.screen_w, w.screen_h);
    ui_run(&w); ui_destroy(&w); return 0;
}

/* ====================================================================================================== launcher */
#define LCOLS 4
static int filtered[MAX_APPS], nfiltered, sel, tile_w[MAX_APPS], search_w = -1, grid_top, grid_rows_vis;
static long open_ms; static int running[MAX_APPS]; static long run_ms;
static void refresh_running(void) { for (size_t i = 0; i < napps; i++) running[i] = exec_running(apps[i].exec); run_ms = ui_now_ms(); }
static int match(const AppEntry *a, const char *q)
{
    if (!*q) return 1;
    char hay[200]; snprintf(hay, sizeof hay, "%.47s %.95s", a->name, a->comment);
    for (char *p = hay; *p; p++) *p = (char)tolower((unsigned char)*p);
    char lq[96]; snprintf(lq, sizeof lq, "%s", q); for (char *p = lq; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strstr(hay, lq) != NULL;
}
static void launch_app(UiWindow *w, int app) { spawn(apps[app].exec); w->closed = 1; }
static void place_tiles(UiWindow *w)
{
    for (size_t i = 0; i < napps; i++) w->wid[tile_w[i]].r = rect_make(0, 0, 0, 0);
    int cell = (w->w - 2 * TH_S4) / LCOLS;
    for (int k = 0; k < nfiltered; k++) {
        int col = k % LCOLS, row = k / LCOLS; UiWidget *t = &w->wid[tile_w[filtered[k]]];
        t->r = row < grid_rows_vis ? rect_make(TH_S4 + col * cell + (cell - TH_TILE_W) / 2, grid_top + row * (TH_TILE_H + TH_S2), TH_TILE_W, TH_TILE_H) : rect_make(0, 0, 0, 0);
    }
}
static void refilter(UiWindow *w, int widget, void *user)
{
    (void)widget; (void)user; const char *q = w->wid[search_w].text; nfiltered = 0;
    for (size_t i = 0; i < napps; i++) if (match(&apps[i], q)) filtered[nfiltered++] = (int)i;
    sel = 0; place_tiles(w);
}
static void draw_tile(UiWindow *w, int i, Surface *s)
{
    const UiWidget *x = &w->wid[i]; int app = (int)(intptr_t)x->user, k = -1;
    if (x->r.w == 0) return;
    for (int j = 0; j < nfiltered; j++) if (filtered[j] == app) k = j;
    int on = k == sel;
    if (on) { gfx_rrect(s, x->r, TH_R_MENU, gfx_mix(TH_BORDER, TH_BLUE, 200)); gfx_rrect(s, rect_make(x->r.x + 1, x->r.y + 1, x->r.w - 2, x->r.h - 2), TH_R_MENU - 1, gfx_mix(TH_WINDOW, TH_BLUE, 36)); }
    Rect box = rect_make(x->r.x + (x->r.w - TH_TILE_ICON) / 2, x->r.y + TH_S2, TH_TILE_ICON, TH_TILE_ICON);
    icon_app_tile(s, box, icon_by_name(apps[app].icon), apps[app].name, on || x->hover, 0);
    char t[48]; const Face *f = font_face(on ? FW_MEDIUM : FW_REGULAR, TY_APP_PX); font_ellipsize(t, sizeof t, apps[app].name, f, x->r.w - TH_S2);
    font_draw_box(s, rect_make(x->r.x, box.y + box.h + TH_S2, x->r.w, 18), t, f, on ? TH_TEXT : TH_TEXT2, FA_CENTER);
    if (running[app]) gfx_disc(s, (float)(x->r.x + x->r.w / 2), (float)(x->r.y + x->r.h - 4), 2.5f, TH_CYAN, 255);        /* running indicator */
}
static void tile_cb(UiWindow *w, int widget, void *user) { (void)widget; launch_app(w, (int)(intptr_t)user); }
static void tile_hover(UiWindow *w, int widget)
{
    (void)w; for (int j = 0; j < nfiltered; j++) if (widget == tile_w[filtered[j]]) sel = j;
}
static void draw_empty(UiWindow *w, int i, Surface *s)
{
    const UiWidget *x = &w->wid[i]; if (nfiltered > 0) return;
    font_draw_box(s, rect_make(x->r.x, x->r.y, x->r.w, TH_TILE_H), napps ? "No matching applications" : "No applications installed", font_face(FW_REGULAR, TY_BODY_PX), TH_TEXT2, FA_CENTER);
}
static void launcher_key(UiWindow *w, int key, int ascii, int pressed)
{
    (void)ascii; if (!pressed) return;
    int rows_move = 0, cols_move = 0;
    switch (key) {
    case KEY_ESC: w->closed = 1; return;
    case KEY_ENTER: case KEY_KPENTER: if (nfiltered > 0) launch_app(w, filtered[sel]); return;
    case KEY_LEFT: cols_move = -1; break;
    case KEY_RIGHT: cols_move = 1; break;
    case KEY_UP: rows_move = -1; break;
    case KEY_DOWN: rows_move = 1; break;
    default: return;
    }
    if (nfiltered == 0) return;
    int n = sel + cols_move + rows_move * LCOLS; if (n >= 0 && n < nfiltered) sel = n;
    ui_redraw(w);
}
static void launcher_tick(UiWindow *w)
{
    long e = ui_now_ms() - open_ms;
    if (e < TH_ANIM_MENU_MS) { float t = (float)e / (float)TH_ANIM_MENU_MS; t = 1.0f - (1.0f - t) * (1.0f - t); ui_set_opacity(w, (int)(255.0f * t)); }
    else { if (w->tick_ms != 530) { ui_set_opacity(w, 255); w->tick_ms = 530; } if (ui_now_ms() - run_ms > 1000) refresh_running(); ui_redraw(w); }        /* afterwards: only the caret blinks */
}
static int run_launcher(int x, int y)
{
    UiWindow w; char err[128]; fcntl(LAUNCHER_PIPE_FD, F_SETFD, FD_CLOEXEC);          /* held only while we run; apps we start must not inherit it */
    napps = app_load_runnable(app_dir(), apps, MAX_APPS); refresh_running();
    if (ui_connect(&w, err, sizeof err) != 0) { fprintf(stderr, "aether-shell: %s\n", err); return 1; }
    int lw = th_min(TH_LAUNCHER_W, w.screen_w - 2 * TH_S2), maxh = th_min(TH_LAUNCHER_H, w.screen_h - PANEL_HEIGHT - 2 * TH_S2);
    int cell_h = TH_TILE_H + TH_S2, head = TH_S4 + TH_SEARCH_H + TH_S3 + 18 + TH_S1, foot = TH_S3 + 14 + TH_S4;
    int rows = ((int)napps + LCOLS - 1) / LCOLS; if (rows < 1) rows = 1;
    int lh = head + rows * cell_h + foot; if (lh > maxh) { rows = th_max(1, (maxh - head - foot) / cell_h); lh = head + rows * cell_h + foot; }
    grid_rows_vis = rows; grid_top = head;
    x = th_clamp(x, TH_S2, w.screen_w - lw - TH_S2); y = th_clamp(y, PANEL_HEIGHT, w.screen_h - lh);
    w.bg = TH_WINDOW; w.flags = WF_TRANSPARENT;
    if (ui_create(&w, "launcher", WK_POPUP, lw, lh, x, y, err, sizeof err) != 0) { fprintf(stderr, "aether-shell: %s\n", err); return 1; }
    search_w = ui_entry(&w, rect_make(TH_S4, TH_S4, lw - 2 * TH_S4, TH_SEARCH_H), "Search applications", refilter, NULL, UI_ALL_PAGES);
    ui_label_ex(&w, TH_S4 + 2, TH_S4 + TH_SEARCH_H + TH_S3, "Applications", TH_TEXT2, FW_MEDIUM, TY_SMALL_PX, UI_ALL_PAGES);
    ui_custom(&w, rect_make(TH_S4, head, lw - 2 * TH_S4, cell_h * rows), draw_empty, NULL, NULL, 0, UI_ALL_PAGES);
    for (size_t i = 0; i < napps; i++) tile_w[i] = ui_custom(&w, rect_make(0, 0, 0, 0), draw_tile, tile_cb, (void *)(intptr_t)i, 1, UI_ALL_PAGES);
    ui_label_ex(&w, TH_S4 + 2, lh - TH_S4 - 14, "Enter to open   Esc to close", TH_MUTED, FW_REGULAR, TY_TINY_PX, UI_ALL_PAGES);
    w.focus = search_w; refilter(&w, search_w, NULL);
    w.on_key = launcher_key; w.on_hover = tile_hover; w.on_tick = launcher_tick; w.tick_ms = TH_FRAME_MS; open_ms = ui_now_ms();
    printf("aether-shell: launcher menu with %zu application(s)\n", napps);
    ui_run(&w); ui_destroy(&w); return 0;
}

/* ================================================================================================== context menu popup */
static CmItem mitems[CM_MAX_ITEMS]; static size_t mn; static int msel = -1; static long menu_open_ms;
static void menu_choose(UiWindow *w, int i)
{
    if (i < 0 || i >= (int)mn || mitems[i].separator) return;
    char line[sizeof mitems[0].cmd + 2]; int n = snprintf(line, sizeof line, "%s\n", mitems[i].cmd);
    if (n > 0 && write(LAUNCHER_PIPE_FD, line, (size_t)n) < 0) fprintf(stderr, "aether-shell: menu: owner is gone\n");    /* the owner performs the action */
    w->closed = 1;
}
static void draw_menu_item(UiWindow *w, int i, Surface *s)
{
    const UiWidget *x = &w->wid[i]; int k = (int)(intptr_t)x->user;
    if (k == msel || x->pressed) gfx_rrect(s, x->r, TH_R_CTRL, x->pressed ? TH_SURFACE2_PRESS : TH_SURFACE2_HOVER);
    font_draw_box(s, rect_make(x->r.x + TH_S3, x->r.y, x->r.w - 2 * TH_S3, x->r.h), mitems[k].label, font_face(FW_REGULAR, TY_BODY_PX), k == msel ? TH_TEXT : TH_TEXT2, FA_LEFT);
}
static void menu_item_cb(UiWindow *w, int widget, void *user) { (void)widget; menu_choose(w, (int)(intptr_t)user); }
static void menu_hover(UiWindow *w, int widget) { msel = (int)(intptr_t)w->wid[widget].user; }
static void menu_pointer(UiWindow *w, int kind, int x, int y, int button, int clicks)
{
    (void)x; (void)y; (void)button; (void)clicks;
    if (kind == 4 && msel != -1) { msel = -1; ui_redraw(w); }                          /* the pointer left the menu: no row stays highlighted */
}
static void menu_key(UiWindow *w, int key, int ascii, int pressed)
{
    (void)ascii; if (!pressed) return;
    switch (key) {
    case KEY_ESC: w->closed = 1; return;
    case KEY_ENTER: case KEY_KPENTER: menu_choose(w, msel); return;
    case KEY_UP: msel = cm_step(mitems, mn, msel, -1); break;
    case KEY_DOWN: msel = cm_step(mitems, mn, msel, 1); break;
    default: return;
    }
    ui_redraw(w);
}
static void menu_tick(UiWindow *w)
{
    long e = ui_now_ms() - menu_open_ms;
    if (e < TH_ANIM_MENU_MS) { float t = (float)e / (float)TH_ANIM_MENU_MS; t = 1.0f - (1.0f - t) * (1.0f - t); ui_set_opacity(w, (int)(255.0f * t)); }
    else { ui_set_opacity(w, 255); w->tick_ms = 0; }
}
static int run_menu(int x, int y, const char *kindname, const char *arg)
{
    UiWindow w; char err[128]; CmKind kind; signal(SIGPIPE, SIG_IGN); fcntl(LAUNCHER_PIPE_FD, F_SETFD, FD_CLOEXEC);
    if (!cm_kind_parse(kindname, &kind)) { fprintf(stderr, "aether-shell: unknown menu kind '%s'\n", kindname); return 1; }
    AppEntry list[MAX_APPS]; const char *names[MAX_APPS]; size_t nl = app_load_runnable(app_dir(), list, MAX_APPS);
    for (size_t i = 0; i < nl; i++) names[i] = list[i].name;
    mn = cm_build(kind, arg, names, nl, mitems);
    if (mn == 0) { fprintf(stderr, "aether-shell: nothing to show in the %s menu\n", kindname); return 1; }
    if (ui_connect(&w, err, sizeof err) != 0) { fprintf(stderr, "aether-shell: %s\n", err); return 1; }
    int mw = TH_MENU_W, mh = cm_menu_h(mitems, mn); cm_place(x, y, mw, mh, w.screen_w, w.screen_h, PANEL_HEIGHT, &x, &y);
    w.bg = TH_WINDOW; w.flags = WF_TRANSPARENT;
    if (ui_create(&w, "context-menu", WK_POPUP, mw, mh, x, y, err, sizeof err) != 0) { fprintf(stderr, "aether-shell: %s\n", err); return 1; }
    for (size_t i = 0; i < mn; i++) {
        Rect r = rect_make(TH_MENU_PAD, cm_item_y(mitems, i), mw - 2 * TH_MENU_PAD, cm_item_h(&mitems[i]));
        if (mitems[i].separator) ui_separator(&w, rect_make(r.x + TH_S2, r.y + r.h / 2, r.w - 2 * TH_S2, 1), UI_ALL_PAGES);
        else ui_custom(&w, r, draw_menu_item, menu_item_cb, (void *)(intptr_t)i, 1, UI_ALL_PAGES);
    }
    w.on_key = menu_key; w.on_hover = menu_hover; w.on_pointer = menu_pointer; w.on_tick = menu_tick; w.tick_ms = TH_FRAME_MS; menu_open_ms = ui_now_ms();
    printf("aether-shell: context menu (%s) with %zu item(s) at %d,%d\n", cm_kind_name(kind), mn, x, y);
    ui_run(&w); ui_destroy(&w); return 0;
}

/* ======================================================================================================= desktop */
static Surface wall; static int dsel = -1, dlast = -1;
static void draw_wall(UiWindow *w, int i, Surface *s) { (void)w; (void)i; gfx_blit(s, 0, 0, &wall, rect_make(0, 0, wall.w, wall.h)); }
static void wall_cb(UiWindow *w, int widget, void *user) { (void)widget; (void)user; if (dsel != -1) { dsel = -1; ui_redraw(w); } }
static void draw_shortcut(UiWindow *w, int i, Surface *s)
{
    const UiWidget *x = &w->wid[i]; int app = (int)(intptr_t)x->user, on = dsel == app;
    if (on) gfx_rrect_a(s, x->r, TH_R_MENU, TH_BLUE, 84); else if (x->hover) gfx_rrect_a(s, x->r, TH_R_MENU, TH_TEXT, 22);
    Rect box = rect_make(x->r.x + (x->r.w - TH_DESK_ICON) / 2, x->r.y + TH_S2, TH_DESK_ICON, TH_DESK_ICON);
    icon_app_tile(s, box, icon_by_name(apps[app].icon), apps[app].name, x->hover, on);
    char t[48]; const Face *f = font_face(FW_REGULAR, TY_APP_PX); font_ellipsize(t, sizeof t, apps[app].name, f, x->r.w - TH_S2);
    Rect lab = rect_make(x->r.x, box.y + box.h + TH_S2, x->r.w, 18);
    font_draw_box(s, rect_make(lab.x, lab.y + 1, lab.w, lab.h), t, f, TH_VOID, FA_CENTER);        /* soft shadow keeps labels legible on bright ribbons */
    font_draw_box(s, lab, t, f, TH_TEXT, FA_CENTER);
}
static void shortcut_cb(UiWindow *w, int widget, void *user)
{
    (void)widget; int app = (int)(intptr_t)user;
    if (w->clicks >= 2) { spawn(apps[app].exec); dlast = -1; }                                      /* double-click (counted by the compositor) launches */
    else dlast = app;
    dsel = app; ui_redraw(w);
}
static int build_desktop(UiWindow *w)
{
    Surface nw = { calloc((size_t)w->w * (size_t)w->h, 4), w->w, w->h, w->w };         /* allocate first: a failed rebuild keeps the old wallpaper */
    if (!nw.px) return -1;
    free(wall.px); wall = nw;
    wallpaper_render(&wall); ui_clear(w);
    ui_custom(w, rect_make(0, 0, w->w, w->h), draw_wall, wall_cb, NULL, 1, UI_ALL_PAGES);
    int perCol = th_max(1, (w->h - TH_DESK_MARGIN) / TH_DESK_CELL_H);
    for (size_t i = 0; i < napps; i++) {
        int col = (int)i / perCol, row = (int)i % perCol;
        ui_custom(w, rect_make(TH_DESK_MARGIN + col * (TH_DESK_CELL_W + TH_S2), TH_DESK_MARGIN + row * TH_DESK_CELL_H, TH_DESK_CELL_W, TH_DESK_CELL_H - TH_S2), draw_shortcut, shortcut_cb, (void *)(intptr_t)i, 1, UI_ALL_PAGES);
    }
    return 0;
}
static void desktop_refresh(UiWindow *w)                    /* re-read the installed applications and redraw the wallpaper and shortcuts */
{
    napps = app_load_runnable(app_dir(), apps, MAX_APPS); dsel = dlast = -1;
    if (build_desktop(w) != 0) { fprintf(stderr, "aether-shell: desktop refresh failed (out of memory); keeping the current desktop\n"); return; }
    ui_redraw(w); printf("aether-shell: desktop refreshed with %zu shortcut(s)\n", napps);
}
static void desktop_context(UiWindow *w, int widget, int x, int y)       /* right-click: a shortcut gets Open, anywhere else the desktop menu */
{
    int app = widget >= 0 && w->wid[widget].draw == draw_shortcut ? (int)(intptr_t)w->wid[widget].user : -1;
    if (app >= 0 && (size_t)app < napps) { dsel = app; ui_redraw(w); open_menu(w, x, y + PANEL_HEIGHT, CM_SHORTCUT, apps[app].name); }     /* the desktop window sits below the panel: add its offset */
    else open_menu(w, x, y + PANEL_HEIGHT, CM_DESKTOP, "");
}
static void desktop_command(UiWindow *w, const CmAction *a)
{
    if (a->kind == CM_ACT_OPEN) open_app_named(a->name);
    else if (a->kind == CM_ACT_REFRESH) desktop_refresh(w);
}
static void desktop_screen(UiWindow *w)
{
    char err[96]; if (ui_resize(w, w->screen_w, w->screen_h - PANEL_HEIGHT, err, sizeof err) != 0) return;
    build_desktop(w); ui_redraw(w);
}
static int run_desktop(void)
{
    UiWindow w; char err[128]; reap_children_automatically(); napps = app_load_runnable(app_dir(), apps, MAX_APPS);
    if (ui_connect(&w, err, sizeof err) != 0) { fprintf(stderr, "aether-shell: %s\n", err); return 1; }
    if (ui_create(&w, "desktop", WK_DESKTOP, w.screen_w, w.screen_h - PANEL_HEIGHT, 0, PANEL_HEIGHT, err, sizeof err) != 0) { fprintf(stderr, "aether-shell: %s\n", err); return 1; }
    if (build_desktop(&w) != 0) return 1;
    w.on_screen = desktop_screen; w.on_context = desktop_context; w.on_watch = on_popup_watch; popup_cmd = desktop_command;
    printf("aether-shell: desktop surface %dx%d with %zu shortcut(s)\n", w.w, w.h, napps);
    ui_run(&w); ui_destroy(&w); return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 4 && !strcmp(argv[1], "--launcher")) return run_launcher(atoi(argv[2]), atoi(argv[3]));
    if (argc == 2 && !strcmp(argv[1], "--desktop")) return run_desktop();
    if (argc == 6 && !strcmp(argv[1], "--menu")) return run_menu(atoi(argv[2]), atoi(argv[3]), argv[4], argv[5]);
    return run_panel();
}
