/* aether-pm: small shared helpers (allocation, hex, strict name/path validation). */
#ifndef AETHER_PM_UTIL_H
#define AETHER_PM_UTIL_H
#include <stddef.h>
#include <stdint.h>

#define PM_ERRLEN 256
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xsprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void set_err(char *err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Read a whole regular file (NUL-terminated, <= max bytes). NULL + errno-style message on failure. */
char *read_file(const char *path, size_t *len, size_t max, char *err);
int write_file_atomic(const char *path, const void *buf, size_t len, unsigned mode, char *err);

void hex_encode(const uint8_t *in, size_t n, char *out);       /* out needs 2n+1 */
int hex_decode(const char *in, uint8_t *out, size_t n);        /* 0 ok; exactly 2n hex chars */

/* Strict syntax rules used by package metadata. All return 1 if valid. */
int valid_pkg_name(const char *s);        /* [a-z0-9][a-z0-9+._-]{0,63} */
int valid_version(const char *s);         /* [0-9A-Za-z.+~:_-]+, starts with digit */
int valid_token(const char *s);           /* [A-Za-z0-9._+@,:=~/-]+ (no spaces/control) */
int valid_relpath(const char *s);         /* relative, no "", ".", ".." components, valid_token, <4096 */
#endif
