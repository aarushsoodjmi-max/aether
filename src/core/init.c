/*
 * Aether init (PID 1).
 *
 * Mounts the pseudo filesystems, loads the core configuration in /etc/aether
 * (see config.h; any problem falls back to built-in defaults and never stops
 * the boot), loads and orders the service definitions in
 * /etc/aether/services, supervises the services (restart policy with delay,
 * restart budget, recovery shell) and performs an orderly shutdown.
 *
 * Runtime control: root-only UNIX socket /run/aether.sock, one text request
 * per connection ("start|stop|restart|status NAME", "list"); see aetherctl.
 *
 * Control signals (sent by aetherctl or the kernel):
 *   SIGTERM, SIGINT, SIGUSR2  power off
 *   SIGUSR1                   reboot
 *
 * Boot environment (initlib.c): /dev, /proc, /sys and /run are mounted only if
 * not already mounted (missing mount points are created); stdio is bound to
 * /dev/console. Failure of /dev or /proc is critical: init then runs only the
 * recovery shell. PID 1 never exits on an initialisation error; if signalfd()
 * is unavailable it falls back to emergency_supervise().
 *
 * Build with -DAETHER_TEST to make mounts a no-op (unless also
 * -DAETHER_TEST_REAL_MOUNTS; with -DAETHER_TEST_DEV_TYPE='"tmpfs"' /dev
 * becomes a tmpfs so a test never exposes host devices), leave stdio alone and replace the reboot(2)
 * call by a message plus _exit(); used only by the integration tests.
 * -DAETHER_FORCE_EMERGENCY simulates a signalfd() failure for tests.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include "service.h"
#include "config.h"
#include "initlib.h"

#ifndef AETHER_SERVICE_DIR
#define AETHER_SERVICE_DIR "/etc/aether/services"
#endif
#ifndef AETHER_GRACE_MS
#define AETHER_GRACE_MS 5000       /* SIGTERM -> SIGKILL grace per service */
#endif
#ifndef AETHER_CTL_SOCK
#define AETHER_CTL_SOCK "/run/aether.sock"
#endif
#define RESTART_DELAY_MS 1000      /* pause between a crash and its restart */
#define STABLE_UPTIME_MS 60000     /* uptime after which the budget resets */
#define POLL_MS 1000               /* housekeeping wake-up interval */

#ifndef AETHER_RECOVERY_SHELL
#define AETHER_RECOVERY_SHELL "/bin/sh"
#endif
#define RECOVERY_RETRY_MS 5000     /* retry spawning a failed recovery shell */
#define ACT_NONE    AINIT_ACT_NONE
#define ACT_POWEROFF AINIT_ACT_POWEROFF
#define ACT_REBOOT  AINIT_ACT_REBOOT

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static void do_reboot(int action)
{
#ifdef AETHER_TEST
    printf("AETHER-TEST: %s\n", action == ACT_REBOOT ? "reboot" : "poweroff");
    fflush(stdout);
    _exit(action == ACT_REBOOT ? 3 : 0);
#else
    reboot(action == ACT_REBOOT ? RB_AUTOBOOT : RB_POWER_OFF);
    for (;;)
        msleep(1000);
#endif
}

static void remove_pidfile(const AetherService *s)
{
    char path[128];
    snprintf(path, sizeof(path), "/run/aether-%s.pid", s->name);
    unlink(path);
}

/* Wait for one specific child; escalate to SIGKILL after the grace period. */
static void stop_service(AetherService *s)
{
    long long deadline = service_now_ms() + AETHER_GRACE_MS;
    int killed = 0;

    kill(-s->pid, SIGTERM);
    kill(s->pid, SIGTERM);

    for (;;) {
        pid_t r = waitpid(s->pid, NULL, WNOHANG);
        if (r == s->pid || (r < 0 && errno != EINTR))
            break;
        if (!killed && service_now_ms() >= deadline) {
            fprintf(stderr, "Aether: %s ignored SIGTERM; sending SIGKILL\n",
                    s->name);
            kill(-s->pid, SIGKILL);
            kill(s->pid, SIGKILL);
            killed = 1;
        }
        msleep(20);
    }
    remove_pidfile(s);
    s->pid = -1;
}


/* ---------------- /etc/aether/fstab ---------------- */

#ifndef AETHER_FSTAB
#define AETHER_FSTAB "/etc/aether/fstab"
#endif
#define MAX_MOUNTS 16

static char mounted_targets[MAX_MOUNTS][256];
static int n_mounted;

static int mkdir_p(const char *path)
{
    char tmp[256];

    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return -1;
    for (char *c = tmp + 1; ; c++) {
        if (*c == '/' || *c == '\0') {
            char save = *c;
            *c = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *c = save;
            if (!save)
                break;
        }
    }
    return 0;
}

/* "ro,nosuid,size=1m" -> MS_ flags + leftover filesystem data string. */
static void parse_mount_opts(char *opts, unsigned long *flags, char *data,
                             size_t dsz)
{
    static const struct { const char *name; unsigned long flag; } known[] = {
        { "ro", MS_RDONLY }, { "nosuid", MS_NOSUID }, { "nodev", MS_NODEV },
        { "noexec", MS_NOEXEC }, { "noatime", MS_NOATIME },
        { "sync", MS_SYNCHRONOUS },
    };
    char *save = NULL;

    *flags = 0;
    data[0] = '\0';
    for (char *t = strtok_r(opts, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        int hit = 0;

        if (strcmp(t, "defaults") == 0 || strcmp(t, "rw") == 0)
            continue;
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
            if (strcmp(t, known[i].name) == 0) {
                *flags |= known[i].flag;
                hit = 1;
            }
        }
        if (!hit && strlen(data) + strlen(t) + 2 < dsz) {
            if (data[0])
                strcat(data, ",");
            strcat(data, t);
        }
    }
}

static void mount_fstab(void)
{
    FILE *f = fopen(AETHER_FSTAB, "r");
    char line[512];
    int ln = 0;

    if (!f)
        return;                      /* no fstab: nothing to mount */
    while (fgets(line, sizeof(line), f)) {
        char src[128], target[256], type[32], opts[128] = "defaults", extra[8];
        char *p = line;

        ln++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;
        int n = sscanf(p, "%127s %255s %31s %127s %7s", src, target, type, opts, extra);
        if (n < 3 || n > 4 || target[0] != '/' || strstr(target, "..")) {
            fprintf(stderr, "Aether: fstab line %d invalid; skipped\n", ln);
            continue;
        }
        if (n_mounted >= MAX_MOUNTS) {
            fprintf(stderr, "Aether: fstab: too many mounts; skipped %s\n", target);
            continue;
        }
        unsigned long flags;
        char data[128];
        parse_mount_opts(opts, &flags, data, sizeof(data));

        if (mkdir_p(target) != 0 ||
            mount(src, target, type, flags, data[0] ? data : NULL) != 0) {
            fprintf(stderr, "Aether: mount %s on %s failed: %s\n", type, target,
                    strerror(errno));
            continue;
        }
        snprintf(mounted_targets[n_mounted++], sizeof(mounted_targets[0]), "%s", target);
        printf("Aether: mounted %s\n", target);
    }
    fclose(f);
    fflush(stdout);
}

static void unmount_all(void)
{
    for (int i = n_mounted - 1; i >= 0; i--) {
        if (umount(mounted_targets[i]) == 0) {
            printf("Aether: unmounted %s\n", mounted_targets[i]);
        } else if (mount(NULL, mounted_targets[i], NULL,
                         MS_REMOUNT | MS_RDONLY, NULL) == 0) {
            printf("Aether: %s busy; remounted read-only\n", mounted_targets[i]);
        } else {
            fprintf(stderr, "Aether: cannot unmount %s: %s\n",
                    mounted_targets[i], strerror(errno));
        }
    }
    n_mounted = 0;
    fflush(stdout);
}

static void shutdown_system(AetherService *services, int count, int action)
{
    printf("Aether: %s\n", action == ACT_REBOOT ? "rebooting" : "powering off");
    fflush(stdout);

    unlink(AETHER_CTL_SOCK);

    /* Stop in reverse start order so dependents stop before dependencies. */
    for (int i = count - 1; i >= 0; i--) {
        services[i].restart_at_ms = 0;
        if (services[i].pid > 0)
            stop_service(&services[i]);
    }

    /* Remaining strays (orphans re-parented to us). */
    kill(-1, SIGTERM);
    msleep(100);
    kill(-1, SIGKILL);
    ainit_reap_children(NULL, NULL);

    sync();
    unmount_all();
    sync();
    do_reboot(action);
}

static void set_recovery(AetherService *s)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->name_storage, sizeof(s->name_storage), "%s", "recovery");
    snprintf(s->program_storage, sizeof(s->program_storage), "%s",
             AETHER_RECOVERY_SHELL);
    s->name = s->name_storage;
    s->program = s->program_storage;
    s->pid = -1;
    s->interactive = 1;
}

/* Replace the console (or add one) with the recovery shell. */
static int install_recovery(AetherService *services, int *count,
                            int *console_index)
{
    int slot = *console_index;

    if (slot < 0) {
        slot = *count < MAX_SERVICES ? *count : MAX_SERVICES - 1;
        if (*count < MAX_SERVICES)
            (*count)++;
    }
    set_recovery(&services[slot]);
    *console_index = slot;
    return slot;
}

/* ---------------- boot environment ---------------- */

/* Best effort: a line on the kernel log, visible even when stdio is unusable. */
static void klog(const char *msg)
{
#ifndef AETHER_TEST
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

    if (fd >= 0) {
        dprintf(fd, "<3>aether-init: %s\n", msg);
        close(fd);
    }
#else
    (void)msg;
#endif
}

/* Mount /dev, /proc, /sys, /run unless already there. Returns the number of
 * critical failures (/dev and /proc are needed for a usable system). */
static int init_boot_filesystems(void)
{
    int critical = 0;

#if defined(AETHER_TEST) && !defined(AETHER_TEST_REAL_MOUNTS)
    return 0;                        /* test build: the harness owns the mounts */
#endif
    for (int i = 0; i < ainit_boot_mount_count; i++) {
        const AInitMount *m = &ainit_boot_mounts[i];
        int created = 0, err = 0;
        const char *type = m->type, *source = m->source;
        AInitMountResult r;

#ifdef AETHER_TEST_DEV_TYPE   /* tests: never expose the host's devtmpfs */
        if (strcmp(m->target, "/dev") == 0)
            type = source = AETHER_TEST_DEV_TYPE;
#endif
        r = ainit_mount(source, m->target, type, m->flags, m->data,
                        m->magic, &created, &err);

        switch (r) {
        case AINIT_MOUNT_OK:
            printf("Aether: init: mounted %s (%s)%s\n", m->target, type,
                   created ? ", created mount point" : "");
            break;
        case AINIT_MOUNT_ALREADY:
            printf("Aether: init: %s already mounted\n", m->target);
            break;
        default:
            fprintf(stderr, "Aether: init: %s: %s on %s %s%s%s\n",
                    m->critical ? "CRITICAL" : "warning", type, m->target,
                    ainit_mount_result_name(r), err ? ": " : "",
                    err ? strerror(err) : "");
            if (m->critical) {
                critical++;
                klog("essential filesystem unavailable");
            }
            break;
        }
    }
    fflush(stdout);
    return critical;
}

/* Console for PID 1 and the services that inherit it. */
static void init_console(int final_attempt)
{
#ifndef AETHER_TEST
    int r = ainit_console_attach("/dev/console", final_attempt ? "/dev/null" : NULL);

    if (!final_attempt)
        return;
    if (r == 1)
        klog("/dev/console unavailable; stdio is /dev/null");
    else if (r < 0)
        klog("cannot set up stdio");
#else
    (void)final_attempt;
#endif
}

/* Drop every configured service; the recovery shell becomes the console. */
static void enter_recovery_only(AetherService *services, int *count,
                                int *console_index)
{
    memset(services, 0, sizeof(AetherService) * MAX_SERVICES);
    *count = 0;
    *console_index = -1;
    install_recovery(services, count, console_index);
}

struct emerg_ctx {
    AetherService *rec;
    long long now;
};

static void emerg_reap(pid_t pid, int status, void *vctx)
{
    struct emerg_ctx *c = vctx;
    AetherService *r = c->rec;

    if (pid != r->pid)
        return;
    r->pid = -1;
    remove_pidfile(r);
    if (WIFEXITED(status))
        printf("Aether: recovery shell exited with status %d.\n",
               WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        printf("Aether: recovery shell killed by signal %d.\n",
               WTERMSIG(status));
    if (ainit_restart_decide(1, &r->restart_count, r->max_restarts,
                             c->now - r->started_ms, STABLE_UPTIME_MS)) {
        r->restart_at_ms = c->now + RESTART_DELAY_MS;
    } else {
        puts("Aether: init: recovery shell restart limit reached; "
             "holding PID 1 (SIGTERM powers off)");
        r->restart_at_ms = 0;
    }
    fflush(stdout);
}

/*
 * Last resort when the normal event loop cannot be built (no signalfd).
 * PID 1 must not exit, so supervise only a recovery shell, with signals
 * collected by sigtimedwait(). Returns only through shutdown_system().
 */
static void emergency_supervise(const sigset_t *set)
{
    AetherService rec;
    int action = ACT_NONE;

    set_recovery(&rec);
    rec.max_restarts = 5;
    rec.restart = 1;
    puts("Aether: init: EMERGENCY mode: recovery shell only");
    fflush(stdout);
    if (service_start(&rec) < 0) {
        fprintf(stderr, "Aether: init: recovery shell failed to start; retrying\n");
        rec.restart_at_ms = service_now_ms() + RECOVERY_RETRY_MS;
    }

    while (action == ACT_NONE) {
        struct timespec ts = { 1, 0 };
        siginfo_t si;
        int sig = sigtimedwait(set, &si, &ts);
        struct emerg_ctx ctx = { &rec, 0 };

        if (sig > 0)
            action = ainit_action_for_signal(sig);
        if (action != ACT_NONE)
            break;
        ctx.now = service_now_ms();
        ainit_reap_children(emerg_reap, &ctx);
        if (rec.pid <= 0 && rec.restart_at_ms > 0 && ctx.now >= rec.restart_at_ms) {
            rec.restart_at_ms = 0;
            if (service_start(&rec) < 0)
                rec.restart_at_ms = ctx.now + RECOVERY_RETRY_MS;
        }
    }
    shutdown_system(&rec, 1, action);
}

/* ---------------- runtime control socket ---------------- */

static int ctl_listen(void)
{
    struct sockaddr_un sa;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", AETHER_CTL_SOCK);
    unlink(AETHER_CTL_SOCK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        chmod(AETHER_CTL_SOCK, 0600) < 0 || listen(fd, 4) < 0) {
        fprintf(stderr, "Aether: control socket unavailable: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static const char *state_of(const AetherService *s)
{
    if (s->pid > 0)
        return "running";
    if (s->restart_at_ms > 0)
        return "restarting";
    if (s->manual_stop)
        return "stopped";
    return "exited";
}

static AetherService *find_service(AetherService *v, int count, const char *n)
{
    for (int i = 0; i < count; i++)
        if (strcmp(v[i].name, n) == 0)
            return &v[i];
    return NULL;
}

static void ctl_status_line(int fd, const AetherService *s)
{
    dprintf(fd, "%s %s pid=%d restarts=%d/%d\n", s->name, state_of(s),
            (int)(s->pid > 0 ? s->pid : 0), s->restart_count, s->max_restarts);
}

static void ctl_handle(int lfd, AetherService *v, int count, int console_index)
{
    int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    struct ucred cred;
    socklen_t cl = sizeof(cred);
    char req[128];

    if (fd < 0)
        return;
    struct timeval tv = { 0, 200000 };      /* a stalled client must not block init */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

#ifndef AETHER_TEST
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) < 0 || cred.uid != 0) {
        dprintf(fd, "error: permission denied\n");
        close(fd);
        return;
    }
#else
    (void)cred; (void)cl;
#endif

    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) {
        close(fd);
        return;
    }
    req[n] = '\0';
    req[strcspn(req, "\n")] = '\0';

    char *arg = strchr(req, ' ');
    if (arg)
        *arg++ = '\0';

    if (strcmp(req, "list") == 0 && !arg) {
        for (int i = 0; i < count; i++)
            ctl_status_line(fd, &v[i]);
    } else if (strcmp(req, "status") == 0 && arg) {
        AetherService *s = find_service(v, count, arg);
        if (s) ctl_status_line(fd, s);
        else dprintf(fd, "error: unknown service %s\n", arg);
    } else if ((strcmp(req, "start") == 0 || strcmp(req, "stop") == 0 ||
                strcmp(req, "restart") == 0) && arg) {
        AetherService *s = find_service(v, count, arg);

        if (!s) {
            dprintf(fd, "error: unknown service %s\n", arg);
        } else if (s == &v[console_index] && strcmp(req, "start") != 0) {
            dprintf(fd, "error: refusing to stop the console service\n");
        } else if (strcmp(req, "start") == 0 && (s->pid > 0 || s->restart_at_ms > 0)) {
            dprintf(fd, "error: %s is already %s\n", s->name, state_of(s));
        } else if (strcmp(req, "stop") == 0 && s->pid <= 0 && s->restart_at_ms <= 0) {
            dprintf(fd, "error: %s is not running\n", s->name);
        } else {
            if (strcmp(req, "start") != 0) {
                s->manual_stop = 1;
                s->restart_at_ms = 0;
                if (s->pid > 0)
                    stop_service(s);
            }
            if (strcmp(req, "stop") != 0) {
                s->manual_stop = 0;
                s->restart_count = 0;
                s->restart_at_ms = 0;
                if (service_start(s) < 0) {
                    dprintf(fd, "error: failed to start %s\n", s->name);
                    close(fd);
                    return;
                }
            }
            printf("Aether: %s %s (control)\n", req, s->name);
            fflush(stdout);
            ctl_status_line(fd, s);
        }
    } else {
        dprintf(fd, "error: bad request\n");
    }
    close(fd);
}

int main(void)
{
    struct utsname info;
    sigset_t set;

    int boot_critical;

    if (getpid() != 1) {
        fprintf(stderr, "aether-init: must run as PID 1\n");
        return 1;
    }

    init_console(0);                 /* use what the kernel/initramfs gave us */
    boot_critical = init_boot_filesystems();
    init_console(1);                 /* /dev is up now: finish stdio setup */

    /* Core configuration. Never fatal: missing or invalid files leave the
     * built-in defaults, which equal the previous hard-coded behaviour. */
    AetherConfig config;
    AetherConfigReport config_report;

    aether_config_load(AETHER_CONFIG_DIR, &config, &config_report);
    aether_config_print_issues(stderr, &config_report, "Aether: ");

    if (sethostname(config.system.hostname, strlen(config.system.hostname)) != 0)
        fprintf(stderr, "Aether: sethostname failed: %s\n", strerror(errno));

    /* Handle control signals and SIGCHLD synchronously: no lost wake-ups. */
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, NULL);

    if (config.boot.mount_fstab)
        mount_fstab();

#ifdef AETHER_FORCE_EMERGENCY        /* test hook: exercise the fallback path */
    int sfd = -1;
    errno = ENOSYS;
#else
    int sfd = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
#endif
    if (sfd < 0) {
        fprintf(stderr, "Aether: init: signalfd failed: %s\n", strerror(errno));
        emergency_supervise(&set);   /* never returns normally; PID 1 stays up */
        return 0;
    }

    if (uname(&info) != 0)
        snprintf(info.release, sizeof(info.release), "unknown");

    if (config.boot.banner) {
        char title[AETHER_SYSNAME_MAX + 1];
        size_t tlen = strlen(config.system.system_name);

        for (size_t i = 0; i <= tlen; i++) {
            char c = config.system.system_name[i];
            title[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
        puts("\n==============================");
        printf("%*s%s\n", tlen < 28 ? (int)((28 - tlen) / 2) : 0, "", title);
        puts("        Core v0.9");
        puts("==============================");
        printf("Hostname: %s\nKernel: %s\n", config.system.hostname,
               info.release);
        puts("Init: native C | Multi-service supervisor");
        fflush(stdout);
    }
    if (boot_critical > 0)
        printf("Aether: init: boot DEGRADED (%d critical failure(s))\n", boot_critical);
    fflush(stdout);

    AetherService services[MAX_SERVICES];
    memset(services, 0, sizeof(services));
    int console_index = -1, rejected = 0;
    int count = service_load_dir(AETHER_SERVICE_DIR, services, MAX_SERVICES,
                                 &console_index, &rejected);
    int recovery_mode = 0;

    if (count < 0)
        count = 0;
    if (rejected > 0)
        fprintf(stderr, "Aether: %d service file(s) rejected\n", rejected);

    if (boot_critical > 0) {
        fprintf(stderr, "Aether: init: CRITICAL: essential filesystems "
                "missing; starting recovery shell only\n");
        enter_recovery_only(services, &count, &console_index);
        recovery_mode = 1;
    }

    if (count == 0 || console_index < 0) {
        fprintf(stderr, "Aether: no valid console service configured\n");
        /* Keep the other services; add a recovery shell as console. */
        install_recovery(services, &count, &console_index);
        recovery_mode = 1;
    }

    if (service_order(services, count, &console_index) < 0) {
        fprintf(stderr, "Aether: invalid service dependency graph; "
                "starting recovery shell only\n");
        memset(services, 0, sizeof(services));
        count = 0;
        console_index = -1;
        install_recovery(services, &count, &console_index);
        recovery_mode = 1;
        service_order(services, count, &console_index);
    }

    int started[MAX_SERVICES] = {0};

    for (int i = 0; i < count; i++) {
        int blocked = 0;

        for (int d = 0; d < services[i].dependency_count; d++) {
            for (int j = 0; j < i; j++) {
                if (!started[j] && strcmp(services[j].name,
                                          services[i].dependency_storage[d]) == 0) {
                    fprintf(stderr, "Aether: not starting %s: required "
                            "service %s failed\n", services[i].name,
                            services[j].name);
                    blocked = 1;
                }
            }
        }
        if (blocked)
            continue;

        if (i == console_index)
            puts("Aether: all services started");

        if (service_start(&services[i]) < 0) {
            fprintf(stderr, "Aether: failed to start %s\n", services[i].name);

            if (i == console_index) {
                set_recovery(&services[i]);
                recovery_mode = 1;
                if (service_start(&services[i]) < 0) {
                    fprintf(stderr, "Aether: init: recovery shell failed to "
                            "start; retrying in %d s\n", RECOVERY_RETRY_MS / 1000);
                    services[i].restart_at_ms = service_now_ms() + RECOVERY_RETRY_MS;
                }
                started[i] = 1;
            }
            continue;
        }
        started[i] = 1;
    }

    int lfd = ctl_listen();
    int action = ACT_NONE;

    while (action == ACT_NONE) {
        long long now = service_now_ms();
        long long wait_ms = POLL_MS;

        /* Delayed restarts. */
        for (int i = 0; i < count; i++) {
            AetherService *s = &services[i];

            if (s->pid > 0 || s->restart_at_ms <= 0)
                continue;
            if (s->restart_at_ms > now) {
                if (s->restart_at_ms - now < wait_ms)
                    wait_ms = s->restart_at_ms - now;
                continue;
            }
            s->restart_at_ms = 0;
            if (service_start(s) < 0) {
                fprintf(stderr, "Aether: restart failed for %s\n", s->name);
                if (i == console_index)      /* never leave the console dead */
                    s->restart_at_ms = service_now_ms() + RECOVERY_RETRY_MS;
            }
        }

        struct pollfd pfd[2] = { { sfd, POLLIN, 0 }, { lfd, POLLIN, 0 } };
        poll(pfd, lfd >= 0 ? 2 : 1, (int)wait_ms);

        if (pfd[0].revents & POLLIN) {
            struct signalfd_siginfo ssi;

            while (read(sfd, &ssi, sizeof(ssi)) == (ssize_t)sizeof(ssi)) {
                int a = ainit_action_for_signal((int)ssi.ssi_signo);
                if (a != ACT_NONE)
                    action = a;
            }
        }
        if (action != ACT_NONE)
            break;
        if (lfd >= 0 && (pfd[1].revents & POLLIN))
            ctl_handle(lfd, services, count, console_index);

        int status;
        pid_t pid;

        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            int index = -1;

            for (int i = 0; i < count; i++) {
                if (services[i].pid == pid) {
                    index = i;
                    break;
                }
            }
            if (index < 0)
                continue;   /* re-parented orphan: reaped, nothing to do */

            AetherService *s = &services[index];
            long long uptime = service_now_ms() - s->started_ms;

            s->pid = -1;
            remove_pidfile(s);

            if (WIFEXITED(status))
                printf("Aether: %s exited with status %d.\n", s->name,
                       WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                printf("Aether: %s killed by signal %d.\n", s->name,
                       WTERMSIG(status));
            fflush(stdout);

            if (index == console_index && recovery_mode) {
                if (WIFEXITED(status) && (WEXITSTATUS(status) == 126 ||
                                          WEXITSTATUS(status) == 127))
                    fprintf(stderr, "Aether: init: recovery shell could not "
                            "be executed (%s)\n", AETHER_RECOVERY_SHELL);
                puts("Aether: recovery shell exited.");
                fflush(stdout);
                action = ACT_POWEROFF;
                break;
            }

            if (ainit_restart_decide(s->restart, &s->restart_count,
                                     s->max_restarts, uptime,
                                     STABLE_UPTIME_MS)) {
                printf("Aether: restarting %s (%d/%d).\n",
                       s->name, s->restart_count, s->max_restarts);
                fflush(stdout);
                s->restart_at_ms = service_now_ms() + RESTART_DELAY_MS;
                continue;
            }

            printf("Aether: %s stopped; restart limit reached.\n", s->name);
            fflush(stdout);

            if (index == console_index) {
                puts("Aether: entering recovery shell.");
                fflush(stdout);
                set_recovery(s);
                recovery_mode = 1;

                if (service_start(s) < 0) {
                    fprintf(stderr, "Aether: init: recovery shell failed to "
                            "start; retrying in %d s\n", RECOVERY_RETRY_MS / 1000);
                    s->restart_at_ms = service_now_ms() + RECOVERY_RETRY_MS;
                }
            }
        }
    }

    shutdown_system(services, count, action);
    return 0;
}
