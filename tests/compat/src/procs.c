#define _GNU_SOURCE
#include "t.h"
#include <unistd.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
extern char **environ;
static void spawn_sh(const char *cmd, int *status) {
    pid_t p; char *av[] = { "sh", "-c", (char *)cmd, NULL };
    CHECK(posix_spawn(&p, "/bin/sh", NULL, NULL, av, environ) == 0);
    CHECK(waitpid(p, status, 0) == p);
}
int main(void) {
    int st; spawn_sh("exit 7", &st); CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7);
    /* pipe + dup2 capture of a child's stdout */
    int fd[2]; CHECK(pipe2(fd, O_CLOEXEC) == 0);
    pid_t p = fork(); CHECK(p >= 0);
    if (p == 0) { dup2(fd[1], 1); char *av[] = { "echo", "captured", NULL }; execv("/bin/echo", av); _exit(127); }
    close(fd[1]); char buf[64] = {0}; ssize_t n = read(fd[0], buf, sizeof buf - 1); CHECK(n > 0);
    CHECK(strcmp(buf, "captured\n") == 0); CHECK(waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 0); close(fd[0]);
    /* shebang script; and exec of a file without any x bit fails EACCES even for root */
    const char *sp = "/tmp/aether-shebang.sh";
    int f = open(sp, O_CREAT | O_TRUNC | O_WRONLY, 0644); CHECK(f >= 0);
    CHECK(write(f, "#!/bin/sh\nexit 9\n", 16) == 16); close(f);
    errno = 0; char *av0[] = { (char *)sp, NULL }; execv(sp, av0); CHECK(errno == EACCES);
    CHECK(chmod(sp, 0755) == 0);
    p = fork(); CHECK(p >= 0); if (p == 0) { execv(sp, av0); _exit(127); }
    CHECK(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 9); unlink(sp);
    /* ENOENT / ENOEXEC distinctions */
    errno = 0; execv("/nonexistent/prog", av0); CHECK(errno == ENOENT);
    /* sessions, process groups, orphan reparenting to init */
    p = fork(); CHECK(p >= 0);
    if (p == 0) { CHECK(setsid() == getpid()); CHECK(getpgrp() == getpid()); _exit(0); }
    CHECK(waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 0);
    CHECK(waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD);
    pid_t me = getpid();
    p = fork(); CHECK(p >= 0);
    if (p == 0) { if (fork() == 0) { usleep(300000); _exit(getppid() == me ? 1 : 0); } _exit(0); }
    CHECK(waitpid(p, &st, 0) == p); usleep(600000);            /* grandchild was reparented, not our child */
    CHECK(waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD);
    DONE("procs");
}
