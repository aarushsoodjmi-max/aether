/*
 * aetherctl - Aether system control.
 *
 *   aetherctl poweroff | reboot     ask PID 1 to shut the system down cleanly
 *   aetherctl start|stop|restart NAME   control a running service
 *   aetherctl status NAME | list        show service state
 *   aetherctl check [DIR]           validate service configuration
 *   aetherctl help
 *
 * Also installed as /bin/poweroff and /bin/reboot (selected by argv[0]).
 * Shutdown is requested by signalling the Aether init, which stops every
 * service in reverse dependency order before calling reboot(2); aetherctl
 * never calls reboot(2) itself, so services are always stopped cleanly.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <libgen.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "service.h"

#define DEFAULT_SERVICE_DIR "/etc/aether/services"

static int is_aether_init(void)
{
    char comm[32] = "";
    FILE *f = fopen("/proc/1/comm", "r");

    if (!f)
        return 0;
    if (fgets(comm, sizeof(comm), f))
        comm[strcspn(comm, "\n")] = '\0';
    fclose(f);
    return strcmp(comm, "init") == 0;
}

static int request_shutdown(int sig, const char *what)
{
    if (!is_aether_init()) {
        fprintf(stderr, "aetherctl: PID 1 is not the Aether init; "
                "refusing to %s\n", what);
        return 1;
    }

    sync();
    if (kill(1, sig) != 0) {
        fprintf(stderr, "aetherctl: cannot signal init: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int check_config(const char *dir)
{
    AetherService services[MAX_SERVICES];
    int console = -1, rejected = 0;

    memset(services, 0, sizeof(services));
    int count = service_load_dir(dir, services, MAX_SERVICES, &console,
                                 &rejected);
    if (count < 0)
        return 1;

    int bad = rejected > 0;

    if (count == 0 || console < 0) {
        fprintf(stderr, "check: no valid console service\n");
        bad = 1;
    }
    if (count > 0 && service_order(services, count, &console) < 0) {
        fprintf(stderr, "check: invalid dependency graph\n");
        bad = 1;
    }

    if (!bad) {
        printf("start order:");
        for (int i = 0; i < count; i++)
            printf(" %s", services[i].name);
        printf("\n");
    }
    printf("check: %d service(s) valid, %d file(s) rejected: %s\n",
           count, rejected, bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}

static int control(const char *request)
{
    struct sockaddr_un sa;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    char buf[512];
    ssize_t n;
    int first = 1, rc = 0;

    if (fd < 0) { perror("aetherctl: socket"); return 1; }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s",
             getenv("AETHER_CTL_SOCK") ? getenv("AETHER_CTL_SOCK") : "/run/aether.sock");
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "aetherctl: cannot reach init (%s): %s\n", sa.sun_path,
                strerror(errno));
        return 1;
    }
    if (write(fd, request, strlen(request)) < 0) { perror("aetherctl: write"); return 1; }
    shutdown(fd, SHUT_WR);
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        if (first && strncmp(buf, "error:", 6) == 0)
            rc = 1;
        first = 0;
        fputs(buf, rc ? stderr : stdout);
    }
    close(fd);
    if (first) { fprintf(stderr, "aetherctl: empty reply from init\n"); return 1; }
    return rc;
}

static int usage(FILE *out, int rc)
{
    fprintf(out, "Usage: aetherctl poweroff | reboot | list | status NAME |\n"
                 "       start NAME | stop NAME | restart NAME | check [DIR] | help\n");
    return rc;
}

int main(int argc, char **argv)
{
    char *self_copy = strdup(argv[0]);
    const char *self = self_copy ? basename(self_copy) : "aetherctl";
    const char *cmd;
    int first_arg;

    if (strcmp(self, "poweroff") == 0 || strcmp(self, "reboot") == 0) {
        cmd = self;
        first_arg = 1;
    } else if (argc >= 2) {
        cmd = argv[1];
        first_arg = 2;
    } else {
        return usage(stderr, 2);
    }

    if (strcmp(cmd, "poweroff") == 0 && argc == first_arg)
        return request_shutdown(SIGUSR2, "power off");
    if (strcmp(cmd, "reboot") == 0 && argc == first_arg)
        return request_shutdown(SIGUSR1, "reboot");
    if (strcmp(cmd, "check") == 0 && argc <= first_arg + 1)
        return check_config(argc > first_arg ? argv[first_arg]
                                             : DEFAULT_SERVICE_DIR);
    if (strcmp(cmd, "list") == 0 && argc == first_arg)
        return control("list\n");
    if ((strcmp(cmd, "start") == 0 || strcmp(cmd, "stop") == 0 ||
         strcmp(cmd, "restart") == 0 || strcmp(cmd, "status") == 0) &&
        argc == first_arg + 1) {
        char req[160];
        snprintf(req, sizeof(req), "%s %s\n", cmd, argv[first_arg]);
        return control(req);
    }
    if (strcmp(cmd, "help") == 0)
        return usage(stdout, 0);

    return usage(stderr, 2);
}
