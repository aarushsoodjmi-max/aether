/* Context-menu model: what each right-click menu contains, how a chosen item is encoded for the owning process, where the
 * menu is placed and how the keyboard moves through it. Pure logic (no sockets, no drawing): unit-tested in
 * tests/test_gui_unit.c and used by shell.c (`aether-shell --menu`).
 *
 * Flow: the owner (panel or desktop surface) spawns the menu popup like the launcher, with a pipe on fd 9. The popup
 * writes ONE command line to the pipe when an item is chosen and exits; the owner parses it with cm_parse_command() and
 * performs the action itself (it alone may talk to the compositor as a panel, or knows the desktop state). */
#ifndef AETHER_CTXMENU_H
#define AETHER_CTXMENU_H
#include <stddef.h>
#include <stdint.h>

typedef enum { CM_DESKTOP = 0, CM_SHORTCUT, CM_TASK } CmKind;           /* what was right-clicked */
typedef enum { CM_ACT_NONE = 0, CM_ACT_OPEN, CM_ACT_REFRESH, CM_ACT_ACTIVATE, CM_ACT_CLOSE } CmActKind;
#define CM_MAX_ITEMS 8
#define CM_NAME_MAX 48                                                   /* same bound as AppEntry.name */
typedef struct { char label[CM_NAME_MAX]; char cmd[CM_NAME_MAX + 16]; int separator; } CmItem;   /* cmd is "" for a separator */
typedef struct { CmActKind kind; char name[CM_NAME_MAX]; uint32_t id; } CmAction;

int cm_kind_parse(const char *s, CmKind *out);                           /* "desktop" | "shortcut" | "task" -> 1, else 0 */
const char *cm_kind_name(CmKind k);

/* Build the items. `arg` is the application name (CM_SHORTCUT) or the decimal window id (CM_TASK); `runnable` lists the names of
 * applications that really exist (appmeta: exec present). The desktop menu offers Terminal, Files and Settings only when they
 * are in that list, then Refresh. Returns the item count (0 for an invalid argument: the caller opens no menu). */
size_t cm_build(CmKind kind, const char *arg, const char *const *runnable, size_t nrun, CmItem out[CM_MAX_ITEMS]);

/* Parse one command line from a menu popup ("open NAME", "refresh", "activate ID", "close ID"; trailing CR/LF allowed).
 * Returns 1 and fills *out, or 0 for anything malformed (unknown verb, control characters, bad / zero / overflowing id). */
int cm_parse_command(const char *line, CmAction *out);

int cm_item_h(const CmItem *it);                                         /* row height: item or separator */
int cm_menu_h(const CmItem *items, size_t n);                            /* total window height including padding */
int cm_item_y(const CmItem *items, size_t idx);                          /* top of row idx inside the menu window */
/* Top-left of the menu for a click at (px, py) on a sw x sh screen: below-right of the pointer, flipped above it / pulled left
 * when that would leave the screen, never above `top` (the panel). */
void cm_place(int px, int py, int mw, int mh, int sw, int sh, int top, int *x, int *y);
/* Keyboard: next selectable row from `cur` (-1 = none) in direction dir (+1 / -1), skipping separators, wrapping. -1 if none. */
int cm_step(const CmItem *items, size_t n, int cur, int dir);
#endif
