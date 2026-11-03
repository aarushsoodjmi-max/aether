/*
 * Unit tests for src/core/initlib.c: the PID 1-independent logic of Aether init.
 * Nothing here needs to be PID 1 or touches the host: files live in a mkdtemp
 * directory, anything that mounts runs in a private mount namespace (skipped
 * when that is not permitted), and anything that rearranges fds runs in a
 * forked child.
 */
#define _GNU_SOURCE
#include "../src/core/initlib.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond, msg) do {                                              \
    checks++;                                                              \
    if (cond) { printf("PASS: %s\n", msg); }                               \
    else { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
} while (0)

static char tmp[256];

static void path(char *out, size_t n, const char *rel)
{
    snprintf(out, n, "%s/%s", tmp, rel);
}

/* ---------------- signal -> action ---------------- */
static void test_signals(void)
{
    CHECK(ainit_action_for_signal(SIGTERM) == AINIT_ACT_POWEROFF, "SIGTERM -> poweroff");
    CHECK(ainit_action_for_signal(SIGINT) == AINIT_ACT_POWEROFF, "SIGINT -> poweroff");
    CHECK(ainit_action_for_signal(SIGUSR2) == AINIT_ACT_POWEROFF, "SIGUSR2 -> poweroff");
    CHECK(ainit_action_for_signal(SIGUSR1) == AINIT_ACT_REBOOT, "SIGUSR1 -> reboot");
    CHECK(ainit_action_for_signal(SIGCHLD) == AINIT_ACT_NONE, "SIGCHLD -> no action (reaping only)");
    CHECK(ainit_action_for_signal(SIGHUP) == AINIT_ACT_NONE, "SIGHUP ignored");
    CHECK(ainit_action_for_signal(0) == AINIT_ACT_NONE, "signal 0 ignored");
    CHECK(ainit_action_for_signal(-1) == AINIT_ACT_NONE, "negative signal ignored");
    CHECK(ainit_action_for_signal(100000) == AINIT_ACT_NONE, "huge signal number ignored");
}

/* ---------------- shell / service restart policy ---------------- */
static void test_restart_policy(void)
{
    int count = 0, granted = 0;

    /* A shell that dies instantly, forever: exactly max restarts, then stop. */
    for (int i = 0; i < 1000; i++)
        if (ainit_restart_decide(1, &count, 5, 10, 60000))
            granted++;
    CHECK(granted == 5, "rapid crash loop: exactly max_restarts restarts granted, then refused");
    CHECK(count == 5, "counter stops at the limit");

    count = 5;
    CHECK(ainit_restart_decide(1, &count, 5, 59999, 60000) == 0, "just under stable uptime: budget not reset");
    CHECK(ainit_restart_decide(1, &count, 5, 60000, 60000) == 1 && count == 1,
          "stable uptime resets the budget, then one restart is used");

    count = 0;
    CHECK(ainit_restart_decide(0, &count, 5, 10, 60000) == 0 && count == 0,
          "restart disabled: never restarted, counter untouched");
    count = 3;
    CHECK(ainit_restart_decide(0, &count, 5, 70000, 60000) == 0 && count == 0,
          "restart disabled: stable uptime still resets the counter");

    count = 0;
    CHECK(ainit_restart_decide(1, &count, 0, 10, 60000) == 0, "max_restarts = 0: no restart");
    CHECK(ainit_restart_decide(1, NULL, 5, 10, 60000) == 0, "NULL counter is refused, no crash");

    /* Alternating stable and crashing runs never exhaust the budget. */
    count = 0;
    int ok = 1;
    for (int i = 0; i < 50; i++) {
        ok &= ainit_restart_decide(1, &count, 1, 61000, 60000);
    }
    CHECK(ok == 1, "a service that is stable between crashes is restarted indefinitely");
}

/* ---------------- directories ---------------- */
static void test_mkdir(void)
{
    char a[300], b[300], f[300];
    struct stat st;

    path(a, sizeof(a), "x/y/z");
    CHECK(ainit_mkdir_p(a, 0755) == 0 && stat(a, &st) == 0 && S_ISDIR(st.st_mode),
          "mkdir_p creates nested directories");
    CHECK(ainit_mkdir_p(a, 0755) == 0, "mkdir_p is idempotent on existing directories");

    path(f, sizeof(f), "file");
    int fd = open(f, O_CREAT | O_WRONLY, 0600);
    if (fd >= 0) close(fd);
    CHECK(ainit_mkdir_p(f, 0755) == -1 && errno == ENOTDIR, "mkdir_p refuses a regular file (ENOTDIR)");
    path(b, sizeof(b), "file/sub");
    CHECK(ainit_mkdir_p(b, 0755) == -1, "mkdir_p refuses a path below a regular file");

    CHECK(ainit_mkdir_p("", 0755) == -1 && errno == EINVAL, "mkdir_p rejects empty path");
    CHECK(ainit_mkdir_p(NULL, 0755) == -1 && errno == EINVAL, "mkdir_p rejects NULL");

    char longp[5000];
    memset(longp, 'a', sizeof(longp) - 1);
    longp[0] = '/';
    longp[sizeof(longp) - 1] = '\0';
    CHECK(ainit_mkdir_p(longp, 0755) == -1 && errno == ENAMETOOLONG, "mkdir_p rejects over-long path");
}

/* ---------------- mount point detection and mount() ---------------- */
static void test_mountpoints(void)
{
    char d[300], f[300];

    CHECK(ainit_is_mountpoint("/") == 1, "/ is a mount point");
    CHECK(ainit_is_mountpoint("/proc") == 1, "/proc is a mount point (host)");
    CHECK(ainit_fs_type_is("/proc", ainit_boot_mounts[1].magic) == 1, "/proc reports the procfs magic");
    CHECK(ainit_fs_type_is("/proc", 0x1234) == 0, "wrong magic does not match");

    path(d, sizeof(d), "plain");
    mkdir(d, 0755);
    CHECK(ainit_is_mountpoint(d) == 0, "a plain directory is not a mount point");
    CHECK(ainit_is_mountpoint("/definitely/not/here") == -1 && errno == ENOENT, "missing path -> error");
    path(f, sizeof(f), "file");
    CHECK(ainit_is_mountpoint(f) == -1 && errno == ENOTDIR, "regular file -> ENOTDIR");

    /* Already-mounted detection needs no privilege: nothing is mounted. */
    int created = -1, err = -1;
    AInitMountResult r = ainit_mount("proc", "/proc", "proc", 0, NULL,
                                     ainit_boot_mounts[1].magic, &created, &err);
    CHECK(r == AINIT_MOUNT_ALREADY && created == 0 && err == 0,
          "mounting /proc again is detected as already mounted (no mount attempted)");
    r = ainit_mount("tmpfs", "/proc", "tmpfs", 0, NULL, 0x1234, &created, &err);
    CHECK(r == AINIT_MOUNT_MISMATCH, "a different filesystem at the target is reported, left untouched");

    /* Argument and path errors. */
    CHECK(ainit_mount(NULL, "/x", "tmpfs", 0, NULL, 0, NULL, &err) == AINIT_MOUNT_FAIL && err == EINVAL, "NULL source rejected");
    CHECK(ainit_mount("tmpfs", "relative", "tmpfs", 0, NULL, 0, NULL, &err) == AINIT_MOUNT_FAIL && err == EINVAL, "relative target rejected");
    CHECK(ainit_mount("tmpfs", f, "tmpfs", 0, NULL, 0, NULL, &err) == AINIT_MOUNT_FAIL && err == ENOTDIR,
          "target that is a regular file fails with ENOTDIR");
    char blocked[300];
    path(blocked, sizeof(blocked), "file/sub");
    CHECK(ainit_mount("tmpfs", blocked, "tmpfs", 0, NULL, 0, &created, &err) == AINIT_MOUNT_FAIL && created == 0,
          "mount point that cannot be created fails, created=0");
    CHECK(strcmp(ainit_mount_result_name(AINIT_MOUNT_ALREADY), "already mounted") == 0, "result names");

    /* Boot mount table sanity: order and criticality. */
    CHECK(ainit_boot_mount_count == 4, "boot mount table has /dev /proc /sys /run");
    CHECK(strcmp(ainit_boot_mounts[0].target, "/dev") == 0 && ainit_boot_mounts[0].critical, "/dev first and critical");
    CHECK(strcmp(ainit_boot_mounts[1].target, "/proc") == 0 && ainit_boot_mounts[1].critical, "/proc critical");
    CHECK(strcmp(ainit_boot_mounts[2].target, "/sys") == 0 && !ainit_boot_mounts[2].critical, "/sys not critical");
    CHECK(strcmp(ainit_boot_mounts[3].target, "/run") == 0 && !ainit_boot_mounts[3].critical, "/run not critical");
}

/* Real mount() in a private mount namespace; skipped if not permitted. */
static void test_real_mount(void)
{
    pid_t p = fork();

    if (p == 0) {
        char d[300];
        int created = 0, err = 0, bad = 0;

        if (unshare(CLONE_NEWNS) != 0 || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
            _exit(77);
        path(d, sizeof(d), "mnt/run");     /* does not exist yet */
        AInitMountResult r = ainit_mount("tmpfs", d, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755",
                                         ainit_boot_mounts[3].magic, &created, &err);
        bad |= !(r == AINIT_MOUNT_OK && created == 1);
        bad |= !(ainit_is_mountpoint(d) == 1);
        r = ainit_mount("tmpfs", d, "tmpfs", 0, NULL, ainit_boot_mounts[3].magic, &created, &err);
        bad |= !(r == AINIT_MOUNT_ALREADY && created == 0);
        r = ainit_mount("tmpfs", d, "tmpfs", 0, NULL, 0x1234, &created, &err);
        bad |= !(r == AINIT_MOUNT_MISMATCH);
        r = ainit_mount("nosuchfs", d, "nosuchfs", 0, NULL, 0, &created, &err);
        bad |= !(r == AINIT_MOUNT_ALREADY);   /* occupied: no mount() attempted */
        char d2[300];
        path(d2, sizeof(d2), "mnt/bad");
        r = ainit_mount("nosuchfs", d2, "nosuchfs", 0, NULL, 0, &created, &err);
        bad |= !(r == AINIT_MOUNT_FAIL && err != 0);
        _exit(bad ? 1 : 0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 77) {
        printf("SKIP: real mount tests (no permission for a mount namespace)\n");
        return;
    }
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "real mount in private namespace: OK, created, already, mismatch, failure");
}

/* ---------------- console setup ---------------- */
static int run_in_child(int (*fn)(void))
{
    pid_t p = fork();

    if (p == 0)
        _exit(fn());
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100;
}

static char con_path[300];

static int child_console_ok(void)
{
    close(0); close(1); close(2);
    int r = ainit_console_attach(con_path, "/dev/null");
    if (r != 0) return 1;
    if (write(1, "x", 1) != 1 || write(2, "y", 1) != 1) return 2;
    char c;
    if (read(0, &c, 1) < 0) return 3;
    return 0;
}

static int child_console_missing(void)
{
    close(0); close(1); close(2);
    int r = ainit_console_attach("/no/such/console", "/dev/null");
    if (r != 1) return 1;
    if (write(2, "x", 1) != 1) return 2;      /* fds are valid (/dev/null) */
    int fd = open("/dev/null", O_RDONLY);
    if (fd <= 2) return 3;                    /* later opens never land on 0-2 */
    return 0;
}

static int child_console_none(void)
{
    close(0); close(1); close(2);
    return ainit_console_attach("/no/such/console", NULL) == -1 ? 0 : 1;
}

static int child_console_preserve(void)
{
    /* fd 1 is already open (pipe): must be left alone. */
    int pfd[2];
    close(0); close(1); close(2);
    if (pipe(pfd) != 0) return 1;             /* fds 0 and 1 */
    int r = ainit_console_attach(con_path, "/dev/null");
    if (r != 0) return 2;
    if (write(1, "z", 1) != 1) return 3;
    char c = 0;
    if (read(0, &c, 1) != 1 || c != 'z') return 4;   /* fd 0 is still the pipe */
    return 0;
}

static void test_console(void)
{
    path(con_path, sizeof(con_path), "console");
    int fd = open(con_path, O_CREAT | O_RDWR, 0600);
    if (fd >= 0) { if (write(fd, "\n", 1) < 0) {} close(fd); }

    CHECK(run_in_child(child_console_ok) == 0, "console: fds 0-2 bound to the console device");
    CHECK(run_in_child(child_console_missing) == 0, "console missing: falls back to /dev/null, fds 0-2 stay reserved");
    CHECK(run_in_child(child_console_none) == 0, "console and fallback missing: reported as failure");
    CHECK(run_in_child(child_console_preserve) == 0, "already-open fds are left alone");
}

/* ---------------- child reaping ---------------- */
struct reap_rec { pid_t pid[16]; int status[16]; int n; };

static void reap_cb(pid_t pid, int status, void *ctx)
{
    struct reap_rec *r = ctx;
    if (r->n < 16) { r->pid[r->n] = pid; r->status[r->n] = status; r->n++; }
}

static void test_reaping(void)
{
    struct reap_rec rec = { .n = 0 };
    pid_t kids[5];

    CHECK(ainit_reap_children(reap_cb, &rec) == 0 && rec.n == 0, "no children: nothing reaped, no error");

    for (int i = 0; i < 5; i++) {
        kids[i] = fork();
        if (kids[i] == 0) {
            if (i == 4) { raise(SIGKILL); }
            _exit(i);
        }
    }
    usleep(300 * 1000);                       /* all five have terminated: zombies */
    int n = ainit_reap_children(reap_cb, &rec);
    CHECK(n == 5 && rec.n == 5, "five zombies reaped in one pass");

    int found_all = 1, status_ok = 1;
    for (int i = 0; i < 5; i++) {
        int seen = -1;
        for (int j = 0; j < rec.n; j++)
            if (rec.pid[j] == kids[i]) seen = j;
        if (seen < 0) { found_all = 0; continue; }
        if (i < 4) status_ok &= WIFEXITED(rec.status[seen]) && WEXITSTATUS(rec.status[seen]) == i;
        else status_ok &= WIFSIGNALED(rec.status[seen]) && WTERMSIG(rec.status[seen]) == SIGKILL;
    }
    CHECK(found_all, "every child pid was reported");
    CHECK(status_ok, "exit codes and fatal signal are passed through correctly");
    CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD, "no zombie is left behind");

    /* A running child is not reaped and not waited for. */
    pid_t slow = fork();
    if (slow == 0) { sleep(5); _exit(0); }
    struct reap_rec r2 = { .n = 0 };
    CHECK(ainit_reap_children(reap_cb, &r2) == 0 && r2.n == 0, "running child is left alone (non-blocking)");
    kill(slow, SIGKILL);
    waitpid(slow, NULL, 0);

    /* NULL callback just discards. */
    pid_t q = fork();
    if (q == 0) _exit(0);
    usleep(200 * 1000);
    CHECK(ainit_reap_children(NULL, NULL) == 1, "NULL callback still reaps");
}

/* PID 1 also inherits orphans. A subreaper in a child process stands in for it. */
static int child_orphans(void)
{
    struct reap_rec rec = { .n = 0 };

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
        return 77;
    pid_t mid = fork();
    if (mid == 0) {
        if (fork() == 0) { usleep(200 * 1000); _exit(7); }
        _exit(0);                       /* grandchild is orphaned -> re-parented to us */
    }
    waitpid(mid, NULL, 0);
    usleep(500 * 1000);                 /* grandchild has exited: zombie owned by us */
    int n = ainit_reap_children(reap_cb, &rec);
    if (n != 1 || rec.n != 1) return 1;
    if (!WIFEXITED(rec.status[0]) || WEXITSTATUS(rec.status[0]) != 7) return 2;
    if (waitpid(-1, NULL, WNOHANG) != -1 || errno != ECHILD) return 3;
    return 0;
}

static void test_orphans(void)
{
    int rc = run_in_child(child_orphans);

    if (rc == 77) {
        printf("SKIP: orphan reaping (PR_SET_CHILD_SUBREAPER unavailable)\n");
        return;
    }
    CHECK(rc == 0, "orphaned grandchild re-parented to init is reaped with its exit status");
}

int main(void)
{
    snprintf(tmp, sizeof(tmp), "/tmp/aether-initlib-XXXXXX");
    if (!mkdtemp(tmp)) { perror("mkdtemp"); return 1; }

    test_signals();
    test_restart_policy();
    test_mkdir();
    test_mountpoints();
    test_real_mount();
    test_console();
    test_reaping();
    test_orphans();

    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf -- '%s'", tmp);
    if (system(cmd) != 0) {}

    printf("%s: %d checks, %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", checks, failures);
    return failures ? 1 : 0;
}
