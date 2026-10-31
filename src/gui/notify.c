/* aether-notify TITLE BODY [TIMEOUT_MS] [info|success|warning|error]: show a desktop notification (a WK_NOTIFY window,
 * Aurora card with a level glyph, fade in/out) and exit. The level may also come from AETHER_NOTIFY_LEVEL. */
#define _GNU_SOURCE
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long t0, end_ms, fade_out_at; static uint32_t accent; static IconId glyph; static char title[64], lines[3][96]; static int nlines;
static void begin_fade_out(void) { long now = ui_now_ms(); if (fade_out_at > now) { fade_out_at = now; end_ms = now + TH_ANIM_NOTIFY_OUT; } }
static void dismiss(UiWindow *w, int widget, void *user) { (void)w; (void)widget; (void)user; begin_fade_out(); }
static void draw_card(UiWindow *w, int i, Surface *s)
{
    (void)i; gfx_fill(s, rect_make(0, 0, TH_S1 - 1, w->h), accent);                                  /* level accent along the left edge */
    gfx_disc(s, (float)(TH_S4 + 14), (float)(TH_S4 + 14), 14.0f, gfx_mix(TH_WINDOW, accent, 48), 255);
    icon_draw_ex(s, glyph, TH_S4 + 5, TH_S4 + 5, 18, accent, accent, -1);
    int tx = TH_S4 + 28 + TH_S3;
    font_draw_box(s, rect_make(tx, TH_S3, w->w - tx - 40, 20), title, font_face(FW_MEDIUM, TY_BODY_PX), TH_TEXT, FA_LEFT);
    for (int l = 0; l < nlines; l++) font_draw_box(s, rect_make(tx, TH_S3 + 22 + l * 17, w->w - tx - TH_S4, 17), lines[l], font_face(FW_REGULAR, TY_SMALL_PX), TH_TEXT2, FA_LEFT);
}
static void draw_close(UiWindow *w, int i, Surface *s)
{
    const UiWidget *x = &w->wid[i]; if (x->hover) gfx_rrect(s, x->r, TH_R_CTRL - 2, x->pressed ? TH_SURFACE2_PRESS : TH_SURFACE2_HOVER);
    icon_draw(s, ICON_CLOSE, x->r.x + 3, x->r.y + 3, 18, x->hover ? IS_HOVER : IS_MUTED);
}
static void tick(UiWindow *w)
{
    long now = ui_now_ms(), e = now - t0; int a = 255;
    if (e < TH_ANIM_NOTIFY_IN) { float t = (float)e / (float)TH_ANIM_NOTIFY_IN; a = (int)(255.0f * (1.0f - (1.0f - t) * (1.0f - t))); }
    else if (now >= fade_out_at) { a = (int)(255L * (end_ms - now) / (end_ms - fade_out_at)); if (a < 0) a = 0; }
    ui_set_opacity(w, a);
    if (now >= end_ms) { w->closed = 1; return; }
    w->tick_ms = (a == 255 && now < fade_out_at - 300) ? 200 : TH_FRAME_MS;
}
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: aether-notify TITLE BODY [TIMEOUT_MS] [info|success|warning|error]\n"); return 2; }
    long timeout = argc > 3 ? atol(argv[3]) : 4000; if (timeout < 600) timeout = 600;
    const char *lvl = argc > 4 ? argv[4] : getenv("AETHER_NOTIFY_LEVEL"); if (!lvl) lvl = "info";
    accent = TH_BLUE_HOVER; glyph = ICON_INFO;
    if (!strcmp(lvl, "success")) { accent = TH_OK; glyph = ICON_CHECK; } else if (!strcmp(lvl, "warning")) { accent = TH_WARN; glyph = ICON_WARNING; } else if (!strcmp(lvl, "error")) { accent = TH_ERR; glyph = ICON_ERROR; }
    UiWindow w; char err[128];
    if (ui_connect(&w, err, sizeof err) != 0) { fprintf(stderr, "aether-notify: %s\n", err); return 1; }
    int cw = th_min(TH_NOTIFY_W, w.screen_w - 2 * TH_S4), tx = TH_S4 + 28 + TH_S3;
    font_ellipsize(title, sizeof title, argv[1], font_face(FW_MEDIUM, TY_BODY_PX), cw - tx - 40);
    nlines = font_wrap(argv[2], font_face(FW_REGULAR, TY_SMALL_PX), cw - tx - TH_S4, lines, 3);
    int ch = th_max(64, TH_S3 + 22 + nlines * 17 + TH_S4);
    w.bg = TH_WINDOW; w.flags = WF_TRANSPARENT;
    if (ui_create(&w, "notification", WK_NOTIFY, cw, ch, 0, 0, err, sizeof err) != 0) { fprintf(stderr, "aether-notify: %s\n", err); return 1; }
    ui_custom(&w, rect_make(0, 0, cw, ch), draw_card, dismiss, NULL, 1, UI_ALL_PAGES);                 /* clicking anywhere dismisses */
    ui_custom(&w, rect_make(cw - TH_S2 - 24, TH_S2, 24, 24), draw_close, dismiss, NULL, 1, UI_ALL_PAGES);
    t0 = ui_now_ms(); end_ms = t0 + timeout; fade_out_at = end_ms - TH_ANIM_NOTIFY_OUT;
    w.tick_ms = TH_FRAME_MS; w.on_tick = tick; printf("aether-notify: showing '%s'\n", title);
    ui_run(&w); ui_destroy(&w); return 0;
}
