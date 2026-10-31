/* Aether UI text: anti-aliased proportional faces (Poppins Regular/Medium, see tools/gen-font-aa.py) drawn with
 * alpha blending onto opaque surfaces. The legacy 8x16 bitmap font in gfx.h remains for compatibility. */
#ifndef AETHER_FONT_H
#define AETHER_FONT_H
#include <stddef.h>
#include <stdint.h>
#include "gfx.h"

typedef struct { uint32_t off; uint8_t w, h; int8_t bx, by; uint16_t adv64; } FontGlyph;   /* by = rows above the baseline */
typedef struct { int px, ascent, descent, cap, weight; const FontGlyph *g; const uint8_t *alpha; } Face;
enum { FW_REGULAR = 400, FW_MEDIUM = 500 };
enum { FA_LEFT = -1, FA_CENTER = 0, FA_RIGHT = 1 };
extern const Face aether_faces[];
extern const int aether_nfaces;

const Face *font_face(int weight, int px);                 /* exact match, else the nearest size of that weight; never NULL */
int font_width(const Face *f, const char *s);              /* advance in whole pixels */
int font_line_h(const Face *f);                            /* comfortable line pitch */
int font_draw(Surface *s, Rect clip, int x, int baseline, const char *str, const Face *f, uint32_t color);   /* returns advance */
/* Draw on one line inside `box`, vertically centred on the cap height, clipped to the box. Returns the drawn width. */
int font_draw_box(Surface *s, Rect box, const char *str, const Face *f, uint32_t color, int align);
/* Copy str into out, appending "..." when it does not fit in maxw pixels. */
void font_ellipsize(char *out, size_t n, const char *str, const Face *f, int maxw);
/* Greedy word wrap into at most maxlines lines (last line ellipsized). Returns the number of lines. */
int font_wrap(const char *str, const Face *f, int maxw, char lines[][96], int maxlines);
#endif
