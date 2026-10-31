/* Aether display protocol (client <-> compositor). AF_UNIX SOCK_SEQPACKET, one fixed-size Msg per packet.
 * Window pixels live in a client-created memfd (XRGB8888, w*h*4 bytes) passed with SCM_RIGHTS in CREATE_WINDOW.
 * The compositor never trusts client values: sizes are bounded and the mapping size is checked against the fd. */
#ifndef AETHER_PROTO_H
#define AETHER_PROTO_H
#include <stdint.h>
#include <sys/types.h>

#define AETHER_DISPLAY_SOCK "/run/aether/display.sock"
#define AETHER_PROTO_VERSION 1
#define AETHER_MAX_WIN_W 4096
#define AETHER_MAX_WIN_H 4096
#define AETHER_WORKSPACES 4
#define PANEL_HEIGHT 38

enum { WK_NORMAL = 0, WK_PANEL = 1, WK_NOTIFY = 2, WK_POPUP = 3, WK_DESKTOP = 4 };   /* DESKTOP: the background surface (wallpaper + icons), below all windows */
enum { WF_RESIZABLE = 1, WF_CENTER = 2, WF_TRANSPARENT = 4 };   /* CREATE_WINDOW v5: re-creatable at a new size; centred placement; start fully transparent (for fade-in) */
enum {   /* client -> compositor */
    M_HELLO = 1,         /* v0 = protocol version */
    M_CREATE_WINDOW,     /* v0=w v1=h v2=kind v3=x v4=y (popup position); text=title; fd = memfd */
    M_COMMIT,            /* window; v0..v3 = damage rect (informational; compositor recomposes) */
    M_DESTROY,           /* window */
    M_SWITCH_WORKSPACE,  /* v0 = index */
    M_SET_OPACITY,       /* window; v0 = 0..255 (popups/notifications fade) */
    M_SET_BUFFER,        /* window; v0=w v1=h; fd = new sealed memfd. Answers a resize request or follows a screen change */
    M_TASK_ACTIVATE      /* window = target; panel clients only: raise/restore it, or minimise it when already focused */
};
enum {   /* compositor -> client */
    E_SCREEN = 100,      /* v0=screen w v1=screen h v2=current workspace v3=workspace count */
    E_CONFIGURE,         /* window assigned/moved: v0=x v1=y v2=w v3=h; v4 = 1 asks the client to answer with M_SET_BUFFER of that size */
    E_KEY,               /* window; v0=keycode v1=ascii v2=pressed v3=modifiers(1 shift,2 ctrl,4 alt) */
    E_POINTER,           /* window; v0=kind(0 motion,1 press,2 release,3 enter,4 leave,5 wheel) v1=x v2=y v3=button (wheel: notches, + = up) v4=popups this press dismissed v5=click count (press only: 1,2,3) */
    E_CLOSE,             /* window: user asked to close it (or popup lost focus) */
    E_WORKSPACE,         /* v0 = current workspace, v1 = count */
    E_TASK,              /* to panels: v0 = phase (0 begin, 1 item, 2 end); item: window = id, v1 = flags (1 focused, 2 minimised), text = title */
    E_HOTKEY             /* to panels: v0 = 1 (Super tapped: toggle the launcher), 2 (Ctrl+Alt+T: open the terminal) */
};
typedef struct { uint32_t type; uint32_t window; int32_t v[6]; char text[48]; } Msg;

int proto_send(int fd, const Msg *m, int passfd);                 /* returns 0, -1 on error */
int proto_recv(int fd, Msg *m, int *recvfd);                      /* 1 got message, 0 EOF, -1 would-block/error (errno set) */
#endif
