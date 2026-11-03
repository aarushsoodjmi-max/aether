#ifndef AETHER_INITLIB_H
#define AETHER_INITLIB_H

/*
 * PID 1-independent helpers of Aether init. Nothing in here needs to run as
 * PID 1, so everything is unit-tested on the host (tests/test_initlib.c).
 */

#include <sys/types.h>

/* ---- signals -> action ---- */
enum { AINIT_ACT_NONE = 0, AINIT_ACT_POWEROFF, AINIT_ACT_REBOOT };

/* SIGTERM/SIGINT/SIGUSR2 -> power off, SIGUSR1 -> reboot, anything else none. */
int ainit_action_for_signal(int signo);

/* ---- restart policy ---- */

/*
 * Decide whether a service that just exited is restarted. The budget
 * (*count of max) is reset first when the service ran at least stable_ms.
 * Returns 1 and increments *count if a restart is granted, else 0. A service
 * that crashes immediately can therefore be restarted at most max times, no
 * matter how often it dies.
 */
int ainit_restart_decide(int restart_enabled, int *count, int max,
                         long long uptime_ms, long long stable_ms);

/* ---- directories and mounts ---- */

/* mkdir -p. Fails with ENOTDIR if a component exists and is not a directory. */
int ainit_mkdir_p(const char *path, mode_t mode);

/* 1 if path is a mount point (different device than its parent, or "/"), 0 if not, -1 on error. */
int ainit_is_mountpoint(const char *path);

/* 1 if statfs(path).f_type == magic, 0 if not, -1 on error. */
int ainit_fs_type_is(const char *path, long magic);

typedef enum {
    AINIT_MOUNT_OK = 0,        /* mounted now */
    AINIT_MOUNT_ALREADY,       /* a filesystem of the right type was already there; no mount done */
    AINIT_MOUNT_MISMATCH,      /* something else is mounted there; left alone */
    AINIT_MOUNT_FAIL           /* could not create the directory or mount; *err has errno */
} AInitMountResult;

/*
 * Create target if needed, then mount type on it unless it already is a mount
 * point. magic is the statfs f_type the filesystem reports (0 = do not check).
 * *created (optional) is set to 1 when the directory had to be created.
 */
AInitMountResult ainit_mount(const char *source, const char *target,
                             const char *type, unsigned long flags,
                             const char *data, long magic, int *created,
                             int *err);

const char *ainit_mount_result_name(AInitMountResult r);

/* The pseudo filesystems init sets up, in order. */
typedef struct {
    const char *source, *target, *type;
    unsigned long flags;
    const char *data;
    long magic;
    int critical;              /* boot cannot continue normally without it */
} AInitMount;

extern const AInitMount ainit_boot_mounts[];
extern const int ainit_boot_mount_count;

/* ---- console ---- */

/*
 * Make fds 0, 1 and 2 usable. Every one of them that is not open is opened on
 * console (O_RDWR|O_NOCTTY); fds that are already open are left alone, so
 * early kernel diagnostics keep going to the same place. If console cannot be
 * opened, fallback (e.g. /dev/null; may be NULL) is used so that later open()
 * calls can never land on fd 0-2.
 * Returns 0 console in use, 1 fallback in use, -1 nothing could be done.
 */
int ainit_console_attach(const char *console, const char *fallback);

/* ---- child reaping ---- */

typedef void (*AInitReapFn)(pid_t pid, int status, void *ctx);

/* Collect every terminated child without blocking. Returns how many. */
int ainit_reap_children(AInitReapFn fn, void *ctx);

#endif
