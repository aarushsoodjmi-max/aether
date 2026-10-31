/* Terminal emulator core for aether-terminal: a VT100/xterm-subset screen model. Pure logic (no I/O, no drawing), so it is
 * unit-tested on its own. Bytes from the shell's pty go in through vt_feed(); the app reads cells back to draw them and
 * collects the replies a terminal must send (cursor-position and device-attribute reports) with vt_take_reply().
 *
 * Supported: printable ASCII + UTF-8 decoding (the bitmap font draws non-ASCII as '?'), C0 controls (BS HT LF VT FF CR),
 * CSI cursor movement (A B C D E F G H f d), erase (J K X), insert/delete (L M P @), scroll region (r), save/restore (s u, ESC 7 8),
 * SGR (bold, underline, reverse, 16/256 colours, truecolour mapped to the 256 palette), DSR/DA replies, tab stops, auto-wrap
 * with xterm's deferred-wrap rule, alternate screen (?47 ?1047 ?1049), application cursor keys (?1), cursor visibility (?25),
 * scrollback. OSC/DCS/APC strings are consumed and ignored.
 * Not supported (ignored, never crash): double-width characters, combining marks, mouse reporting, bracketed paste,
 * character sets other than ASCII, origin mode, insert mode. */
#ifndef AETHER_TERM_VT_H
#define AETHER_TERM_VT_H
#include <stddef.h>
#include <stdint.h>

#define VT_DEFAULT 256                           /* fg/bg value meaning "the theme's default colour" */
enum { VA_BOLD = 1, VA_REVERSE = 2, VA_UNDERLINE = 4 };
typedef struct { uint16_t ch, fg, bg; uint8_t attr, pad; } VtCell;          /* fg/bg: 0..255 palette index or VT_DEFAULT */
typedef struct { VtCell *c; int len; int wrapped; } VtLine;                  /* scrollback line, trailing blanks trimmed */

typedef struct Vt {
    int cols, rows, alt;
    VtCell *grid, *alt_grid; uint8_t *wrap, *alt_wrap;                      /* active and the other screen; wrap[r] = row r continues on row r+1 */
    int cx, cy, wrap_pending, top, bot;                                      /* cursor, deferred wrap flag, scroll region (inclusive rows) */
    int save_cx, save_cy; uint16_t save_fg, save_bg; uint8_t save_attr;
    uint16_t fg, bg; uint8_t attr;
    int cursor_visible, app_cursor, autowrap;
    VtLine *sb; int sb_cap, sb_head, sb_n;                                   /* scrollback ring: oldest line at sb_head */
    uint8_t *tabs;
    int st, params[16], np, priv, inter, str_esc;                            /* parser */
    uint32_t u8; int u8_left;                                                /* UTF-8 decoder */
    char reply[256]; int reply_n;
} Vt;

Vt *vt_new(int cols, int rows, int scrollback_lines);                        /* NULL on bad size / out of memory */
void vt_free(Vt *v);
void vt_feed(Vt *v, const char *data, size_t n);
void vt_resize(Vt *v, int cols, int rows);
void vt_reset(Vt *v);
size_t vt_take_reply(Vt *v, char *out, size_t cap);                          /* bytes the terminal must write back to the pty */
const VtCell *vt_cell(const Vt *v, int row, int col);                        /* active screen cell (NULL if out of range) */
/* Absolute line addressing, for scrollback display and selection: line 0 = oldest scrollback line, lines sb_n .. sb_n+rows-1 = the screen. */
int vt_abs_lines(const Vt *v);
VtCell vt_abs_cell(const Vt *v, int line, int col);                          /* a blank default cell outside the line */
int vt_abs_wrapped(const Vt *v, int line);
/* Text of the inclusive range (l0,c0)..(l1,c1) as UTF-8; trailing blanks of each line trimmed, '\n' between lines (not after wrapped ones).
 * Returns the length (NUL-terminated, truncated to cap-1). */
size_t vt_text(const Vt *v, int l0, int c0, int l1, int c1, char *out, size_t cap);
int vt_word_bounds(const Vt *v, int line, int col, int *c0, int *c1);        /* 1 if (line,col) is on a word; bounds inclusive */
uint32_t vt_palette(int idx);                                                /* 0xRRGGBB for palette entries 0..255 (16 themed + cube + greys) */
#endif
