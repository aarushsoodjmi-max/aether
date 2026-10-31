/* Aether Aurora wallpaper: rendered procedurally (no image asset) for whatever size it is given, so it always
 * matches the real display resolution. Deterministic. Cost is one pass over the surface plus a few thin arcs. */
#ifndef AETHER_WALLPAPER_H
#define AETHER_WALLPAPER_H
#include "gfx.h"
void wallpaper_render(Surface *dst);
#endif
