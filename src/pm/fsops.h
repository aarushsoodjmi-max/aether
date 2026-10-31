/* Filesystem operations and crash-safe transactions. Knows nothing about dependencies or repositories.
 *
 * Safety properties (see docs/aether-package-manager.md):
 *  - Every path is resolved component-by-component with openat(O_NOFOLLOW): a package can never write
 *    through a symlink (so it cannot escape the root or redirect into another directory).
 *  - New content is staged next to its destination as NAME.aether-new, hash-checked, fsync'd.
 *  - Replaced/removed objects are renamed to NAME.aether-old (same directory => atomic, same filesystem).
 *  - A write-ahead journal (txn/journal) records intent before each step. Until txn/COMMIT exists,
 *    recovery rolls everything back; once COMMIT exists, recovery rolls forward (cleanup only).
 */
#ifndef AETHER_PM_FSOPS_H
#define AETHER_PM_FSOPS_H
#include <sys/stat.h>
#include "pkgfile.h"

#define PM_DB_DIR "var/lib/aether/pm"
typedef struct Txn Txn;

int pm_init_dirs(const char *root, char *err);                 /* creates var/lib/aether/pm/{db,txn,state}, var/cache/aether/pm */
int pm_lock(const char *root, int *fd_out, char *err);         /* exclusive flock on var/lib/aether/pm/lock */
void pm_unlock(int fd);

/* 0 = nothing pending, 1 = rolled back an unfinished transaction, 2 = finished (rolled forward) a committed one, -1 = error. */
int txn_recover(const char *root, char *err);
Txn *txn_begin(const char *root, char *err);
int txn_stage_entry(Txn *t, const PkgFile *p, const Entry *e, char *err);   /* dirs are created now; files/links are staged */
int txn_put_bytes(Txn *t, const char *rel, const void *buf, size_t len, unsigned mode, char *err);
int txn_remove_path(Txn *t, const char *rel, char *err);       /* file or symlink; missing is fine */
int txn_rmdir_if_empty(Txn *t, const char *rel, char *err);    /* silently keeps non-empty directories */
int txn_apply(Txn *t, char *err);                              /* backup + atomically place everything staged so far */
int txn_commit(Txn *t, char *err);                             /* sync, write COMMIT marker, clean up backups; frees t */
void txn_abort(Txn *t);                                        /* roll back now; frees t */

/* Read-only helpers (never follow symlinks in parent components). */
int fs_lstat(const char *root, const char *rel, struct stat *st);          /* 0, or -1 with errno (ENOENT, ELOOP, ENOTDIR...) */
int fs_hash(const char *root, const char *rel, uint8_t sha[32]);           /* hashes a regular file; -1 on error */
int fs_readlink(const char *root, const char *rel, char *buf, size_t n);

/* Test hook: AETHER_PM_CRASH_AT=N makes the process _exit(99) right after the Nth journal record. */
#endif
