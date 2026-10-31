#include "wallpaper.h"
#include "theme.h"
#include <math.h>
#include <stdlib.h>

static float clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }
static float sstep(float t) { t = clamp01(t); return t * t * (3.0f - 2.0f * t); }
static void unpack(uint32_t c, float *o) { o[0] = (float)((c >> 16) & 255u) / 255.0f; o[1] = (float)((c >> 8) & 255u) / 255.0f; o[2] = (float)(c & 255u) / 255.0f; }

/* One aurora ribbon: centreline y(u) is a cubic in u (v-space, 0 = top), soft-edged, fading towards both ends. */
typedef struct { float y[4], hw0, hw1, strength; uint32_t c0, c1; } Ribbon;
typedef struct { float y, slope, hw, fade, t; } RibCol;

static const Ribbon kRibbons[3] = {
    { { 0.84f, 0.36f, 0.98f, 0.40f }, 0.135f, 0.070f, 0.52f, TH_BLUE, TH_CYAN },
    { { 1.04f, 0.74f, 0.30f, 0.66f }, 0.090f, 0.052f, 0.40f, TH_CYAN, TH_BLUE },
    { { 0.64f, 0.52f, 0.80f, 0.22f }, 0.050f, 0.024f, 0.34f, TH_BLUE_HOVER, TH_CYAN },
};

/* thin anti-aliased circle outline touching only the pixels near the circle */
static void arc_line(Surface *s, float cx, float cy, float r, uint32_t color, unsigned a)
{
    for (int y = 0; y < s->h; y++) {
        float dy = (float)y + 0.5f - cy, ady = fabsf(dy); if (ady > r + 1.5f) continue;
        float xo = sqrtf((r + 1.5f) * (r + 1.5f) - dy * dy), xi = ady < r - 1.5f ? sqrtf((r - 1.5f) * (r - 1.5f) - dy * dy) : 0.0f;
        for (int side = 0; side < 2; side++) {
            int x0 = (int)floorf(side ? cx + xi : cx - xo), x1 = (int)ceilf(side ? cx + xo : cx - xi);
            for (int x = x0; x <= x1; x++) {
                float dx = (float)x + 0.5f - cx, cov = 1.0f - fabsf(sqrtf(dx * dx + dy * dy) - r) * 1.15f;
                if (cov > 0.0f) gfx_blend_px(s, x, y, color, (unsigned)(cov * (float)a));
            }
        }
    }
}

void wallpaper_render(Surface *s)
{
    if (!s || !s->px || s->w < 1 || s->h < 1) return;
    static const unsigned char bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    const int W = s->w, H = s->h; const float fw = (float)W, fh = (float)H, diag = 0.5f * sqrtf(fw * fw + fh * fh);
    float cvoid[3], cdesk[3], cblue[3], rc0[3][3], rc1[3][3];
    unpack(TH_VOID, cvoid); unpack(TH_DESKTOP, cdesk); unpack(TH_BLUE, cblue);
    for (int i = 0; i < 3; i++) { unpack(kRibbons[i].c0, rc0[i]); unpack(kRibbons[i].c1, rc1[i]); }
    RibCol *col = malloc((size_t)W * 3 * sizeof *col); if (!col) return;
    for (int x = 0; x < W; x++) for (int i = 0; i < 3; i++) {
        const Ribbon *r = &kRibbons[i]; float u = ((float)x + 0.5f) / fw, v = 1.0f - u;
        RibCol *c = &col[x * 3 + i];
        c->y = v * v * v * r->y[0] + 3.0f * v * v * u * r->y[1] + 3.0f * v * u * u * r->y[2] + u * u * u * r->y[3];
        float dydu = 3.0f * (v * v * (r->y[1] - r->y[0]) + 2.0f * v * u * (r->y[2] - r->y[1]) + u * u * (r->y[3] - r->y[2]));
        c->slope = dydu * fh / fw; c->hw = (r->hw0 * v + r->hw1 * u) * fh;
        c->fade = sstep(u / 0.28f) * sstep((1.0f - u) / 0.28f); c->t = sstep(u);
        c->fade *= 0.8f + 0.2f * sstep(1.0f - fabsf(u - 0.58f) * 2.2f);          /* gentle swell mid-screen */
    }
    for (int y = 0; y < H; y++) {
        uint32_t *row = s->px + (size_t)y * (size_t)s->stride; const float v = ((float)y + 0.5f) / fh;
        for (int x = 0; x < W; x++) {
            const float u = ((float)x + 0.5f) / fw;
            float dxc = ((float)x + 0.5f) - 0.5f * fw, dyc = ((float)y + 0.5f) - 0.5f * fh;
            float vig = sstep((sqrtf(dxc * dxc + dyc * dyc) / diag - 0.30f) / 0.70f);
            float o[3];
            for (int k = 0; k < 3; k++) o[k] = cdesk[k] + (cvoid[k] - cdesk[k]) * vig;
            /* lower-right blue emergence */
            float gx = ((float)x + 0.5f) - 0.84f * fw, gy = ((float)y + 0.5f) - 0.94f * fh;
            float glow = 1.0f - sstep(sqrtf(gx * gx + gy * gy) / (0.95f * fh)); glow *= glow;
            for (int k = 0; k < 3; k++) o[k] = 1.0f - (1.0f - o[k]) * (1.0f - cblue[k] * glow * 0.40f);
            for (int i = 0; i < 3; i++) {
                const RibCol *c = &col[x * 3 + i]; float d = fabsf((v - c->y) * fh) / sqrtf(1.0f + c->slope * c->slope);
                float t = sstep(1.0f - d / c->hw); if (t <= 0.0f) continue;
                float k_ = t * t * c->fade * kRibbons[i].strength;
                for (int k = 0; k < 3; k++) { float rc = rc0[i][k] + (rc1[i][k] - rc0[i][k]) * c->t; o[k] = 1.0f - (1.0f - o[k]) * (1.0f - rc * k_); }
            }
            (void)u;
            float th = ((float)bayer[y & 3][x & 3] + 0.5f) / 16.0f; unsigned q[3];
            for (int k = 0; k < 3; k++) { float f = clamp01(o[k]) * 255.0f + th - 0.5f; q[k] = f <= 0.0f ? 0u : (f >= 255.0f ? 255u : (unsigned)f); }
            row[x] = (q[0] << 16) | (q[1] << 8) | q[2];
        }
    }
    free(col);
    /* fine geometric accents: three concentric arcs around the lower-right corner, very low opacity */
    arc_line(s, 1.02f * fw, 1.10f * fh, 0.46f * fh, TH_CYAN, 26);
    arc_line(s, 1.02f * fw, 1.10f * fh, 0.64f * fh, TH_BLUE_HOVER, 22);
    arc_line(s, 1.02f * fw, 1.10f * fh, 0.84f * fh, TH_BLUE, 20);
    /* sparse star dust, deterministic */
    uint32_t seed = 0x9E3779B1u; int n = (W * H) / 20000;
    for (int i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u; int x = (int)((seed >> 8) % (unsigned)W);
        seed = seed * 1664525u + 1013904223u; int y = (int)((seed >> 8) % (unsigned)H);
        seed = seed * 1664525u + 1013904223u; unsigned a = 40u + (seed >> 24) % 110u;
        gfx_blend_px(s, x, y, TH_TEXT, a);
        if (a > 120u) { gfx_blend_px(s, x + 1, y, TH_CYAN, a / 4u); gfx_blend_px(s, x, y + 1, TH_CYAN, a / 4u); }
    }
}
