#define _GNU_SOURCE
#include "input.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define LONG_BITS (sizeof(long) * 8)
static int test_bit(int bit, const unsigned long *a) { return (int)((a[(size_t)bit / LONG_BITS] >> ((size_t)bit % LONG_BITS)) & 1); }

/* ---- coordinate conversion ---------------------------------------------------------------------------------- */
int input_abs_scale(int v, int vmin, int vmax, int size)
{
    if (size <= 1 || vmax <= vmin) return 0;
    if (v < vmin) v = vmin;
    if (v > vmax) v = vmax;
    long long num = (long long)(v - vmin) * (size - 1), den = vmax - vmin;
    return (int)((num * 2 + den) / (den * 2));            /* round to nearest */
}

/* ---- classification ------------------------------------------------------------------------------------------ */
static void classify(int fd, InDev *d)
{
    unsigned long ev[(EV_MAX + LONG_BITS) / LONG_BITS], key[(KEY_MAX + LONG_BITS) / LONG_BITS], rel[(REL_MAX + LONG_BITS) / LONG_BITS], abs[(ABS_MAX + LONG_BITS) / LONG_BITS];
    memset(ev, 0, sizeof ev); memset(key, 0, sizeof key); memset(rel, 0, sizeof rel); memset(abs, 0, sizeof abs);
    d->caps = 0; d->name[0] = 0;
    if (ioctl(fd, EVIOCGNAME(sizeof d->name), d->name) < 0) snprintf(d->name, sizeof d->name, "event device");
    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0) return;
    if (test_bit(EV_KEY, ev)) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
    if (test_bit(EV_REL, ev)) ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
    if (test_bit(EV_ABS, ev)) ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    if (test_bit(EV_KEY, ev) && (test_bit(KEY_A, key) || test_bit(KEY_ENTER, key))) d->caps |= DEV_KEYBOARD;
    if (test_bit(EV_REL, ev) && test_bit(REL_X, rel) && test_bit(REL_Y, rel)) d->caps |= DEV_REL;
    /* Absolute pointer: a tablet / touchscreen has X, Y and a button or touch. A touchpad (BTN_TOOL_FINGER) is NOT a
     * direct-pointing device: treating its finger coordinates as screen coordinates would make the pointer teleport,
     * so touchpads are refused rather than mishandled. */
    if (test_bit(EV_ABS, ev) && test_bit(ABS_X, abs) && test_bit(ABS_Y, abs) && (test_bit(BTN_LEFT, key) || test_bit(BTN_TOUCH, key)) && !test_bit(BTN_TOOL_FINGER, key)) {
        struct input_absinfo ax, ay;
        if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 && ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0 && ax.maximum > ax.minimum && ay.maximum > ay.minimum) {
            d->caps |= DEV_ABS; d->abs_min[0] = ax.minimum; d->abs_max[0] = ax.maximum; d->abs_val[0] = ax.value;
            d->abs_min[1] = ay.minimum; d->abs_max[1] = ay.maximum; d->abs_val[1] = ay.value; d->btn_left_native = test_bit(BTN_LEFT, key);
        }
    }
}
void input_dev_init_abs(InDev *d, int xmin, int xmax, int ymin, int ymax)
{
    d->caps |= DEV_ABS; d->abs_min[0] = xmin; d->abs_max[0] = xmax; d->abs_min[1] = ymin; d->abs_max[1] = ymax;
    d->abs_val[0] = xmin; d->abs_val[1] = ymin; d->btn_left_native = 1;
}

int input_rescan(Input *in)
{
    DIR *dir = opendir("/dev/input"); if (!dir) return 0; struct dirent *e; int added = 0;
    while (in->n < INPUT_MAX && (e = readdir(dir))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300]; snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
        struct stat st; int known = 0;
        if (fstat(fd, &st) != 0) { close(fd); continue; }
        for (int i = 0; i < in->n; i++) if (in->d[i].dev == (unsigned long)st.st_rdev) known = 1;
        if (known) { close(fd); continue; }
        InDev *d = &in->d[in->n]; memset(d, 0, sizeof *d); d->fd = fd; d->dev = (unsigned long)st.st_rdev;
        classify(fd, d);
        if (!d->caps) { close(fd); continue; }          /* not a keyboard / mouse / tablet (power button, lid switch, touchpad, ...) */
        in->n++; added++;
        printf("aether-compositor: input %s: \"%s\" [%s%s%s]", e->d_name, d->name, d->caps & DEV_KEYBOARD ? "keyboard " : "", d->caps & DEV_REL ? "relative-pointer " : "", d->caps & DEV_ABS ? "absolute-pointer" : "");
        if (d->caps & DEV_ABS) printf(" range x %d..%d y %d..%d", d->abs_min[0], d->abs_max[0], d->abs_min[1], d->abs_max[1]);
        printf("\n");
    }
    closedir(dir); return added;
}
int input_open(Input *in) { memset(in, 0, sizeof *in); input_rescan(in); return in->n; }
void input_remove(Input *in, int idx)
{
    if (idx < 0 || idx >= in->n) return;
    printf("aether-compositor: input device \"%s\" removed\n", in->d[idx].name);
    close(in->d[idx].fd);
    for (int i = idx; i + 1 < in->n; i++) in->d[i] = in->d[i + 1];
    in->n--;
}
void input_close(Input *in) { for (int i = 0; i < in->n; i++) close(in->d[i].fd); in->n = 0; }

/* ---- event translation --------------------------------------------------------------------------------------- */
static void frame_reset(InDev *d) { d->f_rx = d->f_ry = d->f_wheel = d->f_abs = d->f_n = 0; }
static int flush_frame(InDev *d, InEvent *out, int max)
{
    int n = 0; InEvent o;
    if ((d->f_rx || d->f_ry) && n < max) { memset(&o, 0, sizeof o); o.kind = IN_MOTION; o.dx = d->f_rx; o.dy = d->f_ry; out[n++] = o; }
    if (d->f_abs && (d->caps & DEV_ABS) && n < max) {
        memset(&o, 0, sizeof o); o.kind = IN_ABS; o.dx = d->abs_val[0]; o.dy = d->abs_val[1];
        o.x0 = d->abs_min[0]; o.x1 = d->abs_max[0]; o.y0 = d->abs_min[1]; o.y1 = d->abs_max[1]; out[n++] = o;
    }
    for (int i = 0; i < d->f_n && n < max; i++) out[n++] = d->f_ev[i];
    if (d->f_wheel && n < max) { memset(&o, 0, sizeof o); o.kind = IN_WHEEL; o.dy = d->f_wheel; out[n++] = o; }
    frame_reset(d); return n;
}
static void queue(InDev *d, InKind k, int code, int value)
{
    if (d->f_n >= INPUT_FRAME_MAX) return;
    InEvent o; memset(&o, 0, sizeof o); o.kind = k; o.code = code; o.value = value; d->f_ev[d->f_n++] = o;
    if (k == IN_BUTTON && code == BTN_LEFT) d->left_down = value != 0;
}
int input_feed(InDev *d, const struct input_event *ev, InEvent *out, int max)
{
    switch (ev->type) {
    case EV_KEY:
        if (ev->code >= BTN_MISC && ev->code < BTN_JOYSTICK) queue(d, IN_BUTTON, ev->code, ev->value);
        else if (ev->code == BTN_TOUCH && (d->caps & DEV_ABS) && !d->btn_left_native) queue(d, IN_BUTTON, BTN_LEFT, ev->value);   /* touchscreen / pen tip = primary button */
        else if (ev->code >= BTN_JOYSTICK && ev->code < KEY_OK) break;                    /* other BTN_* codes (gamepad, tool, stylus...) are not keyboard keys */
        else queue(d, IN_KEY, ev->code, ev->value);
        break;
    case EV_REL:
        if (ev->code == REL_X) d->f_rx += ev->value;
        else if (ev->code == REL_Y) d->f_ry += ev->value;
        else if (ev->code == REL_WHEEL) d->f_wheel += ev->value;
        break;
    case EV_ABS:
        if (!(d->caps & DEV_ABS)) break;
        if (ev->code == ABS_X) { d->abs_val[0] = ev->value; d->f_abs = 1; }
        else if (ev->code == ABS_Y) { d->abs_val[1] = ev->value; d->f_abs = 1; }
        break;
    case EV_SYN:
        if (ev->code == SYN_REPORT) return flush_frame(d, out, max);
        if (ev->code == SYN_DROPPED) {                  /* the kernel buffer overflowed: the frame is garbage; never leave the button stuck down */
            frame_reset(d); int n = 0;
            if (d->left_down && max > 0) { InEvent o; memset(&o, 0, sizeof o); o.kind = IN_BUTTON; o.code = BTN_LEFT; o.value = 0; out[n++] = o; d->left_down = 0; }
            return n;
        }
        break;
    default: break;
    }
    return 0;
}
int input_dev_read(InDev *d, InEvent *out, int max)
{
    int total = 0;
    for (int rounds = 0; rounds < 8 && total + INPUT_FRAME_MAX + 4 <= max; rounds++) {
        struct input_event ev[64]; ssize_t r = read(d->fd, ev, sizeof ev);
        if (r < 0) { if (errno == EAGAIN || errno == EINTR) break; return total ? total : -1; }
        if (r == 0) return total ? total : -1;
        for (size_t i = 0; i < (size_t)r / sizeof ev[0]; i++) total += input_feed(d, &ev[i], out + total, max - total);
        if ((size_t)r < sizeof ev) break;
    }
    return total;
}

static const char kLow[] = "\0\0" "1234567890-=\0\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char kUp[]  = "\0\0" "!@#$%^&*()_+\0\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
int keycode_to_ascii(int k, int shift)
{
    if (k == KEY_BACKSPACE) return '\b';
    if (k == KEY_ESC) return 27;
    if (k < 0 || k >= (int)sizeof kLow - 1) return 0;
    return (unsigned char)(shift ? kUp[k] : kLow[k]);
}
int keycode_is_modifier(int k, int *mod)
{
    switch (k) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: *mod = KMOD_SHIFT; return 1;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: *mod = KMOD_CTRL; return 1;
    case KEY_LEFTALT: case KEY_RIGHTALT: *mod = KMOD_ALT; return 1;
    case KEY_LEFTMETA: case KEY_RIGHTMETA: *mod = KMOD_SUPER; return 1;
    default: return 0;
    }
}
