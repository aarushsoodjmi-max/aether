/* Aether UI toolkit: client-side window + widgets on top of the display protocol (proto.h), gfx.h, font.h and the
 * Aurora theme. One window per UiWindow/process. Widgets: label, button, toggle (switch), separator, text entry, and
 * custom-draw widgets. Retained mode, software-drawn; every state change repaints the window and sends one COMMIT. */
#ifndef AETHER_UI_H
#define AETHER_UI_H
#include "font.h"
#include "gfx.h"
#include "icons.h"
#include "proto.h"
#include "theme.h"

typedef struct UiWindow UiWindow;
typedef void (*UiCallback)(UiWindow *w, int widget, void *user);
typedef void (*UiDraw)(UiWindow *w, int widget, Surface *s);          /* custom widgets paint themselves into the window surface */
typedef enum { UI_LABEL, UI_BUTTON, UI_TOGGLE, UI_SEPARATOR, UI_ENTRY, UI_CUSTOM } UiType;
typedef struct {
    UiType type; Rect r; char text[96]; uint32_t fg, bg; int page, hover, pressed, checked, primary, weight, px, align, disabled, interactive;
    UiCallback cb; void *user; UiDraw draw; const char *hint;
} UiWidget;
#define UI_MAX_WIDGETS 160
#define UI_ALL_PAGES (-1)

struct UiWindow {
    int sock, memfd; uint32_t id; int kind, w, h, flags; uint32_t *px; size_t size; Surface s;
    char title[48]; UiWidget wid[UI_MAX_WIDGETS]; int nwid, page; uint32_t bg;
    int screen_w, screen_h, workspace, nws; int closed;
    int focus;                          /* widget with keyboard focus (text entry), or -1 */
    int mods, clicks;                   /* modifier bits of the last key event (KMOD_*: 1 shift, 2 ctrl, 4 alt); click count of the last press (1,2,3) */
    void (*on_pointer)(UiWindow *, int kind, int x, int y, int button, int clicks);   /* raw pointer events: kind 0 motion,1 press,2 release,3 enter,4 leave,5 wheel (button = notches) */
    int press_dismissed;                /* the last press that reached us also dismissed a popup (see E_POINTER v4) */
    int tick_ms; void (*on_tick)(UiWindow *); void (*on_workspace)(UiWindow *); void (*on_key)(UiWindow *, int key, int ascii, int pressed); void *user;
    int watch_fd; void (*on_watch)(UiWindow *);                      /* poll one extra descriptor in the event loop */
    void (*on_resize)(UiWindow *, int nw, int nh);                    /* compositor asks for a new size (maximise/restore) */
    void (*on_screen)(UiWindow *);                                    /* display size changed (screen_w/h updated) */
    void (*on_task)(UiWindow *, int phase, uint32_t id, int flags, const char *title);   /* panel: window list updates */
    void (*on_hotkey)(UiWindow *, int id);
    void (*on_hover)(UiWindow *, int widget);                         /* the widget under the pointer changed (-1: none) */
    void (*on_context)(UiWindow *, int widget, int x, int y);         /* right-click (button 3 pressed and released over the same interactive widget, or over none = -1); x,y in window coordinates.
                                                                       * Not sent when that press only dismissed a popup (right-clicking away from an open menu just closes it) */
    int ctx_armed, ctx_widget;                                        /* toolkit state for on_context */
};

int ui_connect(UiWindow *w, char *err, size_t errlen);                       /* connect + HELLO; fills screen_w/h */
int ui_create(UiWindow *w, const char *title, int kind, int cw, int ch, int px, int py, char *err, size_t errlen);
int ui_label(UiWindow *w, int x, int y, const char *text, uint32_t fg, int page);                                   /* body text */
int ui_label_ex(UiWindow *w, int x, int y, const char *text, uint32_t fg, int weight, int px, int page);
int ui_button(UiWindow *w, Rect r, const char *text, UiCallback cb, void *user, int page);
int ui_button_primary(UiWindow *w, Rect r, const char *text, UiCallback cb, void *user, int page);
int ui_toggle(UiWindow *w, int x, int y, const char *text, int checked, UiCallback cb, void *user, int page);
int ui_separator(UiWindow *w, Rect r, int page);
int ui_entry(UiWindow *w, Rect r, const char *hint, UiCallback on_change, void *user, int page);                    /* single-line text entry; takes focus */
int ui_custom(UiWindow *w, Rect r, UiDraw draw, UiCallback on_click, void *user, int interactive, int page);
void ui_clear(UiWindow *w);                                                   /* remove all widgets (before re-laying out) */
void ui_set_text(UiWindow *w, int widget, const char *text);
void ui_set_colors(UiWindow *w, int widget, uint32_t fg, uint32_t bg);
void ui_set_page(UiWindow *w, int page);
void ui_redraw(UiWindow *w);                                                  /* repaint + COMMIT */
void ui_switch_workspace(UiWindow *w, int index);
void ui_set_opacity(UiWindow *w, int alpha);                                  /* 0..255, for fades */
int ui_resize(UiWindow *w, int nw, int nh, char *err, size_t errlen);         /* allocate a new buffer and hand it to the compositor */
void ui_task_activate(UiWindow *w, uint32_t window_id);                       /* panel only: raise/restore, or minimise when already focused */
void ui_task_raise(UiWindow *w, uint32_t window_id);                          /* panel only: raise/restore/focus, never minimise */
void ui_task_close(UiWindow *w, uint32_t window_id);                          /* panel only: ask the window to close (M_TASK_CLOSE) */
void ui_run(UiWindow *w);                                                     /* event loop until closed */
void ui_destroy(UiWindow *w);
long ui_now_ms(void);
#endif
