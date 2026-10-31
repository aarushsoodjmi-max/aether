#define _GNU_SOURCE
#include "fsops.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

#define NEW_SUFFIX ".aether-new"
#define OLD_SUFFIX ".aether-old"

struct Txn {
    char *root; int rootfd; int jfd;
    char **staged; size_t nstaged, applied;       /* staged-but-not-yet-placed relative paths, in order */
    long counter; long crash_at;
};

/* ---------- path walking without symlink traversal ---------- */
/* Opens the parent directory of rel without ever following a symlink. When create is set, missing
 * directories are created; cb journals each creation BEFORE it happens (write-ahead). */
typedef int (*mk_fn)(void *ctx, const char *relprefix);
static int walk_parent(int rootfd, const char *rel, int create, mk_fn cb, void *ctx, int *pfd, char **base)
{
    const char *last = strrchr(rel, '/');
    *base = xstrdup(last ? last + 1 : rel);
    int cur = dup(rootfd);
    if (cur < 0) { free(*base); return -1; }
    if (last) {
        size_t dirlen = (size_t)(last - rel), pos = 0;
        while (pos < dirlen) {
            const char *s = rel + pos, *e = memchr(s, '/', dirlen - pos);
            size_t clen = e ? (size_t)(e - s) : dirlen - pos;
            char comp[256];
            if (clen == 0 || clen >= sizeof comp) { close(cur); free(*base); errno = ENAMETOOLONG; return -1; }
            memcpy(comp, s, clen); comp[clen] = 0;
            int nfd = openat(cur, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (nfd < 0 && errno == ENOENT && create) {
                char *prefix = xmalloc(pos + clen + 1); memcpy(prefix, rel, pos + clen); prefix[pos + clen] = 0;
                int jr = cb ? cb(ctx, prefix) : 0;
                free(prefix);
                if (jr != 0) { close(cur); free(*base); errno = EIO; return -1; }
                if (mkdirat(cur, comp, 0755) != 0 && errno != EEXIST) { int se = errno; close(cur); free(*base); errno = se; return -1; }
                nfd = openat(cur, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            }
            if (nfd < 0) { int se = errno; close(cur); free(*base); errno = se; return -1; }
            close(cur); cur = nfd; pos += clen + 1;
        }
    }
    *pfd = cur; return 0;
}

int fs_lstat(const char *root, const char *rel, struct stat *st)
{
    int rfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (rfd < 0) return -1;
    int pfd; char *base; if (walk_parent(rfd, rel, 0, NULL, NULL, &pfd, &base) != 0) { int se = errno; close(rfd); errno = se; return -1; }
    int rc = fstatat(pfd, base, st, AT_SYMLINK_NOFOLLOW); int se = errno; close(pfd); close(rfd); free(base); errno = se; return rc;
}
int fs_hash(const char *root, const char *rel, uint8_t sha[32])
{
    int rfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (rfd < 0) return -1;
    int pfd; char *base; if (walk_parent(rfd, rel, 0, NULL, NULL, &pfd, &base) != 0) { close(rfd); return -1; }
    int fd = openat(pfd, base, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); close(pfd); close(rfd); free(base); if (fd < 0) return -1;
    Sha s; sha_init(&s); uint8_t buf[65536];
    for (;;) { ssize_t r = read(fd, buf, sizeof buf); if (r < 0 && errno == EINTR) continue; if (r < 0) { close(fd); return -1; } if (r == 0) break; sha_update(&s, buf, (size_t)r); }
    close(fd); sha_final(&s, sha); return 0;
}
int fs_readlink(const char *root, const char *rel, char *buf, size_t n)
{
    int rfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (rfd < 0) return -1;
    int pfd; char *base; if (walk_parent(rfd, rel, 0, NULL, NULL, &pfd, &base) != 0) { close(rfd); return -1; }
    ssize_t r = readlinkat(pfd, base, buf, n - 1); close(pfd); close(rfd); free(base); if (r < 0) return -1; buf[r] = 0; return 0;
}

/* ---------- db dirs, lock ---------- */
static int mkdir_p_root(int rootfd, const char *rel, char *err)
{
    char *c = xstrdup(rel); int cur = dup(rootfd);
    for (char *p = c; cur >= 0; ) {
        char *e = strchr(p, '/'); if (e) *e = 0;
        if (*p && mkdirat(cur, p, 0755) != 0 && errno != EEXIST) { set_err(err, "mkdir %s: %s", rel, strerror(errno)); close(cur); free(c); return -1; }
        int n = openat(cur, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (n < 0) { set_err(err, "open %s: %s", rel, strerror(errno)); close(cur); free(c); return -1; }
        close(cur); cur = n; if (!e) break; p = e + 1;
    }
    if (cur >= 0) {
        close(cur);
    }
    free(c);
    return 0;
}
int pm_init_dirs(const char *root, char *err)
{
    int rfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (rfd < 0) { set_err(err, "%s: %s", root, strerror(errno)); return -1; }
    static const char *const d[] = { PM_DB_DIR "/db", PM_DB_DIR "/txn", PM_DB_DIR "/state", "var/cache/aether/pm" };
    for (size_t i = 0; i < sizeof d / sizeof *d; i++) if (mkdir_p_root(rfd, d[i], err) != 0) { close(rfd); return -1; }
    close(rfd); return 0;
}
int pm_lock(const char *root, int *fd_out, char *err)
{
    char *p = xsprintf("%s/" PM_DB_DIR "/lock", root);
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { set_err(err, "%s: %s", p, strerror(errno)); free(p); return -1; }
    free(p);
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { set_err(err, "another aether-pm is running (database locked)"); close(fd); return -1; }
    *fd_out = fd; return 0;
}
void pm_unlock(int fd) { if (fd >= 0) close(fd); }

/* ---------- journal ---------- */
static int jappend(Txn *t, const char *op, const char *path, int mode)
{
    char *line = mode >= 0 ? xsprintf("%s %s %04o\n", op, path, (unsigned)mode) : xsprintf("%s %s\n", op, path);
    size_t n = strlen(line); ssize_t w = write(t->jfd, line, n); free(line);
    if (w != (ssize_t)n || fsync(t->jfd) != 0) return -1;
    t->counter++;
    if (t->crash_at > 0 && t->counter >= t->crash_at) _exit(99);
    return 0;
}
static int journal_mkdir_cb(void *ctx, const char *prefix) { return jappend(ctx, "mkdir", prefix, -1); }

static int txn_dirfd(const char *root) { char *p = xsprintf("%s/" PM_DB_DIR "/txn", root); int fd = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC); free(p); return fd; }

Txn *txn_begin(const char *root, char *err)
{
    Txn *t = calloc(1, sizeof *t); if (!t) exit(70);
    t->root = xstrdup(root); t->jfd = -1;
    t->rootfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (t->rootfd < 0) { set_err(err, "%s: %s", root, strerror(errno)); goto fail; }
    int dfd = txn_dirfd(root); if (dfd < 0) { set_err(err, "package database not initialised: %s", strerror(errno)); goto fail; }
    t->jfd = openat(dfd, "journal", O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
    if (t->jfd < 0) { set_err(err, "cannot start transaction: %s (an interrupted transaction? run 'aether-pm recover')", strerror(errno)); close(dfd); goto fail; }
    fsync(dfd); close(dfd);
    const char *c = getenv("AETHER_PM_CRASH_AT"); t->crash_at = c ? atol(c) : 0;
    return t;
fail:
    if (t->rootfd >= 0) close(t->rootfd);
    free(t->root); free(t); return NULL;
}
static void txn_free(Txn *t) { if (t->jfd >= 0) close(t->jfd); if (t->rootfd >= 0) close(t->rootfd); for (size_t i = 0; i < t->nstaged; i++) free(t->staged[i]); free(t->staged); free(t->root); free(t); }

static char *with_suffix(const char *base, const char *suf) { return xsprintf("%s%s", base, suf); }

int txn_stage_entry(Txn *t, const PkgFile *p, const Entry *e, char *err)
{
    int pfd; char *base;
    if (walk_parent(t->rootfd, e->path, 1, journal_mkdir_cb, t, &pfd, &base) != 0) { set_err(err, "%s: cannot create parent directories (%s)", e->path, errno == ELOOP || errno == ENOTDIR ? "a path component is a symlink or not a directory" : strerror(errno)); return -1; }
    int rc = -1; char *tmpn = with_suffix(base, NEW_SUFFIX);
    if (e->type == 'd') {
        struct stat st;
        if (fstatat(pfd, base, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISDIR(st.st_mode)) { set_err(err, "%s exists and is not a directory", e->path); goto out; }
            rc = 0; goto out;
        }
        if (jappend(t, "mkdir", e->path, -1) != 0) { set_err(err, "journal write failed"); goto out; }
        if (mkdirat(pfd, base, e->mode) != 0) { set_err(err, "mkdir %s: %s", e->path, strerror(errno)); goto out; }
        if (fchmodat(pfd, base, e->mode, 0) != 0 && errno != ENOTSUP) { /* mode may be masked by umask; best effort */ }
        rc = 0; goto out;
    }
    if (jappend(t, "stage", e->path, -1) != 0) { set_err(err, "journal write failed"); goto out; }
    unlinkat(pfd, tmpn, 0);
    if (e->type == 'l') {
        if (symlinkat(e->target, pfd, tmpn) != 0) { set_err(err, "symlink %s: %s", e->path, strerror(errno)); goto out; }
    } else {
        int fd = openat(pfd, tmpn, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) { set_err(err, "create %s: %s", e->path, strerror(errno)); goto out; }
        Sha s; sha_init(&s); uint8_t buf[65536]; uint64_t left = e->size, off = p->payload_off + e->off;
        while (left) {
            size_t n = left > sizeof buf ? sizeof buf : (size_t)left; ssize_t r = pread(p->fd, buf, n, (off_t)off);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { set_err(err, "%s: package read error", e->path); close(fd); goto out; }
            sha_update(&s, buf, (size_t)r);
            for (ssize_t o = 0; o < r;) { ssize_t w = write(fd, buf + o, (size_t)(r - o)); if (w < 0 && errno == EINTR) continue; if (w <= 0) { set_err(err, "%s: write error: %s", e->path, strerror(errno)); close(fd); goto out; } o += w; }
            off += (uint64_t)r; left -= (uint64_t)r;
        }
        uint8_t d[32]; sha_final(&s, d);
        if (sodium_memcmp(d, e->sha, 32) != 0) { set_err(err, "%s: checksum mismatch (corrupt or tampered package)", e->path); close(fd); goto out; }
        if (fchmod(fd, e->mode) != 0 || fsync(fd) != 0) { set_err(err, "%s: %s", e->path, strerror(errno)); close(fd); goto out; }
        close(fd);
    }
    t->staged = xrealloc(t->staged, (t->nstaged + 1) * sizeof *t->staged); t->staged[t->nstaged++] = xstrdup(e->path);
    rc = 0;
out:
    close(pfd); free(base); free(tmpn); return rc;
}

int txn_put_bytes(Txn *t, const char *rel, const void *buf, size_t len, unsigned mode, char *err)
{
    int pfd; char *base;
    if (walk_parent(t->rootfd, rel, 1, journal_mkdir_cb, t, &pfd, &base) != 0) { set_err(err, "%s: cannot create parent directories", rel); return -1; }
    char *tmpn = with_suffix(base, NEW_SUFFIX); int rc = -1;
    if (jappend(t, "stage", rel, -1) != 0) { set_err(err, "journal write failed"); goto out; }
    unlinkat(pfd, tmpn, 0);
    int fd = openat(pfd, tmpn, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600); if (fd < 0) { set_err(err, "create %s: %s", rel, strerror(errno)); goto out; }
    const char *b = buf; size_t o = 0;
    while (o < len) { ssize_t w = write(fd, b + o, len - o); if (w < 0 && errno == EINTR) continue; if (w <= 0) { set_err(err, "%s: write error", rel); close(fd); goto out; } o += (size_t)w; }
    if (fchmod(fd, mode) != 0 || fsync(fd) != 0) { set_err(err, "%s: %s", rel, strerror(errno)); close(fd); goto out; }
    close(fd);
    t->staged = xrealloc(t->staged, (t->nstaged + 1) * sizeof *t->staged); t->staged[t->nstaged++] = xstrdup(rel); rc = 0;
out:
    close(pfd); free(base); free(tmpn); return rc;
}

static int backup_then_remove(Txn *t, int pfd, const char *rel, const char *base, char *err)
{
    struct stat st; if (fstatat(pfd, base, &st, AT_SYMLINK_NOFOLLOW) != 0) return 0;
    if (S_ISDIR(st.st_mode)) { set_err(err, "%s is a directory where a file was expected", rel); return -1; }
    char *old = with_suffix(base, OLD_SUFFIX);
    if (jappend(t, "backup", rel, -1) != 0) { set_err(err, "journal write failed"); free(old); return -1; }
    unlinkat(pfd, old, 0);
    int rc = renameat(pfd, base, pfd, old); if (rc != 0) set_err(err, "backup %s: %s", rel, strerror(errno));
    free(old); return rc;
}
int txn_remove_path(Txn *t, const char *rel, char *err)
{
    int pfd; char *base;
    if (walk_parent(t->rootfd, rel, 0, NULL, NULL, &pfd, &base) != 0) { if (errno == ENOENT) return 0; set_err(err, "%s: %s", rel, strerror(errno)); return -1; }
    int rc = backup_then_remove(t, pfd, rel, base, err); close(pfd); free(base); return rc;
}
int txn_rmdir_if_empty(Txn *t, const char *rel, char *err)
{
    /* Directories cannot be removed inside the transaction: they still hold NAME.aether-old backups.
     * Record the request; it runs after the commit point (and is replayed by roll-forward recovery).
     * rmdir(2) only succeeds on an empty directory, so shared or non-empty directories are kept. */
    if (jappend(t, "rmlater", rel, -1) != 0) { set_err(err, "journal write failed"); return -1; }
    return 0;
}

int txn_apply(Txn *t, char *err)
{
    for (; t->applied < t->nstaged; t->applied++) {
        const char *rel = t->staged[t->applied]; int pfd; char *base;
        if (walk_parent(t->rootfd, rel, 0, NULL, NULL, &pfd, &base) != 0) { set_err(err, "%s: %s", rel, strerror(errno)); return -1; }
        char *tmpn = with_suffix(base, NEW_SUFFIX); int rc = backup_then_remove(t, pfd, rel, base, err);
        if (rc == 0 && jappend(t, "placed", rel, -1) != 0) { set_err(err, "journal write failed"); rc = -1; }
        if (rc == 0 && renameat(pfd, tmpn, pfd, base) != 0) { set_err(err, "install %s: %s", rel, strerror(errno)); rc = -1; }
        close(pfd); free(base); free(tmpn); if (rc) return -1;
    }
    return 0;
}

/* ---------- rollback / cleanup driven by the journal ---------- */
typedef struct { char op[8]; char *path; int mode; } JRec;
static JRec *jread(int dfd, size_t *n)
{
    *n = 0; int fd = openat(dfd, "journal", O_RDONLY | O_CLOEXEC); if (fd < 0) return NULL;
    struct stat st; if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    char *buf = xmalloc((size_t)st.st_size + 1); size_t got = 0;
    while (got < (size_t)st.st_size) { ssize_t r = read(fd, buf + got, (size_t)st.st_size - got); if (r <= 0) break; got += (size_t)r; }
    close(fd); buf[got] = 0;
    JRec *recs = NULL; char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n'); if (!nl) break;            /* torn final line: ignore */
        *nl = 0; JRec r; memset(&r, 0, sizeof r);
        char *sp = strchr(p, ' '); if (sp && (size_t)(sp - p) < sizeof r.op) {
            memcpy(r.op, p, (size_t)(sp - p)); char *path = sp + 1; r.mode = -1;
            if (!strcmp(r.op, "rmdir")) { char *ms = strrchr(path, ' '); if (ms) { *ms = 0; r.mode = (int)strtol(ms + 1, NULL, 8); } }
            r.path = xstrdup(path); recs = xrealloc(recs, (*n + 1) * sizeof *recs); recs[(*n)++] = r;
        }
        p = nl + 1;
    }
    free(buf); return recs;
}
static void jfree(JRec *r, size_t n) { for (size_t i = 0; i < n; i++) free(r[i].path); free(r); }

static void rollback_records(int rootfd, const JRec *r, size_t n)
{
    for (size_t i = n; i-- > 0; ) {
        const char *op = r[i].op, *rel = r[i].path; int pfd; char *base;
        if (!strcmp(op, "rmlater")) continue;                                        /* only ever acts after COMMIT */
        if (!strcmp(op, "mkdir") || !strcmp(op, "rmdir")) {
            if (!strcmp(op, "mkdir")) {
                if (walk_parent(rootfd, rel, 0, NULL, NULL, &pfd, &base) == 0) { unlinkat(pfd, base, AT_REMOVEDIR); close(pfd); free(base); }
            } else if (walk_parent(rootfd, rel, 0, NULL, NULL, &pfd, &base) == 0) { mkdirat(pfd, base, (mode_t)(r[i].mode >= 0 ? r[i].mode : 0755)); close(pfd); free(base); }
            continue;
        }
        if (walk_parent(rootfd, rel, 0, NULL, NULL, &pfd, &base) != 0) continue;
        char *nw = with_suffix(base, NEW_SUFFIX), *old = with_suffix(base, OLD_SUFFIX); struct stat st;
        if (!strcmp(op, "stage")) unlinkat(pfd, nw, 0);
        else if (!strcmp(op, "placed")) { if (fstatat(pfd, nw, &st, AT_SYMLINK_NOFOLLOW) != 0) unlinkat(pfd, base, 0); /* new file still staged => rename never happened: leave dest alone */ }
        else if (!strcmp(op, "backup")) { if (fstatat(pfd, old, &st, AT_SYMLINK_NOFOLLOW) == 0) { unlinkat(pfd, base, 0); renameat(pfd, old, pfd, base); } }
        close(pfd); free(base); free(nw); free(old);
    }
}
static void cleanup_backups(int rootfd, const JRec *r, size_t n)
{
    for (size_t i = 0; i < n; i++) if (!strcmp(r[i].op, "backup")) {
        int pfd; char *base; if (walk_parent(rootfd, r[i].path, 0, NULL, NULL, &pfd, &base) != 0) continue;
        char *old = with_suffix(base, OLD_SUFFIX); unlinkat(pfd, old, 0); close(pfd); free(base); free(old);
    }
    for (size_t i = 0; i < n; i++) if (!strcmp(r[i].op, "rmlater")) {          /* post-commit; harmless if repeated */
        int pfd; char *base; if (walk_parent(rootfd, r[i].path, 0, NULL, NULL, &pfd, &base) != 0) continue;
        unlinkat(pfd, base, AT_REMOVEDIR); close(pfd); free(base);
    }
}

int txn_recover(const char *root, char *err)
{
    int rfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (rfd < 0) { set_err(err, "%s: %s", root, strerror(errno)); return -1; }
    int dfd = txn_dirfd(root); if (dfd < 0) { close(rfd); return 0; }          /* no database yet */
    struct stat st; int has_j = fstatat(dfd, "journal", &st, 0) == 0, has_c = fstatat(dfd, "COMMIT", &st, 0) == 0, res = 0;
    if (has_j) {
        size_t n; JRec *r = jread(dfd, &n);
        if (has_c) { cleanup_backups(rfd, r, n); res = 2; } else { rollback_records(rfd, r, n); res = 1; }
        jfree(r, n); syncfs(rfd); unlinkat(dfd, "journal", 0);
    }
    if (has_c) unlinkat(dfd, "COMMIT", 0);
    fsync(dfd); close(dfd); close(rfd); return res;
}

int txn_commit(Txn *t, char *err)
{
    if (t->applied != t->nstaged) { set_err(err, "internal error: unapplied staged files"); return -1; }
    int dfd = txn_dirfd(t->root); if (dfd < 0) { set_err(err, "txn dir: %s", strerror(errno)); return -1; }
    if (syncfs(t->rootfd) != 0) { set_err(err, "syncfs: %s", strerror(errno)); close(dfd); return -1; }
    int cfd = openat(dfd, "COMMIT", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (cfd < 0 || fsync(cfd) != 0 || fsync(dfd) != 0) { set_err(err, "commit marker: %s", strerror(errno)); if (cfd >= 0) close(cfd); close(dfd); return -1; }
    close(cfd);
    t->counter++; if (t->crash_at > 0 && t->counter >= t->crash_at) _exit(99);     /* crash AFTER commit point: must roll forward */
    close(t->jfd); t->jfd = -1;
    size_t n; JRec *r = jread(dfd, &n); cleanup_backups(t->rootfd, r, n); jfree(r, n);
    unlinkat(dfd, "journal", 0); unlinkat(dfd, "COMMIT", 0); fsync(dfd); close(dfd);
    txn_free(t); return 0;
}
void txn_abort(Txn *t)
{
    int dfd = txn_dirfd(t->root);
    if (t->jfd >= 0) { close(t->jfd); t->jfd = -1; }
    if (dfd >= 0) { size_t n; JRec *r = jread(dfd, &n); rollback_records(t->rootfd, r, n); jfree(r, n); unlinkat(dfd, "journal", 0); fsync(dfd); close(dfd); }
    txn_free(t);
}
