#define _GNU_SOURCE
#include "initlib.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PROC_SUPER_MAGIC
#define PROC_SUPER_MAGIC 0x9fa0
#endif
#ifndef SYSFS_MAGIC
#define SYSFS_MAGIC 0x62656572
#endif
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

/* devtmpfs reports TMPFS_MAGIC. */
const AInitMount ainit_boot_mounts[] = {
    { "devtmpfs", "/dev",  "devtmpfs", 0, NULL, TMPFS_MAGIC,     1 },
    { "proc",     "/proc", "proc",     MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL, PROC_SUPER_MAGIC, 1 },
    { "sysfs",    "/sys",  "sysfs",    MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL, SYSFS_MAGIC,      0 },
    { "tmpfs",    "/run",  "tmpfs",    MS_NOSUID | MS_NODEV, "mode=0755", TMPFS_MAGIC,           0 },
};
const int ainit_boot_mount_count =
    (int)(sizeof(ainit_boot_mounts) / sizeof(ainit_boot_mounts[0]));

int ainit_action_for_signal(int signo)
{
    switch (signo) {
    case SIGTERM:
    case SIGINT:
    case SIGUSR2:
        return AINIT_ACT_POWEROFF;
    case SIGUSR1:
        return AINIT_ACT_REBOOT;
    default:
        return AINIT_ACT_NONE;
    }
}

int ainit_restart_decide(int restart_enabled, int *count, int max,
                         long long uptime_ms, long long stable_ms)
{
    if (!count)
        return 0;
    if (uptime_ms >= stable_ms)
        *count = 0;
    if (restart_enabled && *count < max) {
        (*count)++;
        return 1;
    }
    return 0;
}

int ainit_mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];

    if (!path || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (char *c = tmp + 1; ; c++) {
        if (*c == '/' || *c == '\0') {
            char save = *c;

            *c = '\0';
            if (mkdir(tmp, mode) != 0) {
                struct stat st;

                if (errno != EEXIST)
                    return -1;
                if (stat(tmp, &st) != 0)
                    return -1;
                if (!S_ISDIR(st.st_mode)) {
                    errno = ENOTDIR;
                    return -1;
                }
            }
            *c = save;
            if (!save)
                break;
        }
    }
    return 0;
}

int ainit_is_mountpoint(const char *path)
{
    struct stat st, parent;
    char up[PATH_MAX];

    if (stat(path, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    if (snprintf(up, sizeof(up), "%s/..", path) >= (int)sizeof(up)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (stat(up, &parent) != 0)
        return -1;
    if (st.st_dev != parent.st_dev)
        return 1;
    return st.st_ino == parent.st_ino;      /* "/" is its own parent */
}

int ainit_fs_type_is(const char *path, long magic)
{
    struct statfs sf;

    if (statfs(path, &sf) != 0)
        return -1;
    return (long)sf.f_type == magic;
}

AInitMountResult ainit_mount(const char *source, const char *target,
                             const char *type, unsigned long flags,
                             const char *data, long magic, int *created,
                             int *err)
{
    struct stat st;
    int mp;

    if (created)
        *created = 0;
    if (err)
        *err = 0;
    if (!source || !target || !type || target[0] != '/') {
        if (err)
            *err = EINVAL;
        return AINIT_MOUNT_FAIL;
    }

    if (stat(target, &st) != 0) {
        if (errno != ENOENT || ainit_mkdir_p(target, 0755) != 0) {
            if (err)
                *err = errno;
            return AINIT_MOUNT_FAIL;
        }
        if (created)
            *created = 1;
    } else if (!S_ISDIR(st.st_mode)) {
        if (err)
            *err = ENOTDIR;
        return AINIT_MOUNT_FAIL;
    }

    mp = ainit_is_mountpoint(target);
    if (mp < 0) {
        if (err)
            *err = errno;
        return AINIT_MOUNT_FAIL;
    }
    if (mp == 1) {
        if (magic != 0 && ainit_fs_type_is(target, magic) != 1)
            return AINIT_MOUNT_MISMATCH;
        return AINIT_MOUNT_ALREADY;
    }

    if (mount(source, target, type, flags, data) != 0) {
        if (err)
            *err = errno;
        return AINIT_MOUNT_FAIL;
    }
    return AINIT_MOUNT_OK;
}

const char *ainit_mount_result_name(AInitMountResult r)
{
    switch (r) {
    case AINIT_MOUNT_OK:       return "mounted";
    case AINIT_MOUNT_ALREADY:  return "already mounted";
    case AINIT_MOUNT_MISMATCH: return "occupied by another filesystem";
    default:                   return "failed";
    }
}

static int fd_open(int fd)
{
    return fcntl(fd, F_GETFD) != -1;
}

static int attach_one(int fd, const char *path)
{
    int n = open(path, O_RDWR | O_NOCTTY);

    if (n < 0)
        return -1;
    if (n != fd) {
        if (dup2(n, fd) < 0) {
            close(n);
            return -1;
        }
        close(n);
    }
    return 0;
}

int ainit_console_attach(const char *console, const char *fallback)
{
    int used_fallback = 0, failed = 0;

    for (int fd = 0; fd <= 2; fd++) {
        if (fd_open(fd))
            continue;
        if (console && attach_one(fd, console) == 0)
            continue;
        if (fallback && attach_one(fd, fallback) == 0) {
            used_fallback = 1;
            continue;
        }
        failed = 1;
    }
    if (failed)
        return -1;
    return used_fallback;
}

int ainit_reap_children(AInitReapFn fn, void *ctx)
{
    int n = 0;

    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid > 0) {
            n++;
            if (fn)
                fn(pid, status, ctx);
            continue;
        }
        if (pid < 0 && errno == EINTR)
            continue;
        return n;                       /* 0: none ready, <0: no children */
    }
}
