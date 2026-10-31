/* aether-settings: system settings application built on the Aether UI toolkit and the Aurora design system.
 * Pages: About, Display, Packages, Notifications, Appearance. Reads live system data; changes nothing it does not say.
 * Resizable: maximising re-runs the layout at the new size. */
#define _GNU_SOURCE
#include "ui.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

enum { PG_ABOUT, PG_DISPLAY, PG_PACKAGES, PG_NOTIFY, PG_APPEARANCE, PG_COUNT };
static const char *const kNames[PG_COUNT] = { "About", "Display", "Packages", "Notifications", "Appearance" };
static const IconId kIcons[PG_COUNT] = { ICON_INFO, ICON_DISPLAY, ICON_PACKAGE, ICON_BELL, ICON_SETTINGS };
static const char *const kDesc[PG_COUNT] = {
    "Information about this Aether system.", "Display and workspace information reported by the compositor.", "Packages installed by aether-pm.",
    "Notifications are shown by aether-notify.", "The Aether Aurora design system. Read-only: choosing themes is not implemented yet." };
static int notify_on = 1, notify_toggle = -1;

static void nav_cb(UiWindow *w, int widget, void *user) { (void)widget; ui_set_page(w, (int)(intptr_t)user); }
static long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r"); char line[128]; long v = -1; size_t kl = strlen(key); if (!f) return -1;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, kl) && line[kl] == ':') { v = strtol(line + kl + 1, NULL, 10); break; }
    fclose(f); return v;
}
static void test_notify_cb(UiWindow *w, int widget, void *user)
{
    (void)widget; (void)user;
    if (!w->wid[notify_toggle].checked) return;                                      /* the toggle really gates the test notification */
    if (fork() == 0) { setsid(); execl("/bin/aether-notify", "aether-notify", "Settings", "This is a test notification.", "4000", "info", (char *)NULL); _exit(127); }
}
static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* ---- custom-drawn pieces ---- */
static void draw_sidebar(UiWindow *w, int i, Surface *s) { const UiWidget *x = &w->wid[i]; gfx_fill(s, x->r, TH_PANEL); gfx_fill(s, rect_make(x->r.x + x->r.w - 1, x->r.y, 1, x->r.h), TH_BORDER); }
static void draw_nav(UiWindow *w, int i, Surface *s)
{
    const UiWidget *x = &w->wid[i]; int pg = (int)(intptr_t)x->user, on = w->page == pg;
    if (on) gfx_rrect(s, x->r, TH_R_CTRL, TH_SURFACE2); else if (x->hover || x->pressed) gfx_rrect(s, x->r, TH_R_CTRL, x->pressed ? TH_SURFACE2_PRESS : gfx_mix(TH_PANEL, TH_SURFACE2, 150));
    if (on) gfx_rrect(s, rect_make(x->r.x + 2, x->r.y + 9, 3, x->r.h - 18), 1, TH_BLUE);
    icon_draw(s, kIcons[pg], x->r.x + TH_S3 + 2, x->r.y + (x->r.h - 18) / 2, 18, on ? IS_ACTIVE : (x->hover ? IS_HOVER : IS_DEFAULT));
    font_draw_box(s, rect_make(x->r.x + TH_S3 + 2 + 18 + TH_S2, x->r.y, x->r.w, x->r.h), x->text, font_face(on ? FW_MEDIUM : FW_REGULAR, TY_BODY_PX), on ? TH_TEXT : TH_TEXT2, FA_LEFT);
}
static void draw_card(UiWindow *w, int i, Surface *s)
{
    const UiWidget *x = &w->wid[i]; gfx_rrect(s, x->r, TH_R_MENU, TH_BORDER);
    gfx_rrect(s, rect_make(x->r.x + 1, x->r.y + 1, x->r.w - 2, x->r.h - 2), TH_R_MENU - 1, gfx_mix(TH_WINDOW, TH_SURFACE2, 170));
}
static void draw_swatches(UiWindow *w, int i, Surface *s)
{
    static const struct { const char *n; uint32_t c; } sw[] = { { "Void", TH_VOID }, { "Desktop", TH_DESKTOP }, { "Panel", TH_PANEL }, { "Window", TH_WINDOW }, { "Surface", TH_SURFACE2 }, { "Border", TH_BORDER }, { "Blue", TH_BLUE }, { "Cyan", TH_CYAN } };
    const UiWidget *x = &w->wid[i]; int n = (int)(sizeof sw / sizeof sw[0]), pitch = x->r.w / n;
    for (int k = 0; k < n; k++) {
        Rect r = rect_make(x->r.x + k * pitch, x->r.y, 40, 40); gfx_rrect(s, r, TH_R_CTRL, TH_BORDER); gfx_rrect(s, rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2), TH_R_CTRL - 1, sw[k].c);
        char hex[12]; snprintf(hex, sizeof hex, "#%06X", sw[k].c);
        font_draw_box(s, rect_make(r.x, r.y + 44, pitch, 16), sw[k].n, font_face(FW_REGULAR, TY_TINY_PX), TH_TEXT2, FA_LEFT);
        font_draw_box(s, rect_make(r.x, r.y + 58, pitch, 16), hex, font_face(FW_REGULAR, TY_TINY_PX), TH_MUTED, FA_LEFT);
    }
}

/* One group of rows: a card with a label on the left, a value on the right, hairlines between rows. */
static int group(UiWindow *w, int page, int x, int y, int cw, int n, const char *const *label, const char *const *value)
{
    ui_custom(w, rect_make(x, y, cw, n * TH_ROW_H), draw_card, NULL, NULL, 0, page);
    const Face *f = font_face(FW_REGULAR, TY_BODY_PX);
    for (int r = 0; r < n; r++) {
        int ry = y + r * TH_ROW_H, ty = ry + (TH_ROW_H - font_line_h(f)) / 2;
        if (r) ui_separator(w, rect_make(x + TH_S4, ry, cw - 2 * TH_S4, 1), page);
        ui_label(w, x + TH_S4, ty, label[r], TH_TEXT, page);
        if (value && value[r]) {
            char v[96]; font_ellipsize(v, sizeof v, value[r], f, cw - 2 * TH_S4 - font_width(f, label[r]) - TH_S6);
            ui_label(w, x + cw - TH_S4 - font_width(f, v), ty, v, TH_TEXT2, page);
        }
    }
    return y + n * TH_ROW_H;
}

static void build(UiWindow *w)
{
    char buf[160]; ui_clear(w); int sb = w->w < 560 ? 150 : TH_SIDEBAR_W;
    ui_custom(w, rect_make(0, 0, sb, w->h), draw_sidebar, NULL, NULL, 0, UI_ALL_PAGES);
    for (int i = 0; i < PG_COUNT; i++) { int n = ui_custom(w, rect_make(TH_S2, TH_NAV_TOP + i * TH_NAV_PITCH, sb - 2 * TH_S2, TH_NAV_H), draw_nav, nav_cb, (void *)(intptr_t)i, 1, UI_ALL_PAGES); snprintf(w->wid[n].text, sizeof w->wid[n].text, "%s", kNames[i]); }
    int x = sb + TH_WIN_PAD, cw = w->w - sb - 2 * TH_WIN_PAD, top = TH_WIN_PAD + 4;
    for (int p = 0; p < PG_COUNT; p++) {
        ui_label_ex(w, x, top, kNames[p], TH_TEXT, FW_MEDIUM, TY_H1_PX, p);
        char lines[2][96]; int nl = font_wrap(kDesc[p], font_face(FW_REGULAR, TY_SMALL_PX), cw, lines, 2);
        for (int l = 0; l < nl; l++) ui_label_ex(w, x, top + 32 + l * 17, lines[l], TH_TEXT2, FW_REGULAR, TY_SMALL_PX, p);
    }
    int gy = top + 32 + 2 * 17 + TH_S4;
    /* About */
    struct utsname u; uname(&u); char host[64] = "?"; gethostname(host, sizeof host);
    long tot = meminfo_kb("MemTotal"), av = meminfo_kb("MemAvailable");
    char kern[160], mem[96]; snprintf(kern, sizeof kern, "%.63s %.63s", u.sysname, u.release); snprintf(mem, sizeof mem, "%ld MiB total, %ld MiB available", tot / 1024, av / 1024);
    { const char *l[] = { "Operating system", "Kernel", "Host name", "Architecture", "Memory" }, *v[] = { "Aether OS (prototype)", kern, host, u.machine, mem }; group(w, PG_ABOUT, x, gy, cw, 5, l, v); }
    /* Display */
    char drv[64] = "unknown", tgt[128]; ssize_t n = readlink("/sys/class/drm/card0/device/driver", tgt, sizeof tgt - 1);
    if (n > 0) { tgt[n] = 0; const char *b = strrchr(tgt, '/'); snprintf(drv, sizeof drv, "%.63s", b ? b + 1 : tgt); }
    char res[48], wsn[64]; snprintf(res, sizeof res, "%d x %d", w->screen_w, w->screen_h); snprintf(wsn, sizeof wsn, "%d (Alt+1 to Alt+%d)", w->nws, w->nws);
    { const char *l[] = { "Resolution", "GPU driver", "Workspaces", "Rendering" }, *v[] = { res, drv, wsn, "Software (KMS dumb buffer)" }; group(w, PG_DISPLAY, x, gy, cw, 4, l, v); }
    /* Packages */
    char *pk[64]; int np = 0; DIR *d = opendir("/var/lib/aether/pm/db"); struct dirent *e;
    while (d && (e = readdir(d)) && np < 64) { size_t l = strlen(e->d_name); if (l > 5 && !strcmp(e->d_name + l - 5, ".meta")) { pk[np] = strndup(e->d_name, l - 5); np++; } }
    if (d) closedir(d);
    qsort(pk, (size_t)np, sizeof *pk, cmp);
    int avail = (w->h - gy - TH_S4) / TH_ROW_H, shown = th_clamp(np, 0, th_max(0, avail - 1 - (np > avail - 1 ? 1 : 0)));
    snprintf(buf, sizeof buf, "%d package%s", np, np == 1 ? "" : "s");
    { const char *l[22]; const char *v[22]; int k = 0; l[k] = "Installed"; v[k++] = buf;
      char more[48]; for (int i = 0; i < shown && k < 20; i++) { l[k] = pk[i]; v[k++] = ""; }
      if (np > shown && np) { snprintf(more, sizeof more, "and %d more", np - shown); l[k] = more; v[k++] = ""; }
      if (np == 0) { l[k] = "No packages installed"; v[k++] = ""; }
      group(w, PG_PACKAGES, x, gy, cw, k, l, v); }
    /* Notifications */
    ui_custom(w, rect_make(x, gy, cw, 2 * TH_ROW_H), draw_card, NULL, NULL, 0, PG_NOTIFY);
    const Face *bf = font_face(FW_REGULAR, TY_BODY_PX); int ty = (TH_ROW_H - font_line_h(bf)) / 2;
    ui_label(w, x + TH_S4, gy + ty, "Show the test notification", TH_TEXT, PG_NOTIFY);
    notify_toggle = ui_toggle(w, x + cw - TH_S4 - 36, gy + (TH_ROW_H - 20) / 2, "", notify_on, NULL, NULL, PG_NOTIFY); w->wid[notify_toggle].r.w = 36;
    ui_separator(w, rect_make(x + TH_S4, gy + TH_ROW_H, cw - 2 * TH_S4, 1), PG_NOTIFY);
    ui_label(w, x + TH_S4, gy + TH_ROW_H + ty, "Send a sample notification", TH_TEXT, PG_NOTIFY);
    ui_button(w, rect_make(x + cw - TH_S4 - 120, gy + TH_ROW_H + (TH_ROW_H - TH_CTRL_H) / 2, 120, TH_CTRL_H), "Send test", test_notify_cb, NULL, PG_NOTIFY);
    ui_label_ex(w, x, gy + 2 * TH_ROW_H + TH_S3, "The switch gates the test above. A system-wide notification policy is not implemented yet.", TH_MUTED, FW_REGULAR, TY_TINY_PX, PG_NOTIFY);
    /* Appearance */
    { const char *l[] = { "Theme", "Typeface" }, *v[] = { "Aether Aurora (dark)", "Poppins Regular / Medium" }; int by = group(w, PG_APPEARANCE, x, gy, cw, 2, l, v);
      ui_label_ex(w, x, by + TH_S5, "Palette", TH_TEXT2, FW_MEDIUM, TY_SMALL_PX, PG_APPEARANCE);
      ui_custom(w, rect_make(x, by + TH_S5 + 26, cw, 76), draw_swatches, NULL, NULL, 0, PG_APPEARANCE); }
    for (int i = 0; i < np; i++) free(pk[i]);
}
static void settings_resize(UiWindow *w, int nw, int nh)
{
    char err[96]; if (ui_resize(w, nw, nh, err, sizeof err) != 0) return;
    build(w); ui_redraw(w);
}
int main(void)
{
    UiWindow w; char err[128];
    if (ui_connect(&w, err, sizeof err) != 0) { fprintf(stderr, "aether-settings: %s\n", err); return 1; }
    int cw = th_min(TH_SETTINGS_W, w.screen_w - 2 * TH_S6), ch = th_min(TH_SETTINGS_H, w.screen_h - PANEL_HEIGHT - TH_TITLE_H - TH_S6);
    w.flags = WF_RESIZABLE | WF_CENTER;
    if (ui_create(&w, "Settings", WK_NORMAL, cw, ch, 0, 0, err, sizeof err) != 0) { fprintf(stderr, "aether-settings: %s\n", err); return 1; }
    w.page = PG_ABOUT; build(&w); w.on_resize = settings_resize;
    printf("aether-settings: window ready (%d x %d)\n", w.w, w.h);
    { int np = 0; DIR *d = opendir("/var/lib/aether/pm/db"); struct dirent *e; while (d && (e = readdir(d))) { size_t l = strlen(e->d_name); if (l > 5 && !strcmp(e->d_name + l - 5, ".meta")) np++; } if (d) closedir(d); printf("aether-settings: window ready (%d package entries)\n", np); }
    ui_run(&w); ui_destroy(&w); return 0;
}
