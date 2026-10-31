#define _GNU_SOURCE
#include "fm_core.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int cmp(const void *a, const void *b)
{
    const FmEntry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;                    /* folders first */
    int c = strcasecmp(x->name, y->name); return c ? c : strcmp(x->name, y->name);
}
void fm_free(FmDir *d) { free(d->e); d->e = NULL; d->n = 0; }
int fm_load(FmDir *d, const char *path, int show_hidden, char *err, size_t errlen)
{
    char norm[4096]; fm_normalize(norm, sizeof norm, path);
    DIR *dir = opendir(norm);
    if (!dir) { snprintf(err, errlen, "%s", strerror(errno)); return -1; }
    FmEntry *list = NULL; int n = 0, cap = 0, skipped = 0; struct dirent *de;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!show_hidden && de->d_name[0] == '.') { skipped++; continue; }
        if (n == cap) { cap = cap ? cap * 2 : 64; FmEntry *nl = realloc(list, (size_t)cap * sizeof *nl); if (!nl) { free(list); closedir(dir); snprintf(err, errlen, "out of memory"); return -1; } list = nl; }
        FmEntry *e = &list[n]; memset(e, 0, sizeof *e); snprintf(e->name, sizeof e->name, "%s", de->d_name);
        char full[5000]; fm_join(full, sizeof full, norm, de->d_name); struct stat st;
        if (lstat(full, &st) != 0) { e->broken = 1; n++; continue; }              /* vanished or unreadable: still listed, honestly marked */
        e->mode = st.st_mode; e->size = st.st_size; e->mtime = st.st_mtime; e->uid = st.st_uid; e->gid = st.st_gid; e->is_dir = S_ISDIR(st.st_mode);
        if (S_ISLNK(st.st_mode)) {
            e->is_link = 1; ssize_t r = readlink(full, e->link, sizeof e->link - 1); if (r > 0) e->link[r] = 0;
            struct stat ts; if (stat(full, &ts) == 0) e->is_dir = S_ISDIR(ts.st_mode); else e->broken = 1;
        }
        n++;
    }
    closedir(dir);
    if (n > 1) qsort(list, (size_t)n, sizeof *list, cmp);
    fm_free(d); d->e = list; d->n = n; d->hidden_skipped = skipped; snprintf(d->path, sizeof d->path, "%s", norm);
    return 0;
}
void fm_normalize(char *out, size_t n, const char *path)
{
    char buf[4096]; char *parts[512]; int np = 0; snprintf(buf, sizeof buf, "%s", path && *path ? path : "/");
    if (buf[0] != '/') { char cwd[2048]; if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/"); char t[8192]; snprintf(t, sizeof t, "%s/%s", cwd, buf); snprintf(buf, sizeof buf, "%.4095s", t); }
    char *save = NULL;
    for (char *t = strtok_r(buf, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (np) np--; continue; }
        if (np < 512) parts[np++] = t;
    }
    size_t o = 0; if (n == 0) return;
    if (np == 0) { snprintf(out, n, "/"); return; }
    for (int i = 0; i < np && o + 1 < n; i++) o += (size_t)snprintf(out + o, n - o, "/%s", parts[i]);
}
void fm_join(char *out, size_t n, const char *dir, const char *name) { snprintf(out, n, "%s%s%s", dir, (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", name); }
void fm_parent(char *out, size_t n, const char *path) { char t[4200]; snprintf(t, sizeof t, "%s/..", path); fm_normalize(out, n, t); }
void fm_format_size(char *out, size_t n, long long b)
{
    static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" }; double v = (double)b; int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, n, "%lld B", b); else snprintf(out, n, "%.1f %s", v, u[i]);
}
void fm_format_mode(char out[11], mode_t m)
{
    out[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' : S_ISFIFO(m) ? 'p' : S_ISSOCK(m) ? 's' : '-';
    const char *r = "rwxrwxrwx"; for (int i = 0; i < 9; i++) out[1 + i] = (m & (mode_t)(0400 >> i)) ? r[i] : '-';
    if (m & S_ISUID) out[3] = (m & 0100) ? 's' : 'S';
    if (m & S_ISGID) out[6] = (m & 0010) ? 's' : 'S';
    if (m & S_ISVTX) out[9] = (m & 0001) ? 't' : 'T';
    out[10] = 0;
}
void fm_format_time(char *out, size_t n, time_t t) { struct tm tm; if (!gmtime_r(&t, &tm)) { snprintf(out, n, "?"); return; } strftime(out, n, "%Y-%m-%d %H:%M", &tm); }
const char *fm_kind(const FmEntry *e)
{
    if (e->broken && e->is_link) return "Broken symbolic link";
    if (e->is_link) return e->is_dir ? "Symbolic link to folder" : "Symbolic link";
    if (S_ISDIR(e->mode)) return "Folder";
    if (S_ISREG(e->mode)) return "File";
    if (S_ISCHR(e->mode)) return "Character device";
    if (S_ISBLK(e->mode)) return "Block device";
    if (S_ISFIFO(e->mode)) return "Named pipe";
    if (S_ISSOCK(e->mode)) return "Socket";
    return "Unknown";
}
long fm_preview(const char *path, char *out, size_t cap, int *is_text, char *err, size_t errlen)
{
    *is_text = 0; if (cap < 2) return -1;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY); if (fd < 0) { snprintf(err, errlen, "%s", strerror(errno)); return -1; }
    struct stat st; if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); snprintf(err, errlen, "not a regular file"); return -1; }
    ssize_t r = read(fd, out, cap - 1); int e = errno; close(fd);
    if (r < 0) { snprintf(err, errlen, "%s", strerror(e)); return -1; }
    out[r] = 0; int text = 1; for (ssize_t i = 0; i < r; i++) { unsigned char c = (unsigned char)out[i]; if (c == 0 || (c < 32 && c != '\n' && c != '\t' && c != '\r')) { text = 0; break; } }
    *is_text = text; return (long)r;
}
