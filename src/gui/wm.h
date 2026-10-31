/* Aether window manager + software compositor core. Pure logic over client pixel buffers:
 * no sockets, no display, no input devices. The compositor process feeds it events and presents the result. */
#ifndef AETHER_WM_H
#define AETHER_WM_H
#include <stddef.h>
#include <stdint.h>
#include "gfx.h"
#include "proto.h"
#include "theme.h"

#define WM_MAX_WINDOWS 64
#define WM_TITLE_H TH_TITLE_H
/* Interactive resize. The grab bands lie INSIDE the visible frame (plus its 1 px outline), never outside it, so a window
 * cannot catch the pointer where nothing is painted. Corners are larger than edges so they are easy to hit. */
#define WM_RESIZE_BAND 5
#define WM_RESIZE_CORNER 14
#define WM_MIN_W 320                       /* smallest client area a resize can produce (a window already smaller keeps its own size as the floor) */
#define WM_MIN_H 200
#define WM_RESIZE_TIMEOUT_MS 1500          /* a client that never answers a resize request must not lock the window forever */
enum { WM_EDGE_L = 1, WM_EDGE_R = 2, WM_EDGE_T = 4, WM_EDGE_B = 8 };
enum { WM_HIT_NONE = 0, WM_HIT_CLIENT, WM_HIT_TITLE, WM_HIT_EDGE, WM_HIT_CLOSE, WM_HIT_MAX, WM_HIT_MIN };   /* what the pointer is over; controls stay >= CLOSE */
typedef struct {
    uint32_t id; int client, kind, x, y, w, h, ws; char title[48];
    uint32_t *buf; size_t bufsize; int used;
    int flags, minimized, maximized, alpha;          /* alpha 0..255 */
    Rect restore;                                    /* geometry to return to when un-maximising */
    int pending;                                     /* 0 none, 1 maximise requested, 2 restore requested, 3 interactive resize requested */
    int pend_x, pend_y, pend_w, pend_h; long pend_ms;
    int rs_edges, rs_follow, rs_right, rs_bottom;    /* interactive resize: edges being dragged, "ask again after the reply", fixed right/bottom anchors */
    Rect rs_want;                                    /* geometry the pointer currently asks for */
} Win;
typedef void (*WmEmit)(void *ctx, int client, const Msg *m);     /* deliver an event to a client */
typedef struct {
    int sw, sh, ws, nws;
    Win wins[WM_MAX_WINDOWS]; int z[WM_MAX_WINDOWS], nz;          /* z[]: indices of NORMAL windows, bottom to top */
    uint32_t next_id; int ptr_x, ptr_y, focus, grab, hover, drag, drag_dx, drag_dy, mods, dirty;
    int rsz, rsz_edges, rsz_px, rsz_py; Rect rsz_orig;            /* window being edge-resized (id, WM_EDGE_* mask, pointer position and client rect at the start) */
    int cursor_dirty, hot_win, hot_btn, press_win, press_btn, super_down, super_used;
    long now_ms; int clock_on;                                    /* monotonic time fed by the compositor; click counting is off until the first wm_set_time() */
    long click_ms; int click_x, click_y, click_win, click_n;      /* previous primary press: for double/triple click detection */
    int btn_mask, caps;                                           /* pointer buttons currently held (bit0 left, bit1 right, bit2 middle); Caps Lock state */
    WmEmit emit; void *ctx;
} Wm;

void wm_init(Wm *w, int sw, int sh, WmEmit emit, void *ctx);
/* Takes ownership of nothing: buf must stay valid until the window is removed. Returns window id or 0. */
uint32_t wm_add_window(Wm *w, int client, int kind, int cw, int ch, int px, int py, const char *title, uint32_t *buf, size_t bufsize);
void wm_remove_window(Wm *w, uint32_t id);
int wm_remove_client(Wm *w, int client, void (*unmap)(uint32_t *buf, size_t size));   /* returns windows removed */
void wm_pointer_move(Wm *w, int dx, int dy);                      /* relative device: add counts to the pointer position */
void wm_pointer_to(Wm *w, int x, int y);                          /* absolute device: place the pointer at display pixel (x, y) (clamped) */
void wm_button(Wm *w, int down);                                  /* left button */
void wm_button_ex(Wm *w, int button, int down);                   /* button 1 left, 2 right, 3 middle */
void wm_wheel(Wm *w, int notches);                                /* scroll wheel: + = away from the user (up) */
void wm_set_time(Wm *w, long now_ms);                             /* enables click counting (E_POINTER press v5 = 1/2/3 clicks) */
void wm_key(Wm *w, int keycode, int value);                       /* value 1 down, 0 up, 2 repeat */
void wm_switch_workspace(Wm *w, int index);
void wm_set_flags(Wm *w, uint32_t id, int flags);                 /* WF_* (applied after creation; WF_CENTER re-places) */
void wm_set_opacity(Wm *w, uint32_t id, int alpha);
/* Swap in a new client buffer (client answered a resize request, or re-created itself after a screen change). */
int wm_replace_buffer(Wm *w, uint32_t id, int nw, int nh, uint32_t *buf, size_t size, void (*unmap)(uint32_t *buf, size_t size));
void wm_task_activate(Wm *w, uint32_t id);                        /* panel task click */
void wm_resize_screen(Wm *w, int sw, int sh);                     /* new display size: re-clamp, tell every client (E_SCREEN) */
void wm_compose_scene(Wm *w, Surface *dst);                        /* everything except the pointer; clears w->dirty */
void wm_compose(Wm *w, Surface *dst);                              /* scene + pointer */
void wm_draw_cursor(Surface *s, int x, int y);
Rect wm_cursor_rect(const Wm *w);
Win *wm_find(Wm *w, uint32_t id);
/* What is under display pixel (px, py) according to the same hit test that routes real clicks. Returns WM_HIT_*; *win gets the
 * window id (0 for none) and *edges the WM_EDGE_* mask when the part is WM_HIT_EDGE. Used by the regression tests. */
int wm_hit_test(Wm *w, int px, int py, uint32_t *win, int *edges);
#endif
