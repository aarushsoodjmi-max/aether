#include "font.h"
#include <stdio.h>
#include <string.h>

const Face *font_face(int weight, int px)
{
    const Face *best = NULL; int bd = 1 << 30;
    for (int i = 0; i < aether_nfaces; i++) {
        const Face *f = &aether_faces[i]; if (f->weight != weight) continue;
        int d = f->px > px ? f->px - px : px - f->px; if (d < bd) { bd = d; best = f; }
    }
    if (!best) best = &aether_faces[0];
    return best;
}
static const FontGlyph *glyph(const Face *f, unsigned char c) { if (c < 32 || c > 126) c = '?'; return &f->g[c - 32]; }
int font_width(const Face *f, const char *s)
{
    unsigned total = 0; for (; *s; s++) total += glyph(f, (unsigned char)*s)->adv64;
    return (int)((total + 32u) >> 6);
}
int font_line_h(const Face *f) { return f->px + f->px / 2 + 2; }

int font_draw(Surface *s, Rect clip, int x, int baseline, const char *str, const Face *f, uint32_t color)
{
    clip = rect_intersect(clip, rect_make(0, 0, s->w, s->h));
    unsigned pen = 0;
    for (; *str; str++) {
        const FontGlyph *g = glyph(f, (unsigned char)*str);
        int gx = x + (int)((pen + 32u) >> 6) + g->bx, gy = baseline - g->by;
        const uint8_t *a = f->alpha + g->off;
        for (int r = 0; r < g->h; r++) {
            int py = gy + r; if (py < clip.y || py >= clip.y + clip.h) continue;
            uint32_t *row = s->px + (size_t)py * (size_t)s->stride;
            for (int c = 0; c < g->w; c++) {
                unsigned cv = a[r * g->w + c]; int pxx = gx + c;
                if (!cv || pxx < clip.x || pxx >= clip.x + clip.w) continue;
                row[pxx] = cv >= 255 ? (color & 0xffffffu) : gfx_mix(row[pxx], color, cv + (cv >> 7));
            }
        }
        pen += g->adv64;
    }
    return (int)((pen + 32u) >> 6);
}
int font_draw_box(Surface *s, Rect box, const char *str, const Face *f, uint32_t color, int align)
{
    int w = font_width(f, str), x = box.x;
    if (align == FA_CENTER) x = box.x + (box.w - w) / 2; else if (align == FA_RIGHT) x = box.x + box.w - w;
    int baseline = box.y + (box.h + f->cap) / 2;
    font_draw(s, box, x, baseline, str, f, color);
    return w;
}
void font_ellipsize(char *out, size_t n, const char *str, const Face *f, int maxw)
{
    if (n == 0) return;
    size_t l = strlen(str); if (l >= n) l = n - 1;
    if (l < 120 && font_width(f, str) <= maxw) { memcpy(out, str, l); out[l] = 0; return; }
    if (l > 120) l = 120;
    while (l > 0) {
        char tmp[128]; memcpy(tmp, str, l); memcpy(tmp + l, "...", 4);
        if (l + 3 < n && font_width(f, tmp) <= maxw) { memcpy(out, tmp, l + 4); return; }
        l--;
    }
    out[0] = 0;
}
int font_wrap(const char *str, const Face *f, int maxw, char lines[][96], int maxlines)
{
    int n = 0; char cur[96] = ""; const char *p = str;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *e = p; while (*e && *e != ' ') e++;
        char word[96]; size_t wl = (size_t)(e - p); if (wl > sizeof word - 1) wl = sizeof word - 1; memcpy(word, p, wl); word[wl] = 0;
        char trial[200]; snprintf(trial, sizeof trial, "%s%s%s", cur, cur[0] ? " " : "", word);
        if (!cur[0] || font_width(f, trial) <= maxw) snprintf(cur, sizeof cur, "%.95s", trial);
        else {
            if (n == maxlines - 1) {                                    /* out of lines: this one ends with an ellipsis */
                char rest[200]; snprintf(rest, sizeof rest, "%s %s", cur, p); font_ellipsize(lines[n], 96, rest, f, maxw); return n + 1;
            }
            snprintf(lines[n], 96, "%s", cur); n++; snprintf(cur, sizeof cur, "%s", word);
        }
        p = e;
    }
    if (cur[0] && n < maxlines) { font_ellipsize(lines[n], 96, cur, f, maxw); n++; }
    return n;
}
