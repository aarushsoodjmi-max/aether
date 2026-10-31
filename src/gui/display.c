#define _GNU_SOURCE
#include "display.h"
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

static void seterr(char *e, size_t n, const char *fmt, const char *a) { if (e) snprintf(e, n, fmt, a, strerror(errno)); }
/* DRM_MODE_CONNECTED lives in libdrm, not in the kernel uapi header */
#define DRM_CONNECTED 1
static uint64_t P(const void *p) { return (uint64_t)(uintptr_t)p; }

int display_open(Display *d, char *err, size_t errlen)
{
    memset(d, 0, sizeof *d); d->fd = -1;
    for (int i = 0; i < 4 && d->fd < 0; i++) {
        snprintf(d->card, sizeof d->card, "/dev/dri/card%d", i);
        d->fd = open(d->card, O_RDWR | O_CLOEXEC);
    }
    if (d->fd < 0) { seterr(err, errlen, "no DRM device (%s: %s); is the GPU driver module loaded?", "/dev/dri/card*"); return -1; }
    ioctl(d->fd, DRM_IOCTL_SET_MASTER, 0);                                   /* fine if it fails: we may already be master */
    struct drm_mode_card_res res; memset(&res, 0, sizeof res);
    if (ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) { seterr(err, errlen, "%s: GETRESOURCES: %s", d->card); goto fail; }
    uint32_t *crtcs = calloc(res.count_crtcs + 1, 4), *conns = calloc(res.count_connectors + 1, 4), *encs = calloc(res.count_encoders + 1, 4), *fbs = calloc(res.count_fbs + 1, 4);
    res.crtc_id_ptr = P(crtcs); res.connector_id_ptr = P(conns); res.encoder_id_ptr = P(encs); res.fb_id_ptr = P(fbs);
    if (!crtcs || !conns || !encs || !fbs || ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) { seterr(err, errlen, "%s: GETRESOURCES(2): %s", d->card); free(crtcs); free(conns); free(encs); free(fbs); goto fail; }
    int ok = 0; struct drm_mode_modeinfo mode; uint32_t conn_id = 0, crtc_id = 0;
    for (uint32_t c = 0; c < res.count_connectors && !ok; c++) {
        struct drm_mode_get_connector gc; memset(&gc, 0, sizeof gc); gc.connector_id = conns[c];
        if (ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) != 0 || gc.connection != DRM_CONNECTED || gc.count_modes == 0) continue;
        struct drm_mode_modeinfo *modes = calloc(gc.count_modes, sizeof *modes); uint32_t nm = gc.count_modes;
        memset(&gc, 0, sizeof gc); gc.connector_id = conns[c]; gc.count_modes = nm; gc.modes_ptr = P(modes);
        if (modes && ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) == 0 && gc.count_modes > 0) {
            mode = modes[0];                                                 /* first mode is the preferred one */
            conn_id = conns[c];
            if (gc.encoder_id) { struct drm_mode_get_encoder ge; memset(&ge, 0, sizeof ge); ge.encoder_id = gc.encoder_id; if (ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &ge) == 0) crtc_id = ge.crtc_id; }
            if (!crtc_id) for (uint32_t e = 0; e < res.count_encoders && !crtc_id; e++) {
                struct drm_mode_get_encoder ge; memset(&ge, 0, sizeof ge); ge.encoder_id = encs[e];
                if (ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &ge) == 0) for (uint32_t k = 0; k < res.count_crtcs; k++) if (ge.possible_crtcs & (1u << k)) { crtc_id = crtcs[k]; break; }
            }
            if (crtc_id) ok = 1;
        }
        free(modes);
    }
    free(crtcs); free(conns); free(encs); free(fbs);
    if (!ok) { if (err) snprintf(err, errlen, "%s: no connected display with a usable CRTC", d->card); goto fail; }
    d->w = mode.hdisplay; d->h = mode.vdisplay; d->crtc_id = crtc_id;
    struct drm_mode_create_dumb cd; memset(&cd, 0, sizeof cd); cd.width = (uint32_t)d->w; cd.height = (uint32_t)d->h; cd.bpp = 32;
    if (ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) { seterr(err, errlen, "%s: CREATE_DUMB: %s", d->card); goto fail; }
    d->handle = cd.handle; d->pitch = cd.pitch; d->size = (size_t)cd.size;
    struct drm_mode_fb_cmd fb; memset(&fb, 0, sizeof fb); fb.width = (uint32_t)d->w; fb.height = (uint32_t)d->h; fb.pitch = cd.pitch; fb.bpp = 32; fb.depth = 24; fb.handle = cd.handle;
    if (ioctl(d->fd, DRM_IOCTL_MODE_ADDFB, &fb) != 0) { seterr(err, errlen, "%s: ADDFB: %s", d->card); goto fail; }
    d->fb_id = fb.fb_id;
    struct drm_mode_map_dumb md; memset(&md, 0, sizeof md); md.handle = cd.handle;
    if (ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &md) != 0) { seterr(err, errlen, "%s: MAP_DUMB: %s", d->card); goto fail; }
    void *m = mmap(NULL, d->size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, (off_t)md.offset);
    if (m == MAP_FAILED) { seterr(err, errlen, "%s: mmap: %s", d->card); goto fail; }
    d->map = m; memset(d->map, 0, d->size);
    struct drm_mode_crtc sc; memset(&sc, 0, sizeof sc); sc.crtc_id = crtc_id; sc.fb_id = d->fb_id; sc.set_connectors_ptr = P(&conn_id); sc.count_connectors = 1; sc.mode_valid = 1; sc.mode = mode;
    if (ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &sc) != 0) { seterr(err, errlen, "%s: SETCRTC: %s", d->card); goto fail; }
    char link[96], target[128]; const char *base = strrchr(d->card, '/'); snprintf(link, sizeof link, "/sys/class/drm/%s/device/driver", base ? base + 1 : "card0");
    ssize_t n = readlink(link, target, sizeof target - 1); if (n > 0) { target[n] = 0; const char *b = strrchr(target, '/'); snprintf(d->driver, sizeof d->driver, "%.31s", b ? b + 1 : target); } else snprintf(d->driver, sizeof d->driver, "unknown");
    return 0;
fail:
    display_close(d); return -1;
}
Surface display_surface(const Display *d) { Surface s = { d->map, d->w, d->h, (int)(d->pitch / 4) }; return s; }
void display_present(Display *d, const Surface *src)
{
    Surface dst = display_surface(d); gfx_blit(&dst, 0, 0, src, rect_make(0, 0, src->w, src->h));
    /* Drivers with shadow buffers (bochs, virtio-gpu, ...) copy to scanout memory only when told the framebuffer
     * changed. Drivers that scan out directly may answer ENOSYS/EINVAL; that is fine. */
    struct drm_mode_fb_dirty_cmd dirty; memset(&dirty, 0, sizeof dirty); dirty.fb_id = d->fb_id;
    ioctl(d->fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
}
void display_present_rect(Display *d, const Surface *src, Rect r)
{
    Surface dst = display_surface(d); r = rect_intersect(r, rect_make(0, 0, src->w, src->h));
    if (r.w <= 0 || r.h <= 0) return;
    gfx_blit(&dst, r.x, r.y, src, r);
    if (d->headless) return;
    struct drm_clip_rect clip = { (uint16_t)r.x, (uint16_t)r.y, (uint16_t)(r.x + r.w), (uint16_t)(r.y + r.h) };
    struct drm_mode_fb_dirty_cmd dirty; memset(&dirty, 0, sizeof dirty); dirty.fb_id = d->fb_id; dirty.num_clips = 1; dirty.clips_ptr = P(&clip);
    if (ioctl(d->fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) != 0) { memset(&dirty, 0, sizeof dirty); dirty.fb_id = d->fb_id; ioctl(d->fd, DRM_IOCTL_MODE_DIRTYFB, &dirty); }
}
int display_open_headless(Display *d, int w, int h)
{
    memset(d, 0, sizeof *d); d->fd = -1; d->headless = 1; d->w = w; d->h = h; d->pitch = (uint32_t)w * 4; d->size = (size_t)d->pitch * (size_t)h;
    d->map = calloc(1, d->size); snprintf(d->driver, sizeof d->driver, "headless"); snprintf(d->card, sizeof d->card, "memory");
    return d->map ? 0 : -1;
}
int display_resize_headless(Display *d, int w, int h)
{
    if (!d->headless || w < 1 || h < 1) return -1;
    free(d->map); d->w = w; d->h = h; d->pitch = (uint32_t)w * 4; d->size = (size_t)d->pitch * (size_t)h; d->map = calloc(1, d->size); return d->map ? 0 : -1;
}
void display_close(Display *d)
{
    if (d->headless) { free(d->map); memset(d, 0, sizeof *d); d->fd = -1; return; }
    if (d->map) munmap(d->map, d->size);
    if (d->fd >= 0) { if (d->fb_id) { uint32_t id = d->fb_id; ioctl(d->fd, DRM_IOCTL_MODE_RMFB, &id); } close(d->fd); }
    memset(d, 0, sizeof *d); d->fd = -1;
}
