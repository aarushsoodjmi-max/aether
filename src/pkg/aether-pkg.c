/*
 * aether-pkg - Aether package manager (foundation).
 *
 *   aether-pkg [-r ROOT] build DIR OUT.apkg NAME VERSION
 *   aether-pkg [-r ROOT] install FILE.apkg
 *   aether-pkg [-r ROOT] remove NAME
 *   aether-pkg [-r ROOT] list
 *   aether-pkg [-r ROOT] files NAME
 *   aether-pkg [-r ROOT] verify [NAME]
 *
 * An .apkg is a "newc" cpio archive whose first entry is .PKGINFO:
 *   NAME=<id>  VERSION=<id>  FILE=<sha256> <path>   (one per regular file)
 * Install verifies every checksum, rejects unsafe paths (absolute, "..",
 * through symlinks), refuses to overwrite files it does not own, writes each
 * file atomically (temp + rename) and records ownership in
 * ROOT/var/lib/aether/pkg/NAME.list. Packages are NOT signed yet.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include "sha256.h"

#define MAX_ARCHIVE (512UL * 1024 * 1024)
#define DBDIR "var/lib/aether/pkg"

typedef struct {
    char *path;
    unsigned mode;
    unsigned char *data;
    size_t size;
} Ent;

typedef struct { Ent *v; size_t n, cap; } EntList;

static const char *root = "";

static int fail(const char *what, const char *arg)
{
    fprintf(stderr, "aether-pkg: %s%s%s\n", what, arg ? ": " : "", arg ? arg : "");
    return -1;
}

static int valid_ident(const char *s)
{
    size_t n = strlen(s);

    if (n == 0 || n > 63 || s[0] == '.')
        return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-'))
            return 0;
    }
    return 1;
}

static int valid_relpath(const char *p)
{
    size_t n = strlen(p);

    if (n == 0 || n >= 1024 || p[0] == '/' || p[n - 1] == '/')
        return 0;
    for (const char *c = p; *c; c++)
        if ((unsigned char)*c < 0x20)
            return 0;
    const char *comp = p;
    for (;;) {
        const char *end = strchr(comp, '/');
        size_t len = end ? (size_t)(end - comp) : strlen(comp);
        if (len == 0 || (len == 1 && comp[0] == '.') ||
            (len == 2 && comp[0] == '.' && comp[1] == '.'))
            return 0;
        if (!end)
            return 1;
        comp = end + 1;
    }
}

static char *xpath(const char *a, const char *b)
{
    size_t n = strlen(a) + strlen(b) + 2;
    char *p = malloc(n);

    if (!p) { perror("malloc"); exit(1); }
    snprintf(p, n, "%s/%s", a, b);
    return p;
}

static int read_all(const char *path, unsigned char **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return fail(strerror(errno), path);
    size_t cap = 4096, n = 0;
    unsigned char *b = malloc(cap);
    for (;;) {
        if (n == cap) {
            if (cap > MAX_ARCHIVE) { fclose(f); free(b); return fail("file too large", path); }
            cap *= 2;
            b = realloc(b, cap);
        }
        if (!b) { fclose(f); return fail("out of memory", NULL); }
        size_t got = fread(b + n, 1, cap - n, f);
        n += got;
        if (got == 0)
            break;
    }
    int err = ferror(f);
    fclose(f);
    if (err) { free(b); return fail("read error", path); }
    *buf = b;
    *len = n;
    return 0;
}

static void list_add(EntList *l, const char *path, unsigned mode,
                     unsigned char *data, size_t size)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->v = realloc(l->v, l->cap * sizeof(Ent));
        if (!l->v) { perror("realloc"); exit(1); }
    }
    l->v[l->n].path = strdup(path);
    l->v[l->n].mode = mode;
    l->v[l->n].data = data;
    l->v[l->n].size = size;
    l->n++;
}

static int ent_cmp(const void *a, const void *b)
{
    return strcmp(((const Ent *)a)->path, ((const Ent *)b)->path);
}

/* ---------------- archive reading ---------------- */

static int hex8(const unsigned char *p, unsigned long *out)
{
    unsigned long v = 0;

    for (int i = 0; i < 8; i++) {
        unsigned char c = p[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else return -1;
        v = v * 16 + d;
    }
    *out = v;
    return 0;
}

static int parse_archive(unsigned char *buf, size_t len, EntList *out)
{
    size_t pos = 0;

    for (;;) {
        unsigned long f[13];

        if (len - pos < 110 || memcmp(buf + pos, "070701", 6) != 0)
            return fail("corrupt archive (bad header)", NULL);
        for (int i = 0; i < 13; i++)
            if (hex8(buf + pos + 6 + 8 * (size_t)i, &f[i]) < 0)
                return fail("corrupt archive (bad field)", NULL);
        unsigned long mode = f[1], size = f[6], namesz = f[11];
        size_t name_off = pos + 110;

        if (namesz == 0 || namesz > 1100 || namesz > len - name_off)
            return fail("corrupt archive (bad name)", NULL);
        if (buf[name_off + namesz - 1] != 0)
            return fail("corrupt archive (unterminated name)", NULL);
        char *name = (char *)buf + name_off;
        pos = (name_off + namesz + 3) & ~(size_t)3;
        if (size > len || pos > len || size > len - pos)
            return fail("corrupt archive (truncated)", NULL);
        if (strcmp(name, "TRAILER!!!") == 0)
            return 0;
        list_add(out, name, (unsigned)mode, buf + pos, size);
        pos = (pos + size + 3) & ~(size_t)3;
        if (pos > len)
            return fail("corrupt archive (truncated)", NULL);
    }
}

/* ---------------- .PKGINFO ---------------- */

typedef struct {
    char name[64], version[64];
    char **hash, **path;     /* parallel arrays of FILE= lines */
    size_t n;
} PkgInfo;

static int parse_info(const unsigned char *data, size_t size, PkgInfo *pi)
{
    char *text = malloc(size + 1);
    if (!text) return fail("out of memory", NULL);
    memcpy(text, data, size);
    text[size] = '\0';
    memset(pi, 0, sizeof(*pi));

    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "NAME=", 5) == 0 && !pi->name[0] && strlen(line + 5) < 64) {
            strcpy(pi->name, line + 5);
        } else if (strncmp(line, "VERSION=", 8) == 0 && !pi->version[0] && strlen(line + 8) < 64) {
            strcpy(pi->version, line + 8);
        } else if (strncmp(line, "FILE=", 5) == 0 && strlen(line) > 5 + 65) {
            char *h = line + 5;
            if (h[64] != ' ')
                return fail(".PKGINFO: bad FILE line", NULL);
            h[64] = '\0';
            for (int i = 0; i < 64; i++)
                if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f')))
                    return fail(".PKGINFO: bad checksum", NULL);
            if (!valid_relpath(h + 65))
                return fail(".PKGINFO: unsafe path", h + 65);
            pi->hash = realloc(pi->hash, (pi->n + 1) * sizeof(char *));
            pi->path = realloc(pi->path, (pi->n + 1) * sizeof(char *));
            pi->hash[pi->n] = strdup(h);
            pi->path[pi->n] = strdup(h + 65);
            pi->n++;
        } else {
            return fail(".PKGINFO: unrecognised or duplicate line", line);
        }
    }
    if (!valid_ident(pi->name) || !valid_ident(pi->version))
        return fail(".PKGINFO: missing or invalid NAME/VERSION", NULL);
    return 0;
}

/* ---------------- database ---------------- */

static char *dbpath(const char *name, const char *suffix)
{
    char tail[160];
    snprintf(tail, sizeof(tail), DBDIR "/%s%s", name, suffix);
    return xpath(*root ? root : ".", tail);
}

static int mkdir_p_db(void)
{
    char *p = xpath(*root ? root : ".", DBDIR);
    int rc = 0;

    for (char *c = p + 1; ; c++) {
        if (*c == '/' || *c == '\0') {
            char save = *c;
            *c = '\0';
            if (mkdir(p, 0755) != 0 && errno != EEXIST)
                rc = fail(strerror(errno), p);
            *c = save;
            if (!save)
                break;
        }
    }
    free(p);
    return rc;
}

/* Lines of NAME.list: "version V", "d path", "f sha path", "l path". */
static int load_list(const char *name, char ***lines, size_t *n)
{
    unsigned char *buf; size_t len;
    char *p = dbpath(name, ".list");
    if (access(p, F_OK) != 0) { free(p); return -1; }   /* absent: not an error */
    int rc = read_all(p, &buf, &len);
    free(p);
    if (rc < 0) return -1;
    buf = realloc(buf, len + 1);
    buf[len] = '\0';
    *lines = NULL; *n = 0;
    char *save = NULL;
    for (char *l = strtok_r((char *)buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        *lines = realloc(*lines, (*n + 1) * sizeof(char *));
        (*lines)[(*n)++] = strdup(l);
    }
    free(buf);
    return 0;
}

static int installed_names(char ***names, size_t *n)
{
    char *dir = xpath(*root ? root : ".", DBDIR);
    DIR *d = opendir(dir);
    free(dir);
    *names = NULL; *n = 0;
    if (!d)
        return errno == ENOENT ? 0 : -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l > 5 && strcmp(de->d_name + l - 5, ".list") == 0) {
            *names = realloc(*names, (*n + 1) * sizeof(char *));
            (*names)[*n] = strndup(de->d_name, l - 5);
            (*n)++;
        }
    }
    closedir(d);
    return 0;
}

/* Which package owns this path (f/l entries)? NULL if none; skips `except`. */
static char *owner_of(const char *path, const char *except)
{
    char **names; size_t nn;
    if (installed_names(&names, &nn) < 0) return NULL;
    for (size_t i = 0; i < nn; i++) {
        if (except && strcmp(names[i], except) == 0) continue;
        char **lines; size_t nl;
        if (load_list(names[i], &lines, &nl) < 0) continue;
        for (size_t j = 0; j < nl; j++) {
            const char *l = lines[j];
            const char *lp = l[0] == 'f' ? l + 2 + 65 : l[0] == 'l' ? l + 2 : NULL;
            if (lp && (l[0] == 'l' || strlen(l) > 67) && strcmp(lp, path) == 0)
                return names[i];
        }
    }
    return NULL;
}

/* ---------------- filesystem helpers ---------------- */

/* Create parent directories of rel under root; refuse symlinked components. */
static int ensure_parents(const char *rel)
{
    char *full = xpath(*root ? root : ".", rel);
    size_t rootlen = strlen(*root ? root : ".");
    int rc = 0;

    for (char *c = full + rootlen + 1; *c && rc == 0; c++) {
        if (*c != '/') continue;
        *c = '\0';
        struct stat st;
        if (lstat(full, &st) == 0) {
            if (!S_ISDIR(st.st_mode))
                rc = fail("path component is not a directory (or is a symlink)", full);
        } else if (mkdir(full, 0755) != 0) {
            rc = fail(strerror(errno), full);
        }
        *c = '/';
    }
    free(full);
    return rc;
}

static int write_file_atomic(const char *full, const unsigned char *data,
                             size_t size, unsigned mode)
{
    size_t n = strlen(full) + 16;
    char *tmp = malloc(n);
    snprintf(tmp, n, "%s.aether-new", full);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode & 07777);
    int rc = -1;

    if (fd < 0) { fail(strerror(errno), tmp); goto out; }
    size_t off = 0;
    while (off < size) {
        ssize_t w = write(fd, data + off, size - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { fail("write failed", tmp); close(fd); unlink(tmp); goto out; }
        off += (size_t)w;
    }
    if (fchmod(fd, mode & 07777) != 0 || fsync(fd) != 0) {
        fail(strerror(errno), tmp); close(fd); unlink(tmp); goto out;
    }
    close(fd);
    if (rename(tmp, full) != 0) { fail(strerror(errno), full); unlink(tmp); goto out; }
    rc = 0;
out:
    free(tmp);
    return rc;
}

/* ---------------- commands ---------------- */

static int cmd_install(const char *file)
{
    unsigned char *buf; size_t len;
    EntList ents = {0};
    PkgInfo pi;

    if (read_all(file, &buf, &len) < 0 || parse_archive(buf, len, &ents) < 0)
        return -1;
    if (ents.n == 0 || strcmp(ents.v[0].path, ".PKGINFO") != 0)
        return fail("archive does not start with .PKGINFO", NULL);
    if (parse_info(ents.v[0].data, ents.v[0].size, &pi) < 0)
        return -1;

    /* Validate every entry before touching the filesystem. */
    size_t verified = 0;
    for (size_t i = 1; i < ents.n; i++) {
        Ent *e = &ents.v[i];
        unsigned type = e->mode & 0170000;

        if (!valid_relpath(e->path) || strcmp(e->path, ".PKGINFO") == 0)
            return fail("unsafe path in package", e->path);
        for (size_t j = 1; j < i; j++)
            if (strcmp(ents.v[j].path, e->path) == 0)
                return fail("duplicate entry in package", e->path);
        if (type == 0100000) {
            char hex[65];
            size_t k;
            sha256_hex(e->data, e->size, hex);
            for (k = 0; k < pi.n && strcmp(pi.path[k], e->path) != 0; k++) {}
            if (k == pi.n)
                return fail("file not listed in .PKGINFO", e->path);
            if (strcmp(pi.hash[k], hex) != 0)
                return fail("checksum mismatch", e->path);
            verified++;
        } else if (type == 0040000) {
        } else if (type == 0120000) {
            if (e->size == 0 || e->size >= 1024 || memchr(e->data, 0, e->size))
                return fail("bad symlink target", e->path);
        } else {
            return fail("unsupported file type in package", e->path);
        }
    }
    if (verified != pi.n)
        return fail(".PKGINFO lists files missing from the archive", NULL);

    /* Ownership conflicts (files/symlinks only). */
    for (size_t i = 1; i < ents.n; i++) {
        unsigned type = ents.v[i].mode & 0170000;
        if (type == 0040000) continue;
        char *own = owner_of(ents.v[i].path, pi.name);
        if (own) {
            fprintf(stderr, "aether-pkg: %s is owned by package %s\n", ents.v[i].path, own);
            return -1;
        }
        char *full = xpath(*root ? root : ".", ents.v[i].path);
        struct stat st;
        int exists = lstat(full, &st) == 0;
        free(full);
        if (exists) {
            char **ol; size_t nol; int mine = 0;
            if (load_list(pi.name, &ol, &nol) == 0)
                for (size_t j = 0; j < nol; j++) {
                    const char *p = ol[j][0] == 'f' ? ol[j] + 2 + 65 : ol[j][0] == 'l' ? ol[j] + 2 : NULL;
                    if (p && (ol[j][0] == 'l' || strlen(ol[j]) > 67) && strcmp(p, ents.v[i].path) == 0) mine = 1;
                }
            if (!mine)
                return fail("refusing to overwrite file not owned by this package", ents.v[i].path);
        }
    }

    if (mkdir_p_db() < 0)
        return -1;

    /* Previous version's file list, for removal of stale files. */
    char **old = NULL; size_t nold = 0;
    int upgrading = load_list(pi.name, &old, &nold) == 0;

    /* Install. Directories first (sorted order puts parents first). */
    qsort(ents.v + 1, ents.n - 1, sizeof(Ent), ent_cmp);
    size_t listcap = 1024 + ents.n * 1200, used = 0;
    char *list = malloc(listcap);
    used += (size_t)snprintf(list + used, listcap - used, "version %s\n", pi.version);

    for (size_t i = 1; i < ents.n; i++) {
        Ent *e = &ents.v[i];
        unsigned type = e->mode & 0170000;
        char *full = xpath(*root ? root : ".", e->path);

        if (ensure_parents(e->path) < 0) { free(full); return -1; }
        if (type == 0040000) {
            struct stat st;
            if (lstat(full, &st) == 0 ? !S_ISDIR(st.st_mode)
                                      : mkdir(full, e->mode & 0777) != 0) {
                fail("cannot create directory", full); free(full); return -1;
            }
            used += (size_t)snprintf(list + used, listcap - used, "d %s\n", e->path);
        } else if (type == 0100000) {
            char hex[65];
            sha256_hex(e->data, e->size, hex);
            if (write_file_atomic(full, e->data, e->size, e->mode) < 0) { free(full); return -1; }
            used += (size_t)snprintf(list + used, listcap - used, "f %s %s\n", hex, e->path);
        } else {
            char *target = strndup((char *)e->data, e->size);
            size_t n = strlen(full) + 16;
            char *tmp = malloc(n);
            snprintf(tmp, n, "%s.aether-new", full);
            unlink(tmp);
            if (symlink(target, tmp) != 0 || rename(tmp, full) != 0) {
                fail(strerror(errno), full); unlink(tmp); free(full); return -1;
            }
            free(tmp); free(target);
            used += (size_t)snprintf(list + used, listcap - used, "l %s\n", e->path);
        }
        free(full);
    }

    if (upgrading) {
        for (size_t j = 0; j < nold; j++) {
            const char *p = old[j][0] == 'f' ? old[j] + 2 + 65 : old[j][0] == 'l' ? old[j] + 2 : NULL;
            if (!p) continue;
            int keep = 0;
            for (size_t i = 1; i < ents.n; i++)
                if (strcmp(ents.v[i].path, p) == 0) keep = 1;
            if (!keep) {
                char *full = xpath(*root ? root : ".", p);
                unlink(full);
                free(full);
            }
        }
    }

    char *lp = dbpath(pi.name, ".list");
    char *tmp = dbpath(pi.name, ".list.new");
    FILE *f = fopen(tmp, "w");
    if (!f || fwrite(list, 1, used, f) != used || fclose(f) != 0 || rename(tmp, lp) != 0)
        return fail("cannot write package database", lp);
    printf("%s %s: %s (%zu files)\n", upgrading ? "upgraded" : "installed",
           pi.name, pi.version, pi.n);
    return 0;
}

static int cmd_remove(const char *name)
{
    char **lines; size_t n;

    if (!valid_ident(name)) return fail("invalid package name", name);
    if (load_list(name, &lines, &n) < 0) return fail("package not installed", name);

    for (size_t i = 0; i < n; i++) {
        const char *p = lines[i][0] == 'f' ? lines[i] + 2 + 65 : lines[i][0] == 'l' ? lines[i] + 2 : NULL;
        if (!p || !valid_relpath(p)) continue;
        char *full = xpath(*root ? root : ".", p);
        unlink(full);
        free(full);
    }
    for (size_t i = n; i-- > 0;) {       /* directories, deepest first */
        if (lines[i][0] != 'd' || !valid_relpath(lines[i] + 2)) continue;
        char *full = xpath(*root ? root : ".", lines[i] + 2);
        rmdir(full);                      /* fails harmlessly if not empty */
        free(full);
    }
    char *lp = dbpath(name, ".list");
    unlink(lp);
    free(lp);
    printf("removed %s\n", name);
    return 0;
}

static int cmd_list(void)
{
    char **names; size_t n;
    if (installed_names(&names, &n) < 0) return fail("cannot read database", NULL);
    for (size_t i = 0; i < n; i++) {
        char **lines; size_t nl;
        if (load_list(names[i], &lines, &nl) == 0 && nl && strncmp(lines[0], "version ", 8) == 0)
            printf("%s %s\n", names[i], lines[0] + 8);
    }
    return 0;
}

static int cmd_files(const char *name)
{
    char **lines; size_t n;
    if (!valid_ident(name) || load_list(name, &lines, &n) < 0)
        return fail("package not installed", name);
    for (size_t i = 0; i < n; i++) {
        const char *p = lines[i][0] == 'f' ? lines[i] + 2 + 65 : (lines[i][0] == 'l' || lines[i][0] == 'd') ? lines[i] + 2 : NULL;
        if (p) printf("%s\n", p);
    }
    return 0;
}

static int verify_one(const char *name)
{
    char **lines; size_t n; int bad = 0;
    if (load_list(name, &lines, &n) < 0) return fail("package not installed", name);
    for (size_t i = 0; i < n; i++) {
        if (lines[i][0] != 'f' || strlen(lines[i]) <= 67) continue;
        char *full = xpath(*root ? root : ".", lines[i] + 2 + 65);
        unsigned char *data; size_t len; char hex[65];
        if (access(full, F_OK) != 0 || read_all(full, &data, &len) < 0) {
            printf("MISSING  %s\n", lines[i] + 2 + 65); bad++;
        } else {
            sha256_hex(data, len, hex);
            lines[i][2 + 64] = '\0';
            if (strcmp(hex, lines[i] + 2) != 0) { printf("MODIFIED %s\n", lines[i] + 2 + 65); bad++; }
            free(data);
        }
        free(full);
    }
    return bad ? -1 : 0;
}

static int cmd_verify(const char *name)
{
    if (name) return verify_one(name);
    char **names; size_t n; int bad = 0;
    if (installed_names(&names, &n) < 0) return -1;
    for (size_t i = 0; i < n; i++)
        if (verify_one(names[i]) < 0) bad = 1;
    return bad ? -1 : 0;
}

static void collect(const char *disk, const char *rel, EntList *l)
{
    DIR *d = opendir(disk);
    struct dirent *de;
    if (!d) { fail(strerror(errno), disk); exit(1); }
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *dp = xpath(disk, de->d_name);
        char *rp = rel[0] ? xpath(rel, de->d_name) : strdup(de->d_name);
        struct stat st;
        if (lstat(dp, &st) != 0) { fail(strerror(errno), dp); exit(1); }
        if (!valid_relpath(rp)) { fail("unsafe file name", rp); exit(1); }
        unsigned mode = st.st_mode & (0170000 | 07777);
        if (S_ISDIR(st.st_mode)) {
            list_add(l, rp, mode, NULL, 0);
            collect(dp, rp, l);
        } else if (S_ISREG(st.st_mode)) {
            unsigned char *data; size_t len;
            if (read_all(dp, &data, &len) < 0) exit(1);
            list_add(l, rp, mode, data, len);
        } else if (S_ISLNK(st.st_mode)) {
            char *t = malloc((size_t)st.st_size + 1);
            ssize_t k = readlink(dp, t, (size_t)st.st_size);
            if (k <= 0) { fail("cannot read link", dp); exit(1); }
            list_add(l, rp, mode, (unsigned char *)t, (size_t)k);
        } else {
            fail("unsupported file type", dp); exit(1);
        }
        free(dp); free(rp);
    }
    closedir(d);
}

static void put_entry(FILE *o, const char *name, unsigned mode,
                      const unsigned char *data, size_t size, unsigned long ino)
{
    static unsigned long written;
    size_t namesz = strlen(name) + 1;
    static const unsigned char zeros[4] = {0};

    fprintf(o, "070701%08lX%08X%08X%08X%08X%08X%08lX%08X%08X%08X%08X%08lX%08X",
            ino, mode, 0u, 0u, 1u, 0u, (unsigned long)size, 0u, 0u, 0u, 0u,
            (unsigned long)namesz, 0u);
    fwrite(name, 1, namesz, o);
    written += 110 + namesz;
    fwrite(zeros, 1, (4 - written % 4) % 4, o); written += (4 - written % 4) % 4;
    if (size) fwrite(data, 1, size, o);
    written += size;
    fwrite(zeros, 1, (4 - written % 4) % 4, o); written += (4 - written % 4) % 4;
}

static int cmd_build(const char *dir, const char *out, const char *name, const char *version)
{
    EntList l = {0};
    if (!valid_ident(name) || !valid_ident(version))
        return fail("invalid NAME or VERSION", NULL);
    collect(dir, "", &l);
    qsort(l.v, l.n, sizeof(Ent), ent_cmp);

    size_t cap = 256 + l.n * 1200, used = 0;
    char *info = malloc(cap);
    used += (size_t)snprintf(info + used, cap - used, "NAME=%s\nVERSION=%s\n", name, version);
    for (size_t i = 0; i < l.n; i++) {
        if ((l.v[i].mode & 0170000) != 0100000) continue;
        char hex[65];
        sha256_hex(l.v[i].data, l.v[i].size, hex);
        used += (size_t)snprintf(info + used, cap - used, "FILE=%s %s\n", hex, l.v[i].path);
    }

    FILE *o = fopen(out, "wb");
    if (!o) return fail(strerror(errno), out);
    put_entry(o, ".PKGINFO", 0100644, (unsigned char *)info, used, 1);
    for (size_t i = 0; i < l.n; i++)
        put_entry(o, l.v[i].path, l.v[i].mode, l.v[i].data, l.v[i].size, i + 2);
    put_entry(o, "TRAILER!!!", 0, NULL, 0, 0);
    if (fclose(o) != 0) return fail("write failed", out);
    printf("built %s: %s %s (%zu entries)\n", out, name, version, l.n);
    return 0;
}

int main(int argc, char **argv)
{
    int a = 1;

    if (a + 1 < argc && strcmp(argv[a], "-r") == 0) {
        root = argv[a + 1];
        a += 2;
    }
    if (a >= argc) goto usage;
    const char *c = argv[a];
    int rest = argc - a - 1;
    int rc;

    if (!strcmp(c, "build") && rest == 4)         rc = cmd_build(argv[a+1], argv[a+2], argv[a+3], argv[a+4]);
    else if (!strcmp(c, "install") && rest == 1)  rc = cmd_install(argv[a+1]);
    else if (!strcmp(c, "remove") && rest == 1)   rc = cmd_remove(argv[a+1]);
    else if (!strcmp(c, "list") && rest == 0)     rc = cmd_list();
    else if (!strcmp(c, "files") && rest == 1)    rc = cmd_files(argv[a+1]);
    else if (!strcmp(c, "verify") && rest <= 1)   rc = cmd_verify(rest ? argv[a+1] : NULL);
    else goto usage;
    return rc < 0 ? 1 : 0;

usage:
    fprintf(stderr, "Usage: aether-pkg [-r ROOT] build DIR OUT NAME VERSION | install FILE |\n"
                    "       remove NAME | list | files NAME | verify [NAME]\n");
    return 2;
}
