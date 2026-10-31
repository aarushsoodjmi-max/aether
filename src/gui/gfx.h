/* Aether GUI: software 2D primitives on XRGB8888 surfaces. No dependency on display, input or packages. */
#ifndef AETHER_GFX_H
#define AETHER_GFX_H
#include <stdint.h>

typedef struct { uint32_t *px; int w, h, stride; } Surface;   /* stride counted in pixels */
typedef struct { int x, y, w, h; } Rect;

#define FONT_W 8
#define FONT_H 16
extern const unsigned char aether_font8x16[95][16];

static inline uint32_t gfx_rgb(unsigned r, unsigned g, unsigned b) { return (r << 16) | (g << 8) | b; }
Rect rect_make(int x, int y, int w, int h);
Rect rect_intersect(Rect a, Rect b);                  /* w/h are 0 when disjoint */
int rect_contains(Rect r, int x, int y);

void gfx_fill(Surface *s, Rect r, uint32_t color);                          /* clipped */
void gfx_outline(Surface *s, Rect r, uint32_t color);
void gfx_gradient_v(Surface *s, Rect r, uint32_t top, uint32_t bottom);
void gfx_blit(Surface *dst, int dx, int dy, const Surface *src, Rect srcr); /* opaque, clipped on both surfaces */
int gfx_text_width(const char *s);
int gfx_text(Surface *s, int x, int y, const char *str, uint32_t fg);       /* returns advance; non-ASCII drawn as '?' */
void gfx_text_clipped(Surface *s, Rect clip, int x, int y, const char *str, uint32_t fg);

/* ---- Aurora additions: alpha blending, anti-aliased rounded shapes, shadows and vector strokes --------------
 * All colours are 0xRRGGBB; `a` is 0..255 coverage/opacity. Surfaces stay opaque XRGB8888. */
static inline uint32_t gfx_mix(uint32_t a, uint32_t b, unsigned t)       /* t = 0..256 : a -> b */
{
    unsigned r = (((a >> 16) & 255u) * (256u - t) + ((b >> 16) & 255u) * t) >> 8;
    unsigned g = (((a >> 8) & 255u) * (256u - t) + ((b >> 8) & 255u) * t) >> 8;
    unsigned bl = ((a & 255u) * (256u - t) + (b & 255u) * t) >> 8;
    return (r << 16) | (g << 8) | bl;
}
void gfx_blend_px(Surface *s, int x, int y, uint32_t color, unsigned a);
void gfx_fill_alpha(Surface *s, Rect r, uint32_t color, unsigned a);
/* Rounded rectangle with independent corner radii (clamped to half the short side), anti-aliased corners. */
void gfx_rrect_ex(Surface *s, Rect r, int rtl, int rtr, int rbr, int rbl, uint32_t color, unsigned a);
static inline void gfx_rrect(Surface *s, Rect r, int rad, uint32_t color) { gfx_rrect_ex(s, r, rad, rad, rad, rad, color, 255); }
static inline void gfx_rrect_a(Surface *s, Rect r, int rad, uint32_t color, unsigned a) { gfx_rrect_ex(s, r, rad, rad, rad, rad, color, a); }
/* Soft shadow cast by `body` (rounded by rad), shifted down by dy, fading out over `blur` px. Only pixels outside
 * the body rectangle are touched, so paint the body afterwards. max_a is the opacity at the body edge. */
void gfx_shadow(Surface *s, Rect body, int rad, int blur, int dy, unsigned max_a);
/* Copy src->dst with the given corners rounded (anti-aliased) and a constant opacity. Fully covered pixels at
 * a == 255 are copied exactly. Clipped on both surfaces. */
void gfx_blit_rounded(Surface *dst, int dx, int dy, const Surface *src, Rect sr, int rtl, int rtr, int rbr, int rbl, unsigned a);
void gfx_gradient_h(Surface *s, Rect r, uint32_t left, uint32_t right);
/* Vector strokes (coordinates are pixel-space floats; hw = half the stroke width). */
void gfx_capsule(Surface *s, float x0, float y0, float x1, float y1, float hw, uint32_t color, unsigned a);
void gfx_disc(Surface *s, float cx, float cy, float r, uint32_t color, unsigned a);
void gfx_ring(Surface *s, float cx, float cy, float r, float hw, uint32_t color, unsigned a);
void gfx_quad(Surface *s, float x0, float y0, float cx, float cy, float x1, float y1, float hw, uint32_t color, unsigned a);
#endif
