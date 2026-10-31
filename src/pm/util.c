#define _GNU_SOURCE
#include "util.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { fputs("aether-pm: out of memory\n", stderr); exit(70); } return p; }
void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) { fputs("aether-pm: out of memory\n", stderr); exit(70); } return p; }
char *xstrdup(const char *s) { size_t n = strlen(s) + 1; char *d = xmalloc(n); memcpy(d, s, n); return d; }
char *xsprintf(const char *fmt, ...)
{
    va_list ap, ap2; va_start(ap, fmt); va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    if (n < 0) { va_end(ap2); fputs("aether-pm: format error\n", stderr); exit(70); }
    char *s = xmalloc((size_t)n + 1); vsnprintf(s, (size_t)n + 1, fmt, ap2); va_end(ap2); return s;
}
void set_err(char *err, const char *fmt, ...)
{
    if (!err) return;
    va_list ap; va_start(ap, fmt); vsnprintf(err, PM_ERRLEN, fmt, ap); va_end(ap);
}

char *read_file(const char *path, size_t *len, size_t max, char *err)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { set_err(err, "%s: %s", path, strerror(errno)); return NULL; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { set_err(err, "%s: not a regular file", path); close(fd); return NULL; }
    if ((uint64_t)st.st_size > max) { set_err(err, "%s: too large", path); close(fd); return NULL; }
    size_t n = (size_t)st.st_size, got = 0; char *b = xmalloc(n + 1);
    while (got < n) { ssize_t r = read(fd, b + got, n - got); if (r < 0 && errno == EINTR) continue; if (r <= 0) break; got += (size_t)r; }
    close(fd);
    if (got != n) { set_err(err, "%s: short read", path); free(b); return NULL; }
    b[n] = 0; if (len) *len = n; return b;
}

int write_file_atomic(const char *path, const void *buf, size_t len, unsigned mode, char *err)
{
    char *tmp = xsprintf("%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd < 0) { set_err(err, "%s: %s", tmp, strerror(errno)); free(tmp); return -1; }
    const char *p = buf; size_t off = 0;
    while (off < len) { ssize_t w = write(fd, p + off, len - off); if (w < 0 && errno == EINTR) continue; if (w <= 0) goto fail; off += (size_t)w; }
    if (fchmod(fd, mode) != 0 || fsync(fd) != 0) goto fail;
    if (close(fd) != 0) { fd = -1; goto fail; }
    if (rename(tmp, path) != 0) { fd = -1; goto fail; }
    free(tmp); return 0;
fail:
    set_err(err, "%s: %s", path, strerror(errno)); if (fd >= 0) close(fd); unlink(tmp); free(tmp); return -1;
}

void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[in[i] >> 4]; out[2 * i + 1] = d[in[i] & 15]; }
    out[2 * n] = 0;
}
static int hexval(char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; return -1; }
int hex_decode(const char *in, uint8_t *out, size_t n)
{
    if (strlen(in) != 2 * n) return -1;
    for (size_t i = 0; i < n; i++) { int a = hexval(in[2 * i]), b = hexval(in[2 * i + 1]); if (a < 0 || b < 0) return -1; out[i] = (uint8_t)(a * 16 + b); }
    return 0;
}

int valid_pkg_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 64) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (i > 0 && (c == '+' || c == '.' || c == '_' || c == '-'));
        if (!ok) return 0;
    }
    return 1;
}
int valid_version(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 64 || s[0] < '0' || s[0] > '9') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '+' || c == '~' || c == ':' || c == '_' || c == '-')) return 0;
    }
    return 1;
}
int valid_token(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 4096) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '+' || c == '@' ||
              c == ',' || c == ':' || c == '=' || c == '~' || c == '/' || c == '-')) return 0;
    }
    return 1;
}
int valid_relpath(const char *s)
{
    if (!valid_token(s) || s[0] == '/') return 0;
    const char *p = s;
    while (*p) {
        const char *e = strchr(p, '/'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == 0 || (l == 1 && p[0] == '.') || (l == 2 && p[0] == '.' && p[1] == '.')) return 0;
        if (!e) break;
        p = e + 1;
    }
    return 1;
}
