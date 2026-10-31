#include "gfx.h"
#include <string.h>

Rect rect_make(int x, int y, int w, int h) { Rect r = { x, y, w, h }; return r; }
Rect rect_intersect(Rect a, Rect b)
{
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0) return rect_make(x0, y0, 0, 0);
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}
int rect_contains(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static Rect bounds(const Surface *s) { return rect_make(0, 0, s->w, s->h); }

void gfx_fill(Surface *s, Rect r, uint32_t color)
{
    r = rect_intersect(r, bounds(s));
    for (int y = r.y; y < r.y + r.h; y++) { uint32_t *row = s->px + (size_t)y * (size_t)s->stride; for (int x = r.x; x < r.x + r.w; x++) row[x] = color; }
}
void gfx_outline(Surface *s, Rect r, uint32_t color)
{
    gfx_fill(s, rect_make(r.x, r.y, r.w, 1), color); gfx_fill(s, rect_make(r.x, r.y + r.h - 1, r.w, 1), color);
    gfx_fill(s, rect_make(r.x, r.y, 1, r.h), color); gfx_fill(s, rect_make(r.x + r.w - 1, r.y, 1, r.h), color);
}
void gfx_gradient_v(Surface *s, Rect r, uint32_t top, uint32_t bottom)
{
    if (r.h <= 0) return;
    for (int i = 0; i < r.h; i++) {
        unsigned t = r.h > 1 ? (unsigned)(i * 256 / (r.h - 1)) : 0, c = 0;
        for (int sh = 16; sh >= 0; sh -= 8) { unsigned a = (top >> sh) & 255, b = (bottom >> sh) & 255; c |= ((a * (256 - t) + b * t) >> 8 & 255) << sh; }
        gfx_fill(s, rect_make(r.x, r.y + i, r.w, 1), c);
    }
}
void gfx_blit(Surface *dst, int dx, int dy, const Surface *src, Rect sr)
{
    sr = rect_intersect(sr, bounds(src));
    if (dx < 0) { sr.x -= dx; sr.w += dx; dx = 0; }
    if (dy < 0) { sr.y -= dy; sr.h += dy; dy = 0; }
    if (sr.w <= 0 || sr.h <= 0 || dx >= dst->w || dy >= dst->h) return;
    if (dx + sr.w > dst->w) sr.w = dst->w - dx;
    if (dy + sr.h > dst->h) sr.h = dst->h - dy;
    for (int y = 0; y < sr.h; y++)
        memcpy(dst->px + (size_t)(dy + y) * (size_t)dst->stride + (size_t)dx, src->px + (size_t)(sr.y + y) * (size_t)src->stride + (size_t)sr.x, (size_t)sr.w * 4);
}
int gfx_text_width(const char *s) { return (int)strlen(s) * FONT_W; }
void gfx_text_clipped(Surface *s, Rect clip, int x, int y, const char *str, uint32_t fg)
{
    clip = rect_intersect(clip, bounds(s));
    for (; *str; str++, x += FONT_W) {
        unsigned char c = (unsigned char)*str; if (c < 32 || c > 126) c = '?';
        const unsigned char *g = aether_font8x16[c - 32];
        for (int gy = 0; gy < FONT_H; gy++) for (int gx = 0; gx < FONT_W; gx++)
            if ((g[gy] & (0x80 >> gx)) && rect_contains(clip, x + gx, y + gy)) s->px[(size_t)(y + gy) * (size_t)s->stride + (size_t)(x + gx)] = fg;
    }
}
int gfx_text(Surface *s, int x, int y, const char *str, uint32_t fg) { gfx_text_clipped(s, bounds(s), x, y, str, fg); return gfx_text_width(str); }

/* ===================================================================================================================
 * Aurora additions
 * =================================================================================================================== */
#include <math.h>

void gfx_blend_px(Surface *s, int x, int y, uint32_t color, unsigned a)
{
    if (a == 0 || x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    uint32_t *p = &s->px[(size_t)y * (size_t)s->stride + (size_t)x];
    *p = a >= 255 ? (color & 0xffffffu) : gfx_mix(*p, color, a + (a >> 7));
}
void gfx_fill_alpha(Surface *s, Rect r, uint32_t color, unsigned a)
{
    if (a >= 255) { gfx_fill(s, r, color & 0xffffffu); return; }
    if (a == 0) return;
    r = rect_intersect(r, bounds(s));
    for (int y = r.y; y < r.y + r.h; y++) { uint32_t *row = s->px + (size_t)y * (size_t)s->stride; for (int x = r.x; x < r.x + r.w; x++) row[x] = gfx_mix(row[x], color, a + (a >> 7)); }
}

/* coverage (0..255) of pixel (i,j) inside a corner of radius rad; i,j count pixels from the corner's outer edges */
static unsigned corner_cov(int i, int j, int rad)
{
    float dx = (float)rad - ((float)i + 0.5f), dy = (float)rad - ((float)j + 0.5f);
    float c = (float)rad - sqrtf(dx * dx + dy * dy) + 0.5f;
    return c <= 0.0f ? 0u : (c >= 1.0f ? 255u : (unsigned)(c * 255.0f + 0.5f));
}
static int clamp_rad(int v, Rect r) { int m = (r.w < r.h ? r.w : r.h) / 2; return v < 0 ? 0 : (v > m ? m : v); }

void gfx_rrect_ex(Surface *s, Rect r, int rtl, int rtr, int rbr, int rbl, uint32_t color, unsigned a)
{
    if (r.w <= 0 || r.h <= 0 || a == 0) return;
    rtl = clamp_rad(rtl, r); rtr = clamp_rad(rtr, r); rbr = clamp_rad(rbr, r); rbl = clamp_rad(rbl, r);
    for (int y = 0; y < r.h; y++) {
        int sy = r.y + y; if (sy < 0 || sy >= s->h) continue;
        int top = y < r.h / 2;
        int lrad = top ? rtl : rbl, rrad = top ? rtr : rbr;
        int j = top ? y : r.h - 1 - y;                                   /* rows from the nearest horizontal edge */
        if (j >= lrad) lrad = 0;
        if (j >= rrad) rrad = 0;
        for (int i = 0; i < lrad; i++) gfx_blend_px(s, r.x + i, sy, color, corner_cov(i, j, lrad) * a / 255u);
        for (int i = 0; i < rrad; i++) gfx_blend_px(s, r.x + r.w - 1 - i, sy, color, corner_cov(i, j, rrad) * a / 255u);
        gfx_fill_alpha(s, rect_make(r.x + lrad, sy, r.w - lrad - rrad, 1), color, a);
    }
}

void gfx_blit_rounded(Surface *dst, int dx, int dy, const Surface *src, Rect sr, int rtl, int rtr, int rbr, int rbl, unsigned a)
{
    sr = rect_intersect(sr, bounds(src));
    if (sr.w <= 0 || sr.h <= 0 || a == 0) return;
    Rect full = rect_make(0, 0, sr.w, sr.h);
    rtl = clamp_rad(rtl, full); rtr = clamp_rad(rtr, full); rbr = clamp_rad(rbr, full); rbl = clamp_rad(rbl, full);
    for (int y = 0; y < sr.h; y++) {
        int ty = dy + y; if (ty < 0 || ty >= dst->h) continue;
        int top = y < sr.h / 2, lrad = top ? rtl : rbl, rrad = top ? rtr : rbr, j = top ? y : sr.h - 1 - y;
        if (j >= lrad) lrad = 0;
        if (j >= rrad) rrad = 0;
        const uint32_t *srow = src->px + (size_t)(sr.y + y) * (size_t)src->stride + (size_t)sr.x;
        uint32_t *drow = dst->px + (size_t)ty * (size_t)dst->stride;
        for (int x = 0; x < sr.w; x++) {
            int tx = dx + x; if (tx < 0 || tx >= dst->w) continue;
            unsigned cov = a;
            if (x < lrad) cov = corner_cov(x, j, lrad) * a / 255u;
            else if (x >= sr.w - rrad) cov = corner_cov(sr.w - 1 - x, j, rrad) * a / 255u;
            if (cov >= 255) drow[tx] = srow[x] & 0xffffffu;
            else if (cov) drow[tx] = gfx_mix(drow[tx], srow[x], cov + (cov >> 7));
        }
    }
}

void gfx_shadow(Surface *s, Rect body, int rad, int blur, int dy, unsigned max_a)
{
    if (blur < 1 || max_a == 0 || body.w <= 0 || body.h <= 0) return;
    Rect shape = rect_make(body.x, body.y + dy, body.w, body.h);
    Rect region = rect_intersect(rect_make(shape.x - blur, shape.y - blur, shape.w + 2 * blur, shape.h + 2 * blur), bounds(s));
    float hw = (float)shape.w * 0.5f, hh = (float)shape.h * 0.5f, cx = (float)shape.x + hw, cy = (float)shape.y + hh;
    float r = (float)rad; if (r > hw) r = hw; if (r > hh) r = hh;
    for (int y = region.y; y < region.y + region.h; y++) {
        int row_inside = y >= body.y + rad && y < body.y + body.h - rad;
        for (int x = region.x; x < region.x + region.w; x++) {
            if (row_inside && x >= body.x + rad && x < body.x + body.w - rad) { x = body.x + body.w - rad - 1; continue; }   /* hidden by the body */
            if (y >= body.y + rad && y < body.y + body.h - rad && x >= body.x && x < body.x + body.w) continue;
            float qx = fabsf((float)x + 0.5f - cx) - (hw - r), qy = fabsf((float)y + 0.5f - cy) - (hh - r);
            float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
            float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0.0f ? qx : 0.0f) : (qy < 0.0f ? qy : 0.0f)) - r;
            if (d >= (float)blur) continue;
            float t = d <= 0.0f ? 1.0f : 1.0f - d / (float)blur;
            gfx_blend_px(s, x, y, 0x000000u, (unsigned)(t * t * (float)max_a));
        }
    }
}

void gfx_gradient_h(Surface *s, Rect r, uint32_t left, uint32_t right)
{
    if (r.w <= 0) return;
    for (int i = 0; i < r.w; i++) {
        unsigned t = r.w > 1 ? (unsigned)(i * 256 / (r.w - 1)) : 0;
        gfx_fill(s, rect_make(r.x + i, r.y, 1, r.h), gfx_mix(left, right, t));
    }
}

/* ---- vector strokes: coverage from the distance to the shape, 1 px anti-aliasing ramp ---- */
static unsigned cov_from(float edge_dist) { float c = 0.5f - edge_dist; return c <= 0.0f ? 0u : (c >= 1.0f ? 255u : (unsigned)(c * 255.0f + 0.5f)); }
void gfx_capsule(Surface *s, float x0, float y0, float x1, float y1, float hw, uint32_t color, unsigned a)
{
    int bx0 = (int)floorf((x0 < x1 ? x0 : x1) - hw - 1.0f), bx1 = (int)ceilf((x0 > x1 ? x0 : x1) + hw + 1.0f);
    int by0 = (int)floorf((y0 < y1 ? y0 : y1) - hw - 1.0f), by1 = (int)ceilf((y0 > y1 ? y0 : y1) + hw + 1.0f);
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    for (int y = by0; y <= by1; y++) for (int x = bx0; x <= bx1; x++) {
        float px = (float)x + 0.5f - x0, py = (float)y + 0.5f - y0, t = len2 > 0.0f ? (px * vx + py * vy) / len2 : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        float ex = px - vx * t, ey = py - vy * t;
        unsigned c = cov_from(sqrtf(ex * ex + ey * ey) - hw);
        if (c) gfx_blend_px(s, x, y, color, c * a / 255u);
    }
}
void gfx_disc(Surface *s, float cx, float cy, float r, uint32_t color, unsigned a)
{
    for (int y = (int)floorf(cy - r - 1.0f); y <= (int)ceilf(cy + r + 1.0f); y++) for (int x = (int)floorf(cx - r - 1.0f); x <= (int)ceilf(cx + r + 1.0f); x++) {
        float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy; unsigned c = cov_from(sqrtf(dx * dx + dy * dy) - r);
        if (c) gfx_blend_px(s, x, y, color, c * a / 255u);
    }
}
void gfx_ring(Surface *s, float cx, float cy, float r, float hw, uint32_t color, unsigned a)
{
    float o = r + hw;
    for (int y = (int)floorf(cy - o - 1.0f); y <= (int)ceilf(cy + o + 1.0f); y++) for (int x = (int)floorf(cx - o - 1.0f); x <= (int)ceilf(cx + o + 1.0f); x++) {
        float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy; unsigned c = cov_from(fabsf(sqrtf(dx * dx + dy * dy) - r) - hw);
        if (c) gfx_blend_px(s, x, y, color, c * a / 255u);
    }
}
void gfx_quad(Surface *s, float x0, float y0, float cx, float cy, float x1, float y1, float hw, uint32_t color, unsigned a)
{
    float px = x0, py = y0;
    for (int i = 1; i <= 14; i++) {
        float t = (float)i / 14.0f, u = 1.0f - t, x = u * u * x0 + 2.0f * u * t * cx + t * t * x1, y = u * u * y0 + 2.0f * u * t * cy + t * t * y1;
        gfx_capsule(s, px, py, x, y, hw, color, a); px = x; py = y;
    }
}
