/* Input integration: Linux evdev keyboards and pointing devices, normalised into InEvent.
 *
 * Pointer devices come in two kinds and the two must never be mixed up:
 *   relative  (PS/2 / USB mouse, QEMU "mouse")         -> IN_MOTION  dx,dy  = counts, added to the pointer position
 *   absolute  (QEMU usb-tablet / virtio-tablet, touch) -> IN_ABS     x,y    = raw device coordinates + the device's
 *                                                         range (x0..x1, y0..y1); input_abs_to_px() scales them to
 *                                                         the display exactly once and the pointer is *placed*, not moved.
 * Events of one device are delivered per evdev frame (SYN_REPORT) in a fixed order: motion, absolute position, then
 * buttons/keys in arrival order, then wheel. So a click is never applied before the move that preceded it. */
#ifndef AETHER_INPUT_H
#define AETHER_INPUT_H
#include <stddef.h>
#include <linux/input.h>

typedef enum { IN_KEY, IN_MOTION, IN_BUTTON, IN_WHEEL, IN_ABS } InKind;
typedef struct {
    InKind kind; int code; int value; int dx, dy;   /* KEY/BUTTON: code + value(1 down,0 up,2 repeat); WHEEL: dy = notches (+ = away from the user) */
    int x0, x1, y0, y1;                              /* IN_ABS only: dx,dy are raw coordinates within [x0,x1] x [y0,y1] */
} InEvent;

enum { DEV_KEYBOARD = 1, DEV_REL = 2, DEV_ABS = 4 };      /* capability bits */
#define INPUT_MAX 16
#define INPUT_FRAME_MAX 64
typedef struct {
    int fd; unsigned long dev; unsigned caps; char name[64];
    int abs_min[2], abs_max[2], abs_val[2];              /* absolute devices: range and the last known position */
    int btn_left_native;                                 /* the device itself reports BTN_LEFT (else BTN_TOUCH stands in for it) */
    /* frame being assembled */
    int f_rx, f_ry, f_wheel, f_abs, f_n; InEvent f_ev[INPUT_FRAME_MAX];
    int left_down;                                       /* last reported state of the primary button (for SYN_DROPPED resync) */
} InDev;
typedef struct { InDev d[INPUT_MAX]; int n; } Input;

int input_open(Input *in);                                  /* opens every usable /dev/input/event*; returns count */
int input_rescan(Input *in);                                /* hotplug: attach devices that appeared since; returns number added */
void input_remove(Input *in, int idx);                      /* device vanished (read error / hang-up) */
void input_close(Input *in);
/* Drain one device: returns events produced (0 if none), -1 if the device is gone. Reads until the fd would block (bounded). */
int input_dev_read(InDev *d, InEvent *out, int max);
/* The pure translator behind input_dev_read: feed ONE kernel event, collect frame output on SYN_REPORT. Used by the unit tests. */
int input_feed(InDev *d, const struct input_event *ev, InEvent *out, int max);
void input_dev_init_abs(InDev *d, int xmin, int xmax, int ymin, int ymax);   /* for tests and for rescan after EVIOCGABS */
/* Scale a raw absolute coordinate to a pixel in [0, size-1]: rounds to nearest, monotonic, min -> 0 and max -> size-1. */
int input_abs_scale(int v, int vmin, int vmax, int size);
int keycode_to_ascii(int keycode, int shift);               /* US layout; 0 if the key has no character */
#define KMOD_SHIFT 1
#define KMOD_CTRL 2
#define KMOD_ALT 4
#define KMOD_SUPER 8
int keycode_is_modifier(int keycode, int *mod);
#endif
