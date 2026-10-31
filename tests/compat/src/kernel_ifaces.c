#define _GNU_SOURCE
#include "t.h"
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
int main(void) {
    struct utsname u; CHECK(uname(&u) == 0 && strcmp(u.sysname, "Linux") == 0 && strcmp(u.machine, "x86_64") == 0);
    int ep = epoll_create1(EPOLL_CLOEXEC); CHECK(ep >= 0);
    int ev = eventfd(0, EFD_NONBLOCK); CHECK(ev >= 0);
    struct epoll_event e = { .events = EPOLLIN, .data.fd = ev }; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e) == 0);
    uint64_t one = 1; CHECK(write(ev, &one, 8) == 8);
    struct epoll_event out; CHECK(epoll_wait(ep, &out, 1, 1000) == 1 && out.data.fd == ev);
    int tf = timerfd_create(CLOCK_MONOTONIC, 0); CHECK(tf >= 0);
    struct itimerspec its = { .it_value = { 0, 50 * 1000 * 1000 } }; CHECK(timerfd_settime(tf, 0, &its, NULL) == 0);
    uint64_t exp; CHECK(read(tf, &exp, 8) == 8 && exp >= 1);
    int in = inotify_init1(IN_NONBLOCK); CHECK(in >= 0);
    int w = inotify_add_watch(in, "/tmp", IN_CREATE); CHECK(w >= 0);
    int c = open("/tmp/aether-ino", O_CREAT | O_WRONLY, 0600); CHECK(c >= 0); close(c);
    char ib[512]; CHECK(read(in, ib, sizeof ib) > 0); unlink("/tmp/aether-ino");
    int sv[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct msghdr mh; memset(&mh, 0, sizeof mh);                /* SCM_RIGHTS fd passing */
    char cb[CMSG_SPACE(sizeof(int))]; memset(cb, 0, sizeof cb); struct iovec io = { "x", 1 }; mh.msg_iov = &io; mh.msg_iovlen = 1; mh.msg_control = cb; mh.msg_controllen = sizeof cb;
    struct cmsghdr *ch = CMSG_FIRSTHDR(&mh); ch->cmsg_level = SOL_SOCKET; ch->cmsg_type = SCM_RIGHTS; ch->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(ch), &ev, sizeof(int));
    CHECK(sendmsg(sv[0], &mh, 0) == 1);
    char rb; struct iovec io2 = { &rb, 1 }; char cb2[CMSG_SPACE(sizeof(int))]; struct msghdr m2; memset(&m2, 0, sizeof m2); m2.msg_iov = &io2; m2.msg_iovlen = 1; m2.msg_control = cb2; m2.msg_controllen = sizeof cb2;
    CHECK(recvmsg(sv[1], &m2, 0) == 1); struct cmsghdr *rc = CMSG_FIRSTHDR(&m2); CHECK(rc && rc->cmsg_type == SCM_RIGHTS);
    int nfd; memcpy(&nfd, CMSG_DATA(rc), sizeof nfd); CHECK(nfd >= 0 && nfd != ev);
    int mf = (int)syscall(SYS_memfd_create, "aether", 0u); CHECK(mf >= 0 && ftruncate(mf, 4096) == 0);
    unsigned char rnd[16]; CHECK(getrandom(rnd, sizeof rnd, 0) == (ssize_t)sizeof rnd);
    struct timespec t0, t1; CHECK(clock_gettime(CLOCK_MONOTONIC, &t0) == 0); struct timespec nap = { 0, 20 * 1000 * 1000 }; nanosleep(&nap, NULL);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t1) == 0); CHECK((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec) >= 19 * 1000 * 1000);
    struct rlimit rl; CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur >= 64);
    struct timespec rt; CHECK(clock_gettime(CLOCK_REALTIME, &rt) == 0); CHECK(rt.tv_sec > 1700000000);
    DONE("kernel_ifaces");
}
