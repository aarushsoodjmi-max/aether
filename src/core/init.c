/*
 * Aether init (PID 1).
 *
 * Mounts the pseudo filesystems, loads and orders the service definitions in
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
 * Build with -DAETHER_TEST to make mounts a no-op and replace the reboot(2)
 * call by a message plus _exit(); used only by the integration tests.
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

enum { ACT_NONE = 0, ACT_POWEROFF, ACT_REBOOT };

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
    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    sync();
    unmount_all();
    sync();
    do_reboot(action);
}

static void set_recovery(AetherService *s)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->name_storage, sizeof(s->name_storage), "%s", "recovery");
    snprintf(s->program_storage, sizeof(s->program_storage), "%s", "/bin/sh");
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

static void mount_or_warn(const char *src, const char *target, const char *type)
{
#ifdef AETHER_TEST
    (void)src; (void)target; (void)type;
#else
    if (mount(src, target, type, 0, NULL) != 0 && errno != EBUSY)
        fprintf(stderr, "Aether: mount %s on %s failed: %s\n",
                type, target, strerror(errno));
#endif
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

static int action_for_signal(int sig)
{
    switch (sig) {
    case SIGTERM:
    case SIGINT:
    case SIGUSR2:
        return ACT_POWEROFF;
    case SIGUSR1:
        return ACT_REBOOT;
    default:
        return ACT_NONE;
    }
}

int main(void)
{
    struct utsname info;
    sigset_t set;

    if (getpid() != 1)
        return 1;

    mount_or_warn("devtmpfs", "/dev", "devtmpfs");
    mount_or_warn("proc", "/proc", "proc");
    mount_or_warn("sysfs", "/sys", "sysfs");
    if (sethostname("aether", 6) != 0)
        fprintf(stderr, "Aether: sethostname failed: %s\n", strerror(errno));

    /* Handle control signals and SIGCHLD synchronously: no lost wake-ups. */
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, NULL);

    mount_fstab();

    int sfd = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sfd < 0) {
        fprintf(stderr, "Aether: signalfd failed: %s\n", strerror(errno));
        return 1;
    }

    if (uname(&info) != 0)
        snprintf(info.release, sizeof(info.release), "unknown");

    puts("\n==============================");
    puts("         AETHER OS");
    puts("        Core v0.9");
    puts("==============================");
    printf("Hostname: aether\nKernel: %s\n", info.release);
    puts("Init: native C | Multi-service supervisor");
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
                    shutdown_system(services, count, ACT_POWEROFF);
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
            if (service_start(s) < 0)
                fprintf(stderr, "Aether: restart failed for %s\n", s->name);
        }

        struct pollfd pfd[2] = { { sfd, POLLIN, 0 }, { lfd, POLLIN, 0 } };
        poll(pfd, lfd >= 0 ? 2 : 1, (int)wait_ms);

        if (pfd[0].revents & POLLIN) {
            struct signalfd_siginfo ssi;

            while (read(sfd, &ssi, sizeof(ssi)) == (ssize_t)sizeof(ssi)) {
                int a = action_for_signal((int)ssi.ssi_signo);
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
                puts("Aether: recovery shell exited.");
                fflush(stdout);
                action = ACT_POWEROFF;
                break;
            }

            if (uptime >= STABLE_UPTIME_MS)
                s->restart_count = 0;

            if (s->restart && s->restart_count < s->max_restarts) {
                s->restart_count++;
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
                    fprintf(stderr,
                            "Aether: recovery shell failed to start\n");
                    action = ACT_POWEROFF;
                    break;
                }
            }
        }
    }

    shutdown_system(services, count, action);
    return 0;
}
