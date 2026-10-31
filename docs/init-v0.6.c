#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <sys/reboot.h>
#include <sys/wait.h>

static volatile sig_atomic_t shutdown_requested = 0;

static void handle_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
        shutdown_requested = 1;
}

static void reap_children(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

static void shutdown_system(pid_t shell_pid)
{
    if (shell_pid > 0) {
        if (kill(-shell_pid, SIGTERM) == -1)
            kill(shell_pid, SIGTERM);

        while (waitpid(shell_pid, NULL, 0) == -1 && errno == EINTR) {}
    }

    reap_children();
    sync();
    reboot(RB_POWER_OFF);

    for (;;)
        pause();
}

int main(void)
{
    struct utsname info;
    struct sigaction sa = {0};
    pid_t shell_pid = -1;

    if (getpid() != 1) {
        fprintf(stderr, "Aether init must run as PID 1\n");
        return 1;
    }

    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == -1)
        perror("mount /dev");
    if (mount("proc", "/proc", "proc", 0, NULL) == -1)
        perror("mount /proc");
    if (mount("sysfs", "/sys", "sysfs", 0, NULL) == -1)
        perror("mount /sys");

    if (sethostname("aether", 6) == -1)
        perror("sethostname");

    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    uname(&info);

    puts("\n==============================");
    puts("         AETHER OS");
    puts("        Core v0.6");
    puts("==============================");
    printf("Hostname: aether\nKernel: %s\n", info.release);
    puts("Init: native C | PID 1");
    puts("Type 'poweroff' to shut down.\n");
    fflush(stdout);

    while (!shutdown_requested) {
        shell_pid = fork();

        if (shell_pid == 0) {
            execl("/bin/setsid", "setsid", "/bin/cttyhack",
                  "/bin/sh", (char *)NULL);
            _exit(127);
        }

        if (shell_pid < 0) {
            if (errno == EINTR)
                continue;
            perror("fork");
            sleep(1);
            continue;
        }

        pid_t result;
        do {
            result = waitpid(shell_pid, NULL, 0);
        } while (result == -1 && errno == EINTR &&
                 !shutdown_requested);

        if (shutdown_requested)
            shutdown_system(shell_pid);

        shell_pid = -1;
        reap_children();

        if (!shutdown_requested) {
            puts("Aether: shell exited; restarting.");
            fflush(stdout);
        }
    }

    shutdown_system(shell_pid);
    return 0;
}
