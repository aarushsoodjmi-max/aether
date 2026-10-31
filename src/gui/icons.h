/* Aether icon family: original vector glyphs drawn with anti-aliased strokes. One 24-unit design grid, a uniform
 * ~1.8 unit stroke with round caps/joins, and a two-tone scheme (main stroke + accent). Sizes scale from the grid. */
#ifndef AETHER_ICONS_H
#define AETHER_ICONS_H
#include "gfx.h"
typedef enum {
    ICON_NONE = 0, ICON_AETHER, ICON_SETTINGS, ICON_SEARCH, ICON_CLOSE, ICON_MINIMIZE, ICON_MAXIMIZE, ICON_RESTORE,
    ICON_BELL, ICON_NETWORK, ICON_BATTERY, ICON_INFO, ICON_DISPLAY, ICON_PACKAGE, ICON_APP, ICON_CHECK, ICON_WARNING, ICON_ERROR, ICON_COUNT
} IconId;
typedef enum { IS_DEFAULT, IS_MUTED, IS_HOVER, IS_ACTIVE, IS_DISABLED } IconState;

IconId icon_by_name(const char *name);                          /* "settings" -> ICON_SETTINGS; ICON_NONE if unknown */
uint32_t icon_main_color(IconState st);
uint32_t icon_accent_color(IconState st);
/* level: battery 0..100; network 0 = link down (draws a slash); otherwise ignored (-1). */
void icon_draw_ex(Surface *s, IconId id, int x, int y, int size, uint32_t main, uint32_t accent, int level);
void icon_draw(Surface *s, IconId id, int x, int y, int size, IconState st);
/* Application tile: rounded gradient box with the app's glyph, or its initial when it has no built-in glyph. */
void icon_app_tile(Surface *s, Rect box, IconId id, const char *app_name, int hover, int selected);
#endif
