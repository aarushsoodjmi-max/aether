/* Display integration: Linux KMS "dumb buffer" scanout via raw DRM ioctls (no libdrm, no GPU acceleration).
 * Works with any DRM driver that supports dumb buffers (bochs, virtio-gpu, i915, ...). Only bochs is
 * verified (QEMU); real GPUs such as the Intel HD 520 (i915) are UNTESTED. */
#ifndef AETHER_DISPLAY_H
#define AETHER_DISPLAY_H
#include <stddef.h>
#include <stdint.h>
#include "gfx.h"

typedef struct {
    int fd; int w, h; uint32_t pitch; size_t size; uint32_t *map;
    uint32_t crtc_id, fb_id, handle; char driver[32]; char card[32]; int headless;
} Display;

int display_open(Display *d, char *err, size_t errlen);              /* 0 ok */
void display_present(Display *d, const Surface *src);                /* copy a full frame to the scanout buffer */
Surface display_surface(const Display *d);                           /* view of the scanout buffer */
void display_close(Display *d);
void display_present_rect(Display *d, const Surface *src, Rect r);   /* copy only r (clipped); falls back gracefully on drivers without partial dirty */
/* Software-only display for tests and screenshots: no DRM, the "scanout" is plain memory (see compositor --headless). */
int display_open_headless(Display *d, int w, int h);
int display_resize_headless(Display *d, int w, int h);
#endif
