#define _GNU_SOURCE
#include "pkgfile.h"
#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---------- Meta lifecycle ---------- */
void meta_free(Meta *m)
{
    free(m->name); free(m->version); free(m->arch); free(m->libc); free(m->summary);
    for (size_t i = 0; i < m->ndepends; i++) atom_free(&m->depends[i]);
    for (size_t i = 0; i < m->nconflicts; i++) atom_free(&m->conflicts[i]);
    for (size_t i = 0; i < m->nprovides; i++) { free(m->provides[i].name); free(m->provides[i].ver); }
    for (size_t i = 0; i < m->nneeds_so; i++) free(m->needs_so[i]);
    for (size_t i = 0; i < m->nprovides_so; i++) free(m->provides_so[i]);
    for (size_t i = 0; i < m->nentries; i++) { free(m->entries[i].path); free(m->entries[i].target); }
    free(m->depends); free(m->conflicts); free(m->provides); free(m->needs_so); free(m->provides_so); free(m->entries);
    memset(m, 0, sizeof *m);
}

static int valid_soname(const char *s) { return valid_token(s) && !strchr(s, '/'); }
static int set_once(char **dst, const char *key, const char *val, char *err)
{
    if (*dst) { set_err(err, "duplicate '%s'", key); return -1; }
    *dst = xstrdup(val); return 0;
}

int meta_apply_line(Meta *m, char **tok, int n, char *err)
{
    const char *k = tok[0];
    if (!strcmp(k, "name") || !strcmp(k, "version") || !strcmp(k, "arch") || !strcmp(k, "libc")) {
        if (n != 2) { set_err(err, "'%s' needs exactly one value", k); return -1; }
        if (!strcmp(k, "name")) { if (!valid_pkg_name(tok[1])) { set_err(err, "invalid package name '%s'", tok[1]); return -1; } return set_once(&m->name, k, tok[1], err); }
        if (!strcmp(k, "version")) { if (!valid_version(tok[1])) { set_err(err, "invalid version '%s'", tok[1]); return -1; } return set_once(&m->version, k, tok[1], err); }
        if (!strcmp(k, "arch")) { if (strcmp(tok[1], "x86_64") && strcmp(tok[1], "any")) { set_err(err, "unsupported arch '%s'", tok[1]); return -1; } return set_once(&m->arch, k, tok[1], err); }
        int ok = !strcmp(tok[1], "none");
        if (!ok && !strncmp(tok[1], "glibc-", 6) && tok[1][6]) { ok = 1; for (const char *c = tok[1] + 6; *c; c++) if (!((*c >= '0' && *c <= '9') || *c == '.')) ok = 0; }
        if (!ok) { set_err(err, "invalid libc '%s' (none or glibc-X.Y)", tok[1]); return -1; }
        return set_once(&m->libc, k, tok[1], err);
    }
    if (!strcmp(k, "essential")) {
        if (n != 2 || (strcmp(tok[1], "yes") && strcmp(tok[1], "no"))) { set_err(err, "'essential' must be yes or no"); return -1; }
        m->essential = !strcmp(tok[1], "yes"); return 0;
    }
    if (!strcmp(k, "depends") || !strcmp(k, "conflicts")) {
        Atom a; if (atom_parse(tok + 1, n - 1, &a, err) != 0) return -1;
        if (!strcmp(k, "depends")) { m->depends = xrealloc(m->depends, (m->ndepends + 1) * sizeof *m->depends); m->depends[m->ndepends++] = a; }
        else { m->conflicts = xrealloc(m->conflicts, (m->nconflicts + 1) * sizeof *m->conflicts); m->conflicts[m->nconflicts++] = a; }
        return 0;
    }
    if (!strcmp(k, "provides")) {
        Atom a; if (atom_parse(tok + 1, n - 1, &a, err) != 0) return -1;
        if (a.op != OP_ANY && a.op != OP_EQ) { set_err(err, "'provides' only allows '='"); atom_free(&a); return -1; }
        m->provides = xrealloc(m->provides, (m->nprovides + 1) * sizeof *m->provides);
        m->provides[m->nprovides].name = a.name; m->provides[m->nprovides].ver = a.ver; m->nprovides++; return 0;
    }
    if (!strcmp(k, "needs-soname") || !strcmp(k, "provides-soname")) {
        if (n != 2 || !valid_soname(tok[1])) { set_err(err, "'%s' needs one soname (no '/')", k); return -1; }
        if (k[0] == 'n') { m->needs_so = xrealloc(m->needs_so, (m->nneeds_so + 1) * sizeof(char *)); m->needs_so[m->nneeds_so++] = xstrdup(tok[1]); }
        else { m->provides_so = xrealloc(m->provides_so, (m->nprovides_so + 1) * sizeof(char *)); m->provides_so[m->nprovides_so++] = xstrdup(tok[1]); }
        return 0;
    }
    set_err(err, "unknown key '%s'", k); return -1;
}

static int path_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int path_allowed_for_package(const char *path)
{
    static const char *const alias[] = { "bin", "sbin", "lib", "lib64" };      /* usr-merge symlinks are owned by the base layout */
    for (size_t i = 0; i < sizeof alias / sizeof *alias; i++) {
        size_t l = strlen(alias[i]);
        if (!strncmp(path, alias[i], l) && (path[l] == '/' || path[l] == 0)) return 0;
    }
    if (!strncmp(path, "var/lib/aether/pm", 17) && (path[17] == '/' || path[17] == 0)) return 0;   /* package database */
    if (!strncmp(path, "etc/aether/keys", 15) && (path[15] == '/' || path[15] == 0)) return 0;     /* trust store */
    static const char *const virt[] = { "proc", "sys", "dev", "run", "tmp" };
    for (size_t i = 0; i < sizeof virt / sizeof *virt; i++) {
        size_t l = strlen(virt[i]);
        if (!strncmp(path, virt[i], l) && (path[l] == '/' || path[l] == 0)) return 0;
    }
    return 1;
}

int meta_validate(const Meta *m, int need_manifest, char *err)
{
    if (!m->name || !m->version || !m->arch || !m->libc) { set_err(err, "missing required key (name, version, arch, libc)"); return -1; }
    if (!need_manifest) return 0;
    if (m->nentries == 0) { set_err(err, "empty manifest"); return -1; }
    char **ps = xmalloc(m->nentries * sizeof *ps);
    for (size_t i = 0; i < m->nentries; i++) ps[i] = m->entries[i].path;
    qsort(ps, m->nentries, sizeof *ps, path_cmp);
    for (size_t i = 1; i < m->nentries; i++) if (!strcmp(ps[i - 1], ps[i])) { set_err(err, "duplicate manifest path '%s'", ps[i]); free(ps); return -1; }
    free(ps);
    return 0;
}

static unsigned long parse_mode(const char *s, int *ok)
{
    char *e; *ok = 0; if (!*s || strlen(s) > 4) return 0;
    for (const char *c = s; *c; c++) if (*c < '0' || *c > '7') return 0;
    unsigned long v = strtoul(s, &e, 8); if (*e || v > 0777) return 0;
    *ok = 1; return v;
}
static int add_entry(Meta *m, char **tok, int n, char *err)
{
    Entry e; memset(&e, 0, sizeof e); int ok;
    if (!strcmp(tok[0], "dir")) {
        if (n != 3) { set_err(err, "dir: expected 'dir MODE PATH'"); return -1; }
        e.type = 'd'; e.mode = (unsigned)parse_mode(tok[1], &ok); if (!ok) { set_err(err, "dir: bad mode '%s'", tok[1]); return -1; }
        if (!valid_relpath(tok[2]) || !path_allowed_for_package(tok[2])) { set_err(err, "dir: path not allowed '%s'", tok[2]); return -1; }
        e.path = xstrdup(tok[2]);
    } else if (!strcmp(tok[0], "file")) {
        if (n != 5) { set_err(err, "file: expected 'file MODE SIZE SHA256 PATH'"); return -1; }
        e.type = 'f'; e.mode = (unsigned)parse_mode(tok[1], &ok); if (!ok) { set_err(err, "file: bad mode '%s'", tok[1]); return -1; }
        char *end; errno = 0; unsigned long long sz = strtoull(tok[2], &end, 10);
        if (!*tok[2] || *end || errno || strlen(tok[2]) > 15) { set_err(err, "file: bad size '%s'", tok[2]); return -1; }
        e.size = sz;
        if (hex_decode(tok[3], e.sha, 32) != 0) { set_err(err, "file: bad sha256"); return -1; }
        if (!valid_relpath(tok[4]) || !path_allowed_for_package(tok[4])) { set_err(err, "file: path not allowed '%s'", tok[4]); return -1; }
        e.path = xstrdup(tok[4]);
        if (m->payload_len + e.size < m->payload_len) { set_err(err, "payload size overflow"); free(e.path); return -1; }
        e.off = m->payload_len; m->payload_len += e.size;
    } else {
        if (n != 3) { set_err(err, "link: expected 'link TARGET PATH'"); return -1; }
        e.type = 'l'; e.mode = 0777;
        if (!valid_token(tok[1]) || strlen(tok[1]) > 1024) { set_err(err, "link: bad target '%s'", tok[1]); return -1; }
        if (!valid_relpath(tok[2]) || !path_allowed_for_package(tok[2])) { set_err(err, "link: path not allowed '%s'", tok[2]); return -1; }
        e.target = xstrdup(tok[1]); e.path = xstrdup(tok[2]);
    }
    m->entries = xrealloc(m->entries, (m->nentries + 1) * sizeof *m->entries); m->entries[m->nentries++] = e; return 0;
}

int meta_parse(const char *buf, size_t len, Meta *m, int allow_manifest, char *err)
{
    memset(m, 0, sizeof *m);
    if (len == 0 || buf[len - 1] != '\n') { set_err(err, "metadata must end with a newline"); return -1; }
    for (size_t i = 0; i < len; i++) { unsigned char c = (unsigned char)buf[i]; if (c == 0 || (c < 32 && c != '\n') || c > 126) { set_err(err, "metadata contains non-printable byte at offset %zu", i); return -1; } }
    char *copy = xmalloc(len + 1); memcpy(copy, buf, len); copy[len] = 0;
    int lineno = 0, rc = -1, saw_format = 0;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); ; line = strtok_r(NULL, "\n", &save)) {
        if (!line) break;
        lineno++;
        if (*line == 0) { set_err(err, "line %d: empty line", lineno); goto out; }
        char *tok[8]; int n = 0; char *p = line;
        if (!strncmp(line, "summary ", 8)) {
            if (strlen(line + 8) > 200) { set_err(err, "line %d: summary too long", lineno); goto out; }
            if (m->summary) { set_err(err, "line %d: duplicate summary", lineno); goto out; }
            m->summary = xstrdup(line + 8); continue;
        }
        while (1) {
            if (n == 8) { set_err(err, "line %d: too many fields", lineno); goto out; }
            tok[n++] = p; char *sp = strchr(p, ' ');
            if (!sp) break;
            *sp = 0; p = sp + 1;
            if (*p == 0 || *p == ' ') { set_err(err, "line %d: empty field", lineno); goto out; }
        }
        char lerr[PM_ERRLEN] = {0};
        if (lineno == 1) {
            if (n != 2 || strcmp(tok[0], "format") || strcmp(tok[1], "1")) { set_err(err, "line 1 must be 'format 1' (unsupported format)"); goto out; }
            saw_format = 1; continue;
        }
        if (!strcmp(tok[0], "dir") || !strcmp(tok[0], "file") || !strcmp(tok[0], "link")) {
            if (!allow_manifest) { set_err(err, "line %d: manifest entries not allowed here", lineno); goto out; }
            if (add_entry(m, tok, n, lerr) != 0) { set_err(err, "line %d: %s", lineno, lerr); goto out; }
            continue;
        }
        if (meta_apply_line(m, tok, n, lerr) != 0) { set_err(err, "line %d: %s", lineno, lerr); goto out; }
    }
    if (!saw_format) { set_err(err, "missing format line"); goto out; }
    if (meta_validate(m, allow_manifest, err) != 0) goto out;
    rc = 0;
out:
    free(copy); if (rc) meta_free(m); return rc;
}

/* ---------- serialization ---------- */
typedef struct { char *b; size_t n, cap; } Sb;
static void sb_add(Sb *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_add(Sb *s, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); char tmp[4400]; int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) { fputs("aether-pm: metadata line too long\n", stderr); exit(70); }
    if (s->n + (size_t)n + 1 > s->cap) { s->cap = (s->n + (size_t)n + 1) * 2; s->b = xrealloc(s->b, s->cap); }
    memcpy(s->b + s->n, tmp, (size_t)n + 1); s->n += (size_t)n;
}
char *meta_serialize(const Meta *m, int with_manifest)
{
    Sb s = {0};
    sb_add(&s, "format 1\nname %s\nversion %s\narch %s\nlibc %s\n", m->name, m->version, m->arch, m->libc);
    if (m->summary) sb_add(&s, "summary %s\n", m->summary);
    if (m->essential) sb_add(&s, "essential yes\n");
    for (size_t i = 0; i < m->ndepends; i++) { char *a = atom_str(&m->depends[i]); sb_add(&s, "depends %s\n", a); free(a); }
    for (size_t i = 0; i < m->nconflicts; i++) { char *a = atom_str(&m->conflicts[i]); sb_add(&s, "conflicts %s\n", a); free(a); }
    for (size_t i = 0; i < m->nprovides; i++) { if (m->provides[i].ver) sb_add(&s, "provides %s = %s\n", m->provides[i].name, m->provides[i].ver); else sb_add(&s, "provides %s\n", m->provides[i].name); }
    for (size_t i = 0; i < m->nneeds_so; i++) sb_add(&s, "needs-soname %s\n", m->needs_so[i]);
    for (size_t i = 0; i < m->nprovides_so; i++) sb_add(&s, "provides-soname %s\n", m->provides_so[i]);
    if (with_manifest) for (size_t i = 0; i < m->nentries; i++) {
        const Entry *e = &m->entries[i]; char h[65];
        if (e->type == 'd') sb_add(&s, "dir %04o %s\n", e->mode, e->path);
        else if (e->type == 'f') { hex_encode(e->sha, 32, h); sb_add(&s, "file %04o %llu %s %s\n", e->mode, (unsigned long long)e->size, h, e->path); }
        else sb_add(&s, "link %s %s\n", e->target, e->path);
    }
    return s.b;
}

/* ---------- container I/O ---------- */
static uint32_t get_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static int read_full(int fd, void *buf, size_t n, uint64_t off)
{
    char *p = buf; size_t got = 0;
    while (got < n) { ssize_t r = pread(fd, p + got, n - got, (off_t)(off + got)); if (r < 0 && errno == EINTR) continue; if (r <= 0) return -1; got += (size_t)r; }
    return 0;
}

int pkg_hash_file(const char *path, uint8_t out[32], uint64_t *size, char *err)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); if (fd < 0) { set_err(err, "%s: %s", path, strerror(errno)); return -1; }
    Sha s; sha_init(&s); uint8_t buf[65536]; uint64_t total = 0;
    for (;;) { ssize_t r = read(fd, buf, sizeof buf); if (r < 0 && errno == EINTR) continue; if (r < 0) { set_err(err, "%s: %s", path, strerror(errno)); close(fd); return -1; } if (r == 0) break; sha_update(&s, buf, (size_t)r); total += (uint64_t)r; }
    close(fd); sha_final(&s, out); if (size) *size = total; return 0;
}

int pkg_open(const char *path, PkgFile *p, char *err)
{
    memset(p, 0, sizeof *p); p->fd = -1;
    p->fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (p->fd < 0) { set_err(err, "%s: %s", path, strerror(errno)); return -1; }
    struct stat st;
    if (fstat(p->fd, &st) != 0 || !S_ISREG(st.st_mode)) { set_err(err, "%s: not a regular file", path); goto fail; }
    uint8_t h[PKG_HDR_LEN];
    if ((uint64_t)st.st_size < PKG_HDR_LEN || read_full(p->fd, h, sizeof h, 0) != 0) { set_err(err, "%s: truncated (no header)", path); goto fail; }
    if (memcmp(h, PKG_MAGIC, 8) != 0) { set_err(err, "%s: not an .aether package (bad magic)", path); goto fail; }
    if (get_le32(h + 8) != 1) { set_err(err, "%s: unsupported package format %u", path, get_le32(h + 8)); goto fail; }
    uint32_t ml = get_le32(h + 12);
    if (ml == 0 || ml > PKG_META_MAX || (uint64_t)ml > (uint64_t)st.st_size - PKG_HDR_LEN) { set_err(err, "%s: bad metadata length", path); goto fail; }
    memcpy(p->keyid, h + 16, PM_KEYID_LEN); memcpy(p->sig, h + 24, PM_SIG_LEN);
    static const uint8_t zero[PM_KEYID_LEN] = {0}; p->is_signed = memcmp(p->keyid, zero, sizeof zero) != 0;
    p->raw_meta = xmalloc(ml + 1); p->raw_meta_len = ml;
    if (read_full(p->fd, p->raw_meta, ml, PKG_HDR_LEN) != 0) { set_err(err, "%s: truncated metadata", path); goto fail; }
    p->raw_meta[ml] = 0;
    char perr[PM_ERRLEN];
    if (meta_parse((const char *)p->raw_meta, ml, &p->meta, 1, perr) != 0) { set_err(err, "%s: invalid metadata: %s", path, perr); goto fail; }
    p->payload_off = PKG_HDR_LEN + ml;
    if ((uint64_t)st.st_size != p->payload_off + p->meta.payload_len) { set_err(err, "%s: size mismatch (file %llu, expected %llu): truncated or has trailing data", path, (unsigned long long)st.st_size, (unsigned long long)(p->payload_off + p->meta.payload_len)); meta_free(&p->meta); goto fail; }
    sha_buf(p->raw_meta, ml, p->meta_sha);
    p->path = xstrdup(path); return 0;
fail:
    if (p->fd >= 0) close(p->fd);
    free(p->raw_meta); memset(p, 0, sizeof *p); p->fd = -1; return -1;
}
void pkg_close(PkgFile *p) { if (p->fd >= 0) close(p->fd); meta_free(&p->meta); free(p->raw_meta); free(p->path); memset(p, 0, sizeof *p); p->fd = -1; }

int pkg_verify_signature(const PkgFile *p, const Keyring *kr, char *err)
{
    if (!p->is_signed) { set_err(err, "package is unsigned"); return -1; }
    const TrustedKey *k = keyring_find(kr, p->keyid);
    if (!k) { char h[2 * PM_KEYID_LEN + 1]; hex_encode(p->keyid, PM_KEYID_LEN, h); set_err(err, "signed by untrusted key %s", h); return -1; }
    if (verify_detached(PM_DOMAIN_PKG, p->raw_meta, p->raw_meta_len, k->pk, p->sig) != 0) { set_err(err, "BAD SIGNATURE on package metadata"); return -1; }
    return 0;
}

int pkg_verify_payload(const PkgFile *p, char *err)
{
    uint8_t buf[65536];
    for (size_t i = 0; i < p->meta.nentries; i++) {
        const Entry *e = &p->meta.entries[i]; if (e->type != 'f') continue;
        Sha s; sha_init(&s); uint64_t left = e->size, off = p->payload_off + e->off;
        while (left) { size_t n = left > sizeof buf ? sizeof buf : (size_t)left; if (read_full(p->fd, buf, n, off) != 0) { set_err(err, "read error in %s", e->path); return -1; } sha_update(&s, buf, n); off += n; left -= n; }
        uint8_t d[32]; sha_final(&s, d);
        if (sodium_memcmp(d, e->sha, 32) != 0) { set_err(err, "checksum mismatch for %s", e->path); return -1; }
    }
    return 0;
}

/* ---------- builder ---------- */
static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int walk(const char *root, const char *rel, Meta *m, char *err)
{
    char *dir = *rel ? xsprintf("%s/%s", root, rel) : xstrdup(root);
    DIR *d = opendir(dir); if (!d) { set_err(err, "%s: %s", dir, strerror(errno)); free(dir); return -1; }
    char **names = NULL; size_t nn = 0; struct dirent *e;
    while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; names = xrealloc(names, (nn + 1) * sizeof *names); names[nn++] = xstrdup(e->d_name); }
    closedir(d); qsort(names, nn, sizeof *names, cmp_names);
    int rc = 0;
    for (size_t i = 0; i < nn && rc == 0; i++) {
        char *r = *rel ? xsprintf("%s/%s", rel, names[i]) : xstrdup(names[i]);
        char *full = xsprintf("%s/%s", root, r); struct stat st;
        if (lstat(full, &st) != 0) { set_err(err, "%s: %s", full, strerror(errno)); rc = -1; goto next; }
        if (!valid_relpath(r)) { set_err(err, "path not representable in package: '%s'", r); rc = -1; goto next; }
        if (!path_allowed_for_package(r)) { set_err(err, "path not allowed in a package: '%s'", r); rc = -1; goto next; }
        if (st.st_mode & 07000) { set_err(err, "%s: setuid/setgid/sticky bits are not allowed", r); rc = -1; goto next; }
        Entry en; memset(&en, 0, sizeof en); en.path = xstrdup(r); en.mode = st.st_mode & 0777;
        if (S_ISDIR(st.st_mode)) {
            en.type = 'd'; m->entries = xrealloc(m->entries, (m->nentries + 1) * sizeof *m->entries); m->entries[m->nentries++] = en;
            rc = walk(root, r, m, err);
        } else if (S_ISREG(st.st_mode)) {
            en.type = 'f'; if (pkg_hash_file(full, en.sha, &en.size, err) != 0) { free(en.path); rc = -1; goto next; }
            en.off = m->payload_len; m->payload_len += en.size;
            m->entries = xrealloc(m->entries, (m->nentries + 1) * sizeof *m->entries); m->entries[m->nentries++] = en;
        } else if (S_ISLNK(st.st_mode)) {
            char tg[4097]; ssize_t n = readlink(full, tg, sizeof tg - 1);
            if (n <= 0) { set_err(err, "%s: unreadable symlink", full); free(en.path); rc = -1; goto next; }
            tg[n] = 0;
            if (!valid_token(tg) || n > 1024) { set_err(err, "%s: symlink target not representable '%s'", r, tg); free(en.path); rc = -1; goto next; }
            en.type = 'l'; en.mode = 0777; en.target = xstrdup(tg);
            m->entries = xrealloc(m->entries, (m->nentries + 1) * sizeof *m->entries); m->entries[m->nentries++] = en;
        } else { set_err(err, "%s: unsupported file type (devices, fifos, sockets are not packageable)", r); free(en.path); rc = -1; }
next:
        free(r); free(full);
    }
    for (size_t i = 0; i < nn; i++) free(names[i]);
    free(names); free(dir); return rc;
}

int pkg_build(const char *infopath, const char *rootdir, const char *out, const uint8_t *sk, char *err)
{
    size_t il; char *info = read_file(infopath, &il, 1 << 20, err); if (!info) return -1;
    Meta m; char perr[PM_ERRLEN];
    if (meta_parse(info, il, &m, 0, perr) != 0) { set_err(err, "%s: %s", infopath, perr); free(info); return -1; }
    free(info);
    int rc = -1; int ofd = -1; char *tmp = NULL, *meta = NULL;
    if (walk(rootdir, "", &m, err) != 0) goto done;
    if (m.nentries == 0) { set_err(err, "payload tree is empty"); goto done; }
    if (meta_validate(&m, 1, err) != 0) goto done;
    meta = meta_serialize(&m, 1); size_t ml = strlen(meta);
    if (ml > PKG_META_MAX) { set_err(err, "metadata too large"); goto done; }
    uint8_t hdr[PKG_HDR_LEN]; memset(hdr, 0, sizeof hdr); memcpy(hdr, PKG_MAGIC, 8); put_le32(hdr + 8, 1); put_le32(hdr + 12, (uint32_t)ml);
    if (sk) {
        uint8_t pk[32]; crypto_sign_ed25519_sk_to_pk(pk, sk); key_id(pk, hdr + 16);
        if (sign_detached(PM_DOMAIN_PKG, meta, ml, sk, hdr + 24) != 0) { set_err(err, "signing failed"); goto done; }
    }
    tmp = xsprintf("%s.tmp.%d", out, (int)getpid());
    ofd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (ofd < 0) { set_err(err, "%s: %s", tmp, strerror(errno)); goto done; }
    if (write(ofd, hdr, sizeof hdr) != (ssize_t)sizeof hdr || write(ofd, meta, ml) != (ssize_t)ml) { set_err(err, "write error"); goto done; }
    for (size_t i = 0; i < m.nentries; i++) {
        const Entry *e = &m.entries[i]; if (e->type != 'f') continue;
        char *full = xsprintf("%s/%s", rootdir, e->path); int fd = open(full, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) { set_err(err, "%s: %s", full, strerror(errno)); free(full); goto done; }
        Sha s; sha_init(&s); uint8_t buf[65536]; uint64_t total = 0;
        for (;;) {
            ssize_t r = read(fd, buf, sizeof buf); if (r < 0 && errno == EINTR) continue; if (r <= 0) break;
            sha_update(&s, buf, (size_t)r); total += (uint64_t)r;
            if (write(ofd, buf, (size_t)r) != r) { set_err(err, "write error"); close(fd); free(full); goto done; }
        }
        close(fd); uint8_t d[32]; sha_final(&s, d);
        if (total != e->size || memcmp(d, e->sha, 32) != 0) { set_err(err, "%s changed while building", e->path); free(full); goto done; }
        free(full);
    }
    if (fsync(ofd) != 0 || close(ofd) != 0) { ofd = -1; set_err(err, "write error"); goto done; }
    ofd = -1;
    if (rename(tmp, out) != 0) { set_err(err, "%s: %s", out, strerror(errno)); goto done; }
    rc = 0;
done:
    if (ofd >= 0) close(ofd);
    if (rc && tmp) unlink(tmp);
    free(tmp); free(meta); meta_free(&m); return rc;
}
