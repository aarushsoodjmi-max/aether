#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include "service.h"

static volatile sig_atomic_t shutdown_requested = 0;

static void handle_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
        shutdown_requested = 1;
}

static void reap_orphans(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

static void shutdown_system(AetherService *console)
{
    if (console->pid > 0) {
        kill(-console->pid, SIGTERM);
        kill(console->pid, SIGTERM);

        while (waitpid(console->pid, NULL, 0) < 0 &&
               errno == EINTR) {}
    }

    reap_orphans();
    sync();
    reboot(RB_POWER_OFF);

    for (;;)
        pause();
}

int main(void)
{
    struct utsname info;
    struct sigaction sa = {0};

    if (getpid() != 1)
        return 1;

    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    sethostname("aether", 6);

    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    uname(&info);

    puts("\n==============================");
    puts("         AETHER OS");
    puts("        Core v0.7");
    puts("==============================");
    printf("Hostname: aether\nKernel: %s\n", info.release);
    puts("Init: native C | Service supervisor");
    fflush(stdout);


    AetherService console = {0};

    if (service_load("/etc/aether/services/console.conf", &console) < 0) {
        fprintf(stderr, "Aether: failed to load console config\n");
        return 1;
    }

    int recovery_mode = 0;

    while (!shutdown_requested) {
        if (console.pid < 0) {
            if (service_start(&console) < 0) {
                sleep(1);
                continue;
            }
        }

        int status;
        pid_t result;

        do {
            result = waitpid(console.pid, &status, 0);
        } while (result < 0 && errno == EINTR && !shutdown_requested);

        if (shutdown_requested)
            break;

        if (result == console.pid || (result < 0 && errno == ECHILD)) {
            console.pid = -1;

            char pidfile[128];
            snprintf(pidfile, sizeof(pidfile),
                     "/run/aether-%s.pid", console.name);
            unlink(pidfile);

            reap_orphans();

            if (console.restart &&
                console.restart_count < console.max_restarts) {
                console.restart_count++;
                printf("Aether: restarting %s (%d/%d).\n",
                       console.name, console.restart_count,
                       console.max_restarts);
                fflush(stdout);
            } else if (!recovery_mode) {
                puts("Aether: console stopped; entering recovery shell.");
                fflush(stdout);
                recovery_mode = 1;
                console.name = "recovery";
                console.program = "/bin/sh";
                console.restart = 0;
                console.restart_count = 0;
                console.max_restarts = 0;
                continue;
            } else {
                puts("Aether: recovery shell exited.");
                fflush(stdout);
                break;
            }
        } else if (result < 0) {
            perror("Aether: waitpid");
            sleep(1);
        }
    }

    shutdown_system(&console);
    return 0;
}
