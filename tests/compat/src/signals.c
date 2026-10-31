#define _GNU_SOURCE
#include "t.h"
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
static volatile sig_atomic_t usr1, chld, alrm;
static sigjmp_buf jb;
static void h_usr1(int s) { (void)s; usr1++; }
static void h_chld(int s) { (void)s; chld++; }
static void h_alrm(int s) { (void)s; alrm++; }
static void h_segv(int s, siginfo_t *si, void *u) { (void)s; (void)u; (void)si; siglongjmp(jb, 1); }
int main(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sigemptyset(&sa.sa_mask);
    sa.sa_handler = h_usr1; CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    sa.sa_handler = h_chld; CHECK(sigaction(SIGCHLD, &sa, NULL) == 0);
    sa.sa_handler = h_alrm; CHECK(sigaction(SIGALRM, &sa, NULL) == 0);
    raise(SIGUSR1); CHECK(usr1 == 1);
    CHECK(kill(getpid(), SIGUSR1) == 0); CHECK(usr1 == 2);
    /* blocked signal stays pending, delivered on unblock */
    sigset_t m, old, pend; sigemptyset(&m); sigaddset(&m, SIGUSR1);
    CHECK(sigprocmask(SIG_BLOCK, &m, &old) == 0);
    raise(SIGUSR1); CHECK(usr1 == 2);
    CHECK(sigpending(&pend) == 0 && sigismember(&pend, SIGUSR1) == 1);
    CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0); CHECK(usr1 == 3);
    /* alarm interrupts sigsuspend */
    sigset_t none; sigemptyset(&none); alarm(1);
    sigsuspend(&none); CHECK(errno == EINTR && alrm == 1);
    /* child exit -> SIGCHLD, status decoding */
    pid_t p = fork(); CHECK(p >= 0); if (p == 0) _exit(42);
    int st; CHECK(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 42); CHECK(chld >= 1);
    p = fork(); CHECK(p >= 0); if (p == 0) { pause(); _exit(0); }
    CHECK(kill(p, SIGTERM) == 0); CHECK(waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
    p = fork(); CHECK(p >= 0); if (p == 0) { for (;;) pause(); }
    CHECK(kill(p, SIGKILL) == 0); CHECK(waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    /* SIGPIPE ignored -> EPIPE */
    signal(SIGPIPE, SIG_IGN); int fd[2]; CHECK(pipe(fd) == 0); close(fd[0]);
    CHECK(write(fd[1], "x", 1) == -1 && errno == EPIPE); close(fd[1]);
    /* SIGSEGV on a PROT_NONE page handled on an alternate stack */
    static char alt[65536]; stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0 };
    CHECK(sigaltstack(&ss, NULL) == 0);
    memset(&sa, 0, sizeof sa); sigemptyset(&sa.sa_mask); sa.sa_sigaction = h_segv; sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    CHECK(sigaction(SIGSEGV, &sa, NULL) == 0);
    volatile char *bad = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECK(bad != MAP_FAILED);
    if (sigsetjmp(jb, 1) == 0) { (void)*bad; CHECK(!"no fault"); }
    /* real-time signal with payload */
    union sigval sv; sv.sival_int = 1234; sigset_t rt; sigemptyset(&rt); sigaddset(&rt, SIGRTMIN);
    CHECK(sigprocmask(SIG_BLOCK, &rt, NULL) == 0); CHECK(sigqueue(getpid(), SIGRTMIN, sv) == 0);
    siginfo_t si; struct timespec ts = {1, 0}; CHECK(sigtimedwait(&rt, &si, &ts) == SIGRTMIN && si.si_value.sival_int == 1234);
    DONE("signals");
}
