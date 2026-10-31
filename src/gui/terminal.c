/* aether-terminal: a terminal emulator window. It runs the system shell (/bin/sh, BusyBox ash) on a real pty; everything
 * you see is that shell's actual output interpreted by term_vt.c. Nothing is scripted or simulated.
 *
 *   aether-terminal [--cwd DIR] [--exec PROGRAM]     (default: /bin/sh -i in $HOME or /)
 *
 * Keyboard: typing goes to the shell; Ctrl+C/Ctrl+D/Ctrl+Z are real terminal signals/EOF; arrows/Home/End/Del/PgUp/PgDn/F1-F12 are sent
 * as xterm sequences. Shift+PageUp/PageDown scroll the history, Ctrl+Shift+C copies the selection, Ctrl+Shift+V or Shift+Insert pastes.
 * Mouse: drag selects (double-click a word, triple-click a line; the selection is copied automatically), middle-click pastes, wheel scrolls.
 * Test hook: SIGUSR1 writes the visible text to $AETHER_TERMINAL_DUMP (default /run/aether/terminal.dump). */
#define _GNU_SOURCE
#include "input.h"
#include "term_vt.h"
#include "ui.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#define PAD 8
#define SCROLLBACK 2000
#define CLIP_MAX 65536
static Vt *vt; static int master = -1; static pid_t shell_pid;
static int voff;                                                   /* lines scrolled back from the live screen */
static int sel_on, sel_drag, sel_al, sel_ac, sel_bl, sel_bc;       /* selection in absolute (line, col), anchor a .. cursor b */
static char clip[CLIP_MAX]; static size_t clip_n;
static int term_widget = -1;
static volatile sig_atomic_t want_dump;

static void on_usr1(int s) { (void)s; want_dump = 1; }
static int cols_for(int w) { int c = (w - 2 * PAD) / FONT_W; return c < 2 ? 2 : c; }
static int rows_for(int h) { int r = (h - 2 * PAD) / FONT_H; return r < 1 ? 1 : r; }

static void set_winsize(void)
{
    struct winsize ws; memset(&ws, 0, sizeof ws); ws.ws_col = (unsigned short)vt->cols; ws.ws_row = (unsigned short)vt->rows;
    if (master >= 0) ioctl(master, TIOCSWINSZ, &ws);
}
static void write_all(const char *p, size_t n)
{
    while (n > 0 && master >= 0) {
        ssize_t r = write(master, p, n);
        if (r > 0) { p += r; n -= (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == EAGAIN) { struct pollfd pf = { master, POLLOUT, 0 }; poll(&pf, 1, 200); continue; }
        break;
    }
}
static int spawn_shell(const char *cwd, const char *prog, char *err, size_t errlen)
{
    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) { snprintf(err, errlen, "cannot open a pty (is devpts mounted at /dev/pts?): %s", strerror(errno)); return -1; }
    char *sn = ptsname(master); if (!sn) { snprintf(err, errlen, "ptsname: %s", strerror(errno)); return -1; }
    char slave[64]; snprintf(slave, sizeof slave, "%s", sn);
    set_winsize();
    pid_t p = fork(); if (p < 0) { snprintf(err, errlen, "fork: %s", strerror(errno)); return -1; }
    if (p == 0) {
        setsid(); int s = open(slave, O_RDWR); if (s < 0) _exit(126);
        ioctl(s, TIOCSCTTY, 0); dup2(s, 0); dup2(s, 1); dup2(s, 2); if (s > 2) close(s);
        signal(SIGUSR1, SIG_DFL);
        char c[16], l[16]; snprintf(c, sizeof c, "%d", vt->cols); snprintf(l, sizeof l, "%d", vt->rows);
        setenv("TERM", "xterm", 1); setenv("COLUMNS", c, 1); setenv("LINES", l, 1);
        if (!getenv("HOME")) setenv("HOME", "/root", 1);
        setenv("PS1", "\\w \\$ ", 1);
        { const char *home = getenv("HOME"); int ok = (cwd && chdir(cwd) == 0) || (home && chdir(home) == 0) || chdir("/") == 0; (void)ok; }
        if (prog) execl(prog, prog, (char *)NULL); else execl("/bin/sh", "sh", "-i", (char *)NULL);
        dprintf(2, "aether-terminal: cannot execute %s: %s\n", prog ? prog : "/bin/sh", strerror(errno)); _exit(127);
    }
    shell_pid = p; fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    return 0;
}

/* ---- selection / clipboard ---------------------------------------------------------------------------------------- */
static void sel_order(int *l0, int *c0, int *l1, int *c1)
{
    if (sel_al < sel_bl || (sel_al == sel_bl && sel_ac <= sel_bc)) { *l0 = sel_al; *c0 = sel_ac; *l1 = sel_bl; *c1 = sel_bc; }
    else { *l0 = sel_bl; *c0 = sel_bc; *l1 = sel_al; *c1 = sel_ac; }
}
static int in_sel(int line, int col)
{
    if (!sel_on) return 0;
    int l0, c0, l1, c1; sel_order(&l0, &c0, &l1, &c1);
    if (line < l0 || line > l1) return 0;
    if (line == l0 && col < c0) return 0;
    if (line == l1 && col > c1) return 0;
    return 1;
}
static void copy_selection(void)
{
    if (!sel_on) return;
    int l0, c0, l1, c1; sel_order(&l0, &c0, &l1, &c1);
    clip_n = vt_text(vt, l0, c0, l1, c1, clip, sizeof clip);
}
static void paste(void) { if (clip_n) { voff = 0; write_all(clip, clip_n); } }
static int base_line(void) { return vt->alt ? 0 : vt_abs_lines(vt) - vt->rows - voff; }

/* ---- drawing ------------------------------------------------------------------------------------------------------ */
static void draw_glyph(Surface *s, int x, int y, unsigned ch, uint32_t fg, int bold)
{
    if (ch < 32 || ch > 126) ch = '?';
    const unsigned char *g = aether_font8x16[ch - 32];
    for (int r = 0; r < FONT_H; r++) for (int c = 0; c < FONT_W; c++) {
        if (g[r] & (0x80u >> c)) { int px = x + c, py = y + r; if (px >= 0 && py >= 0 && px < s->w && py < s->h) s->px[(size_t)py * (size_t)s->stride + (size_t)px] = fg; if (bold && px + 1 < s->w) s->px[(size_t)py * (size_t)s->stride + (size_t)px + 1] = fg; }
    }
}
static uint32_t color(int idx, int is_fg, int bold)
{
    if (idx == VT_DEFAULT) return is_fg ? TH_TEXT : TH_DESKTOP;
    if (is_fg && bold && idx < 8) idx += 8;
    return vt_palette(idx);
}
static void draw_term(UiWindow *w, int i, Surface *s)
{
    (void)i; (void)w; gfx_fill(s, rect_make(0, 0, s->w, s->h), TH_DESKTOP);
    int base = base_line(), live = voff == 0;
    for (int r = 0; r < vt->rows; r++) {
        int line = base + r, y = PAD + r * FONT_H;
        for (int c = 0; c < vt->cols; c++) {
            VtCell cell = vt_abs_cell(vt, line, c); int x = PAD + c * FONT_W;
            uint32_t fg = color(cell.fg, 1, cell.attr & VA_BOLD), bg = color(cell.bg, 0, 0);
            if (cell.attr & VA_REVERSE) { uint32_t t = fg; fg = (cell.bg == VT_DEFAULT) ? TH_DESKTOP : bg; bg = t; }
            if (in_sel(line, c)) { bg = gfx_mix(TH_DESKTOP, TH_BLUE, 150); fg = TH_TEXT; }
            if (live && vt->cursor_visible && line == base + vt->cy && c == vt->cx) { bg = TH_CYAN; fg = TH_VOID; }
            if (bg != TH_DESKTOP) gfx_fill(s, rect_make(x, y, FONT_W, FONT_H), bg);
            if (cell.ch != ' ') draw_glyph(s, x, y, cell.ch, fg, cell.attr & VA_BOLD);
            if (cell.attr & VA_UNDERLINE) gfx_fill(s, rect_make(x, y + FONT_H - 2, FONT_W, 1), fg);
        }
    }
    if (voff > 0) {                                                           /* scrollback indicator */
        char t[32]; snprintf(t, sizeof t, " history -%d ", voff); int tw = gfx_text_width(t);
        gfx_fill(s, rect_make(s->w - tw - 12, 4, tw, FONT_H), TH_SURFACE2); gfx_text(s, s->w - tw - 12, 4, t, TH_TEXT2);
    }
}
static void layout(UiWindow *w)
{
    ui_clear(w); term_widget = ui_custom(w, rect_make(0, 0, w->w, w->h), draw_term, NULL, NULL, 0, UI_ALL_PAGES);
}

/* ---- input -------------------------------------------------------------------------------------------------------- */
static void send_str(const char *s) { voff = 0; write_all(s, strlen(s)); }
static void on_key(UiWindow *w, int key, int ascii, int pressed)
{
    if (!pressed) return;
    int shift = (w->mods & KMOD_SHIFT) != 0, ctrl = (w->mods & KMOD_CTRL) != 0, alt = (w->mods & KMOD_ALT) != 0;
    int page = vt->rows > 2 ? vt->rows - 1 : 1;
    if (ctrl && shift && key == KEY_C) { copy_selection(); return; }
    if ((ctrl && shift && key == KEY_V) || (shift && key == KEY_INSERT)) { paste(); ui_redraw(w); return; }
    if (shift && key == KEY_PAGEUP) { voff = th_clamp(voff + page, 0, vt->alt ? 0 : vt->sb_n); ui_redraw(w); return; }
    if (shift && key == KEY_PAGEDOWN) { voff = th_clamp(voff - page, 0, vt->alt ? 0 : vt->sb_n); ui_redraw(w); return; }
    static const struct { int key; const char *norm, *app; } nav[] = {
        { KEY_UP, "\x1b[A", "\x1bOA" }, { KEY_DOWN, "\x1b[B", "\x1bOB" }, { KEY_RIGHT, "\x1b[C", "\x1bOC" }, { KEY_LEFT, "\x1b[D", "\x1bOD" },
        { KEY_HOME, "\x1b[H", "\x1bOH" }, { KEY_END, "\x1b[F", "\x1bOF" }, { KEY_DELETE, "\x1b[3~", "\x1b[3~" }, { KEY_INSERT, "\x1b[2~", "\x1b[2~" },
        { KEY_PAGEUP, "\x1b[5~", "\x1b[5~" }, { KEY_PAGEDOWN, "\x1b[6~", "\x1b[6~" },
        { KEY_F1, "\x1bOP", "\x1bOP" }, { KEY_F2, "\x1bOQ", "\x1bOQ" }, { KEY_F3, "\x1bOR", "\x1bOR" }, { KEY_F4, "\x1bOS", "\x1bOS" },
        { KEY_F5, "\x1b[15~", "\x1b[15~" }, { KEY_F6, "\x1b[17~", "\x1b[17~" }, { KEY_F7, "\x1b[18~", "\x1b[18~" }, { KEY_F8, "\x1b[19~", "\x1b[19~" },
        { KEY_F9, "\x1b[20~", "\x1b[20~" }, { KEY_F10, "\x1b[21~", "\x1b[21~" }, { KEY_F11, "\x1b[23~", "\x1b[23~" }, { KEY_F12, "\x1b[24~", "\x1b[24~" } };
    for (size_t i = 0; i < sizeof nav / sizeof nav[0]; i++) if (nav[i].key == key) { sel_on = 0; send_str(vt->app_cursor ? nav[i].app : nav[i].norm); ui_redraw(w); return; }
    char b[4]; size_t n = 0;
    if (key == KEY_ENTER || key == KEY_KPENTER) b[n++] = '\r';
    else if (key == KEY_BACKSPACE) b[n++] = 0x7f;
    else if (key == KEY_TAB) b[n++] = '\t';
    else if (key == KEY_ESC) b[n++] = 0x1b;
    else if (ctrl) { int a = keycode_to_ascii(key, 0); if (a >= 'a' && a <= 'z') b[n++] = (char)(a & 0x1f); else if (a == '[') b[n++] = 0x1b; else if (a == '\\') b[n++] = 0x1c; else if (a == ']') b[n++] = 0x1d; else if (a == ' ') b[n++] = 0; }
    else if (ascii >= 32 && ascii < 127) { if (alt) b[n++] = 0x1b; b[n++] = (char)ascii; }
    if (n) { sel_on = 0; voff = 0; write_all(b, n); ui_redraw(w); }
}
static void cell_at(int px, int py, int *line, int *col)
{
    int c = (px - PAD) / FONT_W, r = (py - PAD) / FONT_H; if (px < PAD) c = 0; if (py < PAD) r = 0;
    *col = th_clamp(c, 0, vt->cols - 1); *line = base_line() + th_clamp(r, 0, vt->rows - 1);
}
static void on_pointer(UiWindow *w, int kind, int x, int y, int button, int clicks)
{
    int line, col; cell_at(x, y, &line, &col);
    if (kind == 5) { voff = th_clamp(voff + button * 3, 0, vt->alt ? 0 : vt->sb_n); ui_redraw(w); return; }
    if (kind == 1 && button == 2) { paste(); ui_redraw(w); return; }
    if (kind == 1 && button == 1) {
        sel_drag = 1; sel_al = sel_bl = line; sel_ac = sel_bc = col; sel_on = 0;
        if (clicks == 2) { int a, z; if (vt_word_bounds(vt, line, col, &a, &z)) { sel_ac = a; sel_bc = z; sel_on = 1; } }
        else if (clicks >= 3) { sel_ac = 0; sel_bc = vt->cols - 1; sel_on = 1; }
        ui_redraw(w);
    } else if (kind == 0 && sel_drag) {
        if (line != sel_bl || col != sel_bc || !sel_on) { sel_bl = line; sel_bc = col; sel_on = !(sel_al == sel_bl && sel_ac == sel_bc) || sel_on; ui_redraw(w); }
    } else if (kind == 2 && button == 1 && sel_drag) { sel_drag = 0; if (sel_on) copy_selection(); }
}

/* ---- pty output / lifecycle --------------------------------------------------------------------------------------- */
static void dump_screen(void)
{
    const char *path = getenv("AETHER_TERMINAL_DUMP"); if (!path) path = "/run/aether/terminal.dump";
    FILE *f = fopen(path, "w"); if (!f) return;
    static char buf[1 << 16]; size_t n = vt_text(vt, vt_abs_lines(vt) - vt->rows, 0, vt_abs_lines(vt) - 1, vt->cols - 1, buf, sizeof buf);
    fwrite(buf, 1, n, f); fputc('\n', f); fclose(f);
}
static void on_pty(UiWindow *w)
{
    char buf[16384]; int got = 0, eof = 0;
    for (int rounds = 0; rounds < 8; rounds++) {
        ssize_t r = read(master, buf, sizeof buf);
        if (r > 0) { vt_feed(vt, buf, (size_t)r); got = 1; if (voff > 0 && !vt->alt) { /* keep the history view anchored while output arrives */ } continue; }
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) eof = 1;
        break;
    }
    char rep[256]; size_t rn = vt_take_reply(vt, rep, sizeof rep); if (rn) write_all(rep, rn);
    if (eof) { w->closed = 1; return; }                                       /* the shell exited: close the window */
    if (got) ui_redraw(w);
}
static void on_tick(UiWindow *w) { if (want_dump) { want_dump = 0; dump_screen(); } int st; if (shell_pid > 0 && waitpid(shell_pid, &st, WNOHANG) == shell_pid) { shell_pid = 0; w->closed = 1; } }
static void on_resize(UiWindow *w, int nw, int nh)
{
    char err[96]; if (ui_resize(w, nw, nh, err, sizeof err) != 0) return;
    vt_resize(vt, cols_for(nw), rows_for(nh)); set_winsize(); voff = 0; sel_on = 0; layout(w); ui_redraw(w);
}

int main(int argc, char **argv)
{
    const char *cwd = NULL, *prog = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cwd") && i + 1 < argc) cwd = argv[++i];
        else if (!strcmp(argv[i], "--exec") && i + 1 < argc) prog = argv[++i];
        else { fprintf(stderr, "usage: aether-terminal [--cwd DIR] [--exec PROGRAM]\n"); return 2; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0); signal(SIGPIPE, SIG_IGN);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_usr1; sigaction(SIGUSR1, &sa, NULL);
    UiWindow w; char err[160];
    if (ui_connect(&w, err, sizeof err) != 0) { fprintf(stderr, "aether-terminal: %s\n", err); return 1; }
    int cw = th_min(PAD * 2 + 96 * FONT_W, w.screen_w - 2 * TH_S6), ch = th_min(PAD * 2 + 28 * FONT_H, w.screen_h - PANEL_HEIGHT - TH_TITLE_H - TH_S6);
    vt = vt_new(cols_for(cw), rows_for(ch), SCROLLBACK); if (!vt) { fprintf(stderr, "aether-terminal: out of memory\n"); return 1; }
    cw = PAD * 2 + vt->cols * FONT_W; ch = PAD * 2 + vt->rows * FONT_H;
    if (spawn_shell(cwd, prog, err, sizeof err) != 0) { fprintf(stderr, "aether-terminal: %s\n", err); return 1; }
    w.bg = TH_DESKTOP; w.flags = WF_RESIZABLE | WF_CENTER;
    if (ui_create(&w, "Terminal", WK_NORMAL, cw, ch, 0, 0, err, sizeof err) != 0) { fprintf(stderr, "aether-terminal: %s\n", err); return 1; }
    layout(&w); w.on_key = on_key; w.on_pointer = on_pointer; w.on_watch = on_pty; w.watch_fd = master; w.on_resize = on_resize; w.on_tick = on_tick; w.tick_ms = 250;
    printf("aether-terminal: window ready (%dx%d cells), shell pid %d on a pty\n", vt->cols, vt->rows, (int)shell_pid);
    ui_run(&w);
    if (shell_pid > 0) { kill(shell_pid, SIGHUP); waitpid(shell_pid, NULL, 0); }
    ui_destroy(&w); vt_free(vt); return 0;
}
