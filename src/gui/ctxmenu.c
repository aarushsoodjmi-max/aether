#include "ctxmenu.h"
#include "theme.h"
#include <stdio.h>
#include <string.h>

int cm_kind_parse(const char *s, CmKind *out)
{
    if (!s) return 0;
    if (!strcmp(s, "desktop")) { *out = CM_DESKTOP; return 1; }
    if (!strcmp(s, "shortcut")) { *out = CM_SHORTCUT; return 1; }
    if (!strcmp(s, "task")) { *out = CM_TASK; return 1; }
    return 0;
}
const char *cm_kind_name(CmKind k) { return k == CM_SHORTCUT ? "shortcut" : (k == CM_TASK ? "task" : "desktop"); }

static int valid_name(const char *s)
{
    size_t n = s ? strlen(s) : 0; if (n == 0 || n >= CM_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 32 || s[i] == 127) return 0;
    return 1;
}
static int parse_id(const char *s, uint32_t *out)                        /* decimal, 1 .. UINT32_MAX, digits only */
{
    if (!s || !*s || strlen(s) > 10) return 0;
    unsigned long long v = 0;
    for (const char *p = s; *p; p++) { if (*p < '0' || *p > '9') return 0; v = v * 10 + (unsigned long long)(*p - '0'); }
    if (v == 0 || v > 0xffffffffull) return 0;
    *out = (uint32_t)v; return 1;
}
static void put(CmItem *it, const char *label, const char *fmt, const char *arg)
{
    memset(it, 0, sizeof *it);
    snprintf(it->label, sizeof it->label, "%s", label);
    if (arg) snprintf(it->cmd, sizeof it->cmd, fmt, arg); else snprintf(it->cmd, sizeof it->cmd, "%s", fmt);
}

size_t cm_build(CmKind kind, const char *arg, const char *const *runnable, size_t nrun, CmItem out[CM_MAX_ITEMS])
{
    size_t n = 0; uint32_t id;
    if (kind == CM_DESKTOP) {
        static const char *const apps[3] = { "Terminal", "Files", "Settings" };
        for (size_t a = 0; a < 3; a++)
            for (size_t i = 0; i < nrun; i++) if (runnable && runnable[i] && !strcmp(runnable[i], apps[a])) { put(&out[n++], apps[a], "open %s", apps[a]); break; }
        if (n > 0) { memset(&out[n], 0, sizeof out[n]); out[n++].separator = 1; }
        put(&out[n++], "Refresh", "refresh", NULL);
    } else if (kind == CM_SHORTCUT) {
        if (!valid_name(arg)) return 0;
        put(&out[n++], "Open", "open %s", arg);
    } else if (kind == CM_TASK) {
        if (!parse_id(arg, &id)) return 0;
        put(&out[n++], "Activate", "activate %s", arg);
        put(&out[n++], "Close", "close %s", arg);
    }
    return n;
}

int cm_parse_command(const char *line, CmAction *out)
{
    if (!line || !out) return 0;
    char b[CM_NAME_MAX + 16]; size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    if (n == 0 || n >= sizeof b) return 0;
    memcpy(b, line, n); b[n] = 0;
    memset(out, 0, sizeof *out);
    if (!strcmp(b, "refresh")) { out->kind = CM_ACT_REFRESH; return 1; }
    if (!strncmp(b, "open ", 5)) { if (!valid_name(b + 5)) return 0; out->kind = CM_ACT_OPEN; snprintf(out->name, sizeof out->name, "%.*s", CM_NAME_MAX - 1, b + 5); return 1; }
    if (!strncmp(b, "activate ", 9)) { if (!parse_id(b + 9, &out->id)) return 0; out->kind = CM_ACT_ACTIVATE; return 1; }
    if (!strncmp(b, "close ", 6)) { if (!parse_id(b + 6, &out->id)) return 0; out->kind = CM_ACT_CLOSE; return 1; }
    return 0;
}

int cm_item_h(const CmItem *it) { return it->separator ? TH_MENU_SEP_H : TH_MENU_ITEM_H; }
int cm_menu_h(const CmItem *items, size_t n) { int h = 2 * TH_MENU_PAD; for (size_t i = 0; i < n; i++) h += cm_item_h(&items[i]); return h; }
int cm_item_y(const CmItem *items, size_t idx) { int y = TH_MENU_PAD; for (size_t i = 0; i < idx; i++) y += cm_item_h(&items[i]); return y; }

void cm_place(int px, int py, int mw, int mh, int sw, int sh, int top, int *x, int *y)
{
    int ax = px, ay = py;
    if (ax + mw > sw) ax = sw - mw;
    if (ax < 0) ax = 0;
    if (ay + mh > sh) ay = py - mh;                      /* no room below: open upwards from the pointer */
    if (ay + mh > sh) ay = sh - mh;
    if (ay < top) ay = top;
    *x = ax; *y = ay;
}

int cm_step(const CmItem *items, size_t n, int cur, int dir)
{
    if (n == 0 || (dir != 1 && dir != -1)) return -1;
    int count = (int)n, i = cur;
    if (i < 0 || i >= count) i = dir > 0 ? -1 : count;
    for (int tries = 0; tries < count; tries++) {
        i += dir; if (i >= count) i = 0; if (i < 0) i = count - 1;
        if (!items[i].separator) return i;
    }
    return -1;
}
