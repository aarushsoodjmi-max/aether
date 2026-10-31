#define _GNU_SOURCE
#include "proto.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>

int proto_send(int fd, const Msg *m, int passfd)
{
    struct iovec iov = { (void *)m, sizeof *m };
    char cb[CMSG_SPACE(sizeof(int))]; struct msghdr mh; memset(&mh, 0, sizeof mh); mh.msg_iov = &iov; mh.msg_iovlen = 1;
    if (passfd >= 0) {
        memset(cb, 0, sizeof cb); mh.msg_control = cb; mh.msg_controllen = sizeof cb;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(c), &passfd, sizeof(int));
    }
    for (;;) { ssize_t r = sendmsg(fd, &mh, MSG_NOSIGNAL); if (r == (ssize_t)sizeof *m) return 0; if (r < 0 && errno == EINTR) continue; return -1; }
}
int proto_recv(int fd, Msg *m, int *recvfd)
{
    struct iovec iov = { m, sizeof *m }; char cb[CMSG_SPACE(sizeof(int))]; struct msghdr mh; memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = cb; mh.msg_controllen = sizeof cb;
    if (recvfd) *recvfd = -1;
    ssize_t r; do r = recvmsg(fd, &mh, MSG_CMSG_CLOEXEC); while (r < 0 && errno == EINTR);
    if (r == 0) return 0;
    if (r < 0) return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) { int f; memcpy(&f, CMSG_DATA(c), sizeof f); if (recvfd && *recvfd < 0) *recvfd = f; else close(f); }   /* keep the first fd, close extras */
    if (r != (ssize_t)sizeof *m || (mh.msg_flags & MSG_TRUNC)) { if (recvfd && *recvfd >= 0) { close(*recvfd); *recvfd = -1; } errno = EPROTO; return -1; }
    m->text[sizeof m->text - 1] = 0; return 1;
}
