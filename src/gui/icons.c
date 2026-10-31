#include "icons.h"
#include "font.h"
#include "theme.h"
#include <math.h>
#include <string.h>

typedef struct { const char *name; IconId id; } IconName;
static const IconName kNames[] = { { "aether", ICON_AETHER }, { "settings", ICON_SETTINGS }, { "search", ICON_SEARCH }, { "info", ICON_INFO },
    { "display", ICON_DISPLAY }, { "package", ICON_PACKAGE }, { "bell", ICON_BELL }, { "network", ICON_NETWORK }, { "battery", ICON_BATTERY }, { "app", ICON_APP } };
IconId icon_by_name(const char *name)
{
    if (!name) return ICON_NONE;
    for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; i++) if (!strcmp(kNames[i].name, name)) return kNames[i].id;
    return ICON_NONE;
}
uint32_t icon_main_color(IconState st)
{
    switch (st) { case IS_MUTED: return TH_MUTED; case IS_HOVER: return TH_TEXT; case IS_ACTIVE: return TH_BLUE_HOVER;
                  case IS_DISABLED: return gfx_mix(TH_MUTED, TH_VOID, 110); default: return TH_TEXT2; }
}
uint32_t icon_accent_color(IconState st)
{
    switch (st) { case IS_MUTED: return TH_MUTED; case IS_DISABLED: return gfx_mix(TH_MUTED, TH_VOID, 110); case IS_HOVER: case IS_ACTIVE: return TH_CYAN; default: return TH_BLUE_HOVER; }
}

void icon_draw_ex(Surface *s, IconId id, int ix, int iy, int size, uint32_t m, uint32_t ac, int level)
{
    float k = (float)size / 24.0f, ox = (float)ix, oy = (float)iy, hw = 0.95f * k;
    if (hw < 0.6f) hw = 0.6f;
#define PX(v) (ox + (v) * k)
#define PY(v) (oy + (v) * k)
#define LN(x0, y0, x1, y1, c) gfx_capsule(s, PX(x0), PY(y0), PX(x1), PY(y1), hw, (c), 255)
#define QD(x0, y0, cx, cy, x1, y1, c) gfx_quad(s, PX(x0), PY(y0), PX(cx), PY(cy), PX(x1), PY(y1), hw, (c), 255)
#define RING(cx, cy, r, c) gfx_ring(s, PX(cx), PY(cy), (r) * k, hw * 0.92f, (c), 255)
#define DOT(cx, cy, r, c) gfx_disc(s, PX(cx), PY(cy), (r) * k, (c), 255)
    switch (id) {
    case ICON_AETHER: {                                  /* an "A" whose crossbar is an aurora arc: blue leg, cyan leg */
        float lw = hw * 1.12f;
        uint32_t l = ac == m ? m : TH_BLUE_HOVER, r = ac == m ? m : TH_CYAN;
        gfx_capsule(s, PX(4.6f), PY(19.8f), PX(12.0f), PY(4.4f), lw, l, 255);
        gfx_capsule(s, PX(19.4f), PY(19.8f), PX(12.0f), PY(4.4f), lw, r, 255);
        gfx_quad(s, PX(7.4f), PY(15.2f), PX(12.0f), PY(10.6f), PX(16.6f), PY(15.2f), lw * 0.9f, ac == m ? m : TH_TEXT, 255);
        break; }
    case ICON_SETTINGS: {                                /* three sliders */
        static const float ys[3] = { 7.0f, 12.0f, 17.0f }, kx[3] = { 15.5f, 8.5f, 13.5f };
        for (int i = 0; i < 3; i++) {
            LN(4.0f, ys[i], kx[i] - 3.5f, ys[i], m); LN(kx[i] + 3.5f, ys[i], 20.0f, ys[i], m); RING(kx[i], ys[i], 2.3f, ac);
        }
        break; }
    case ICON_SEARCH: RING(10.5f, 10.5f, 5.6f, m); LN(14.8f, 14.8f, 19.6f, 19.6f, ac); break;
    case ICON_CLOSE: LN(6.5f, 6.5f, 17.5f, 17.5f, m); LN(17.5f, 6.5f, 6.5f, 17.5f, m); break;
    case ICON_MINIMIZE: LN(6.5f, 16.5f, 17.5f, 16.5f, m); break;
    case ICON_MAXIMIZE:
        LN(6.5f, 6.5f, 17.5f, 6.5f, m); LN(17.5f, 6.5f, 17.5f, 17.5f, m); LN(17.5f, 17.5f, 6.5f, 17.5f, m); LN(6.5f, 17.5f, 6.5f, 6.5f, m); break;
    case ICON_RESTORE:
        LN(5.5f, 9.5f, 14.5f, 9.5f, m); LN(14.5f, 9.5f, 14.5f, 18.5f, m); LN(14.5f, 18.5f, 5.5f, 18.5f, m); LN(5.5f, 18.5f, 5.5f, 9.5f, m);
        LN(9.5f, 9.5f, 9.5f, 5.5f, m); LN(9.5f, 5.5f, 18.5f, 5.5f, m); LN(18.5f, 5.5f, 18.5f, 14.5f, m); LN(18.5f, 14.5f, 14.5f, 14.5f, m); break;
    case ICON_BELL:
        QD(6.5f, 17.0f, 6.5f, 8.8f, 12.0f, 6.6f, m); QD(17.5f, 17.0f, 17.5f, 8.8f, 12.0f, 6.6f, m);
        LN(5.0f, 17.0f, 19.0f, 17.0f, m); LN(10.4f, 20.2f, 13.6f, 20.2f, ac); DOT(12.0f, 4.6f, 1.1f, ac); break;
    case ICON_NETWORK:
        RING(12.0f, 12.0f, 8.2f, m); LN(3.8f, 12.0f, 20.2f, 12.0f, ac);
        QD(12.0f, 3.8f, 6.6f, 12.0f, 12.0f, 20.2f, ac); QD(12.0f, 3.8f, 17.4f, 12.0f, 12.0f, 20.2f, ac);
        if (level == 0) { LN(4.5f, 19.5f, 19.5f, 4.5f, TH_WARN); }
        break;
    case ICON_BATTERY: {
        LN(3.2f, 7.6f, 19.2f, 7.6f, m); LN(19.2f, 7.6f, 19.2f, 16.4f, m); LN(19.2f, 16.4f, 3.2f, 16.4f, m); LN(3.2f, 16.4f, 3.2f, 7.6f, m); LN(21.4f, 10.6f, 21.4f, 13.4f, m);
        if (level >= 0) {
            int lv = th_clamp(level, 0, 100); float wfill = 12.0f * (float)lv / 100.0f; uint32_t fc = lv >= 40 ? TH_OK : (lv >= 20 ? TH_WARN : TH_ERR);
            if (wfill > 0.4f) gfx_capsule(s, PX(5.6f), PY(12.0f), PX(5.6f + wfill), PY(12.0f), hw * 1.3f, fc, 255);
        }
        break; }
    case ICON_INFO: RING(12.0f, 12.0f, 8.6f, m); DOT(12.0f, 8.0f, 1.15f, ac); LN(12.0f, 11.2f, 12.0f, 16.4f, ac); break;
    case ICON_DISPLAY:
        LN(3.8f, 5.2f, 20.2f, 5.2f, m); LN(20.2f, 5.2f, 20.2f, 15.8f, m); LN(20.2f, 15.8f, 3.8f, 15.8f, m); LN(3.8f, 15.8f, 3.8f, 5.2f, m);
        LN(12.0f, 15.8f, 12.0f, 19.6f, ac); LN(8.0f, 19.6f, 16.0f, 19.6f, ac); break;
    case ICON_PACKAGE:
        LN(12.0f, 3.6f, 19.6f, 7.9f, m); LN(19.6f, 7.9f, 19.6f, 16.1f, m); LN(19.6f, 16.1f, 12.0f, 20.4f, m);
        LN(12.0f, 20.4f, 4.4f, 16.1f, m); LN(4.4f, 16.1f, 4.4f, 7.9f, m); LN(4.4f, 7.9f, 12.0f, 3.6f, m);
        LN(4.4f, 7.9f, 12.0f, 12.2f, ac); LN(19.6f, 7.9f, 12.0f, 12.2f, ac); LN(12.0f, 12.2f, 12.0f, 20.4f, ac); break;
    case ICON_APP: {
        static const float p[4][2] = { { 4.5f, 4.5f }, { 13.5f, 4.5f }, { 4.5f, 13.5f }, { 13.5f, 13.5f } };
        for (int i = 0; i < 4; i++) gfx_rrect(s, rect_make((int)(PX(p[i][0]) + 0.5f), (int)(PY(p[i][1]) + 0.5f), (int)(6.0f * k + 0.5f), (int)(6.0f * k + 0.5f)), (int)(1.8f * k), i == 3 ? ac : m);
        break; }
    case ICON_CHECK: RING(12.0f, 12.0f, 8.6f, m); LN(8.0f, 12.4f, 10.9f, 15.3f, ac); LN(10.9f, 15.3f, 16.2f, 9.2f, ac); break;
    case ICON_WARNING:
        LN(12.0f, 4.2f, 20.8f, 19.4f, m); LN(20.8f, 19.4f, 3.2f, 19.4f, m); LN(3.2f, 19.4f, 12.0f, 4.2f, m);
        LN(12.0f, 9.6f, 12.0f, 14.2f, ac); DOT(12.0f, 16.8f, 1.1f, ac); break;
    case ICON_ERROR: RING(12.0f, 12.0f, 8.6f, m); LN(8.9f, 8.9f, 15.1f, 15.1f, ac); LN(15.1f, 8.9f, 8.9f, 15.1f, ac); break;
    default: break;
    }
#undef PX
#undef PY
#undef LN
#undef QD
#undef RING
#undef DOT
}
void icon_draw(Surface *s, IconId id, int x, int y, int size, IconState st) { icon_draw_ex(s, id, x, y, size, icon_main_color(st), icon_accent_color(st), -1); }

void icon_app_tile(Surface *s, Rect box, IconId id, const char *name, int hover, int selected)
{
    int rad = box.w >= 60 ? 16 : 14;
    uint32_t top = gfx_mix(TH_SURFACE2, TH_BLUE, hover ? 70 : 36), bot = gfx_mix(TH_SURFACE2, TH_VOID, hover ? 40 : 76);
    gfx_rrect(s, box, rad, selected ? TH_BLUE : (hover ? gfx_mix(TH_BORDER, TH_BLUE, 120) : TH_BORDER));
    Rect in = rect_make(box.x + 1, box.y + 1, box.w - 2, box.h - 2);
    gfx_rrect(s, in, rad - 1, bot);
    for (int i = 0; i < in.h * 6 / 10; i++) {                                    /* soft top sheen, clipped to the rounded body */
        int inset = i < rad - 1 ? (rad - 1) - (int)(sqrtf((float)((rad - 1) * (rad - 1) - (rad - 1 - i) * (rad - 1 - i)))) : 0;
        unsigned t = (unsigned)(i * 256 / (in.h * 6 / 10));
        gfx_fill_alpha(s, rect_make(in.x + inset, in.y + i, in.w - 2 * inset, 1), top, (256u - t) * 200u / 256u);
    }
    int gs = box.w >= 60 ? 32 : 28; int gx = box.x + (box.w - gs) / 2, gy = box.y + (box.h - gs) / 2;
    if (id != ICON_NONE) icon_draw(s, id, gx, gy, gs, hover || selected ? IS_ACTIVE : IS_HOVER);
    else { char m[2] = { name && name[0] ? name[0] : '?', 0 }; if (m[0] >= 'a' && m[0] <= 'z') m[0] = (char)(m[0] - 32); font_draw_box(s, box, m, font_face(FW_MEDIUM, 20), TH_CYAN, FA_CENTER); }
}
