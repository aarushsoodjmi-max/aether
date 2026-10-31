#define _GNU_SOURCE
#include "t.h"
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/statfs.h>
#include <sys/file.h>
#include <sys/wait.h>
static char *slurp(const char *p, char *b, size_t n) { int f = open(p, O_RDONLY); CHECK(f >= 0); ssize_t r = read(f, b, n - 1); CHECK(r >= 0); b[r] = 0; close(f); return b; }
int main(int argc, char **argv) {
    (void)argc; char b[8192], ex[PATH_MAX], rp[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", ex, sizeof ex - 1); CHECK(n > 0); ex[n] = 0;
    CHECK(realpath(argv[0], rp) != NULL); CHECK(strcmp(rp, ex) == 0);
    slurp("/proc/self/maps", b, sizeof b); CHECK(strstr(b, "libc.so.6") != NULL);
    slurp("/proc/self/status", b, sizeof b); CHECK(strstr(b, "Name:\tfs_paths") != NULL);
    slurp("/proc/self/cmdline", b, sizeof b); CHECK(strstr(b, "fs_paths") != NULL);
    slurp("/proc/meminfo", b, sizeof b); CHECK(strstr(b, "MemTotal:") != NULL);
    slurp("/proc/cpuinfo", b, sizeof b); CHECK(strstr(b, "processor") != NULL);
    int d = open("/dev/null", O_WRONLY); CHECK(d >= 0 && write(d, "x", 1) == 1); close(d);
    d = open("/dev/urandom", O_RDONLY); CHECK(d >= 0); unsigned char r[32] = {0}; CHECK(read(d, r, 32) == 32); close(d);
    int nz = 0; for (int i = 0; i < 32; i++) nz |= r[i]; CHECK(nz);
    d = open("/dev/zero", O_RDWR); CHECK(d >= 0); char *z = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE, d, 0); CHECK(z != MAP_FAILED && z[100] == 0); close(d);
    struct stat st; CHECK(stat("/tmp", &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 01777) == 01777);
    CHECK(stat("/dev/null", &st) == 0 && S_ISCHR(st.st_mode));
    CHECK(stat("/dev/console", &st) == 0 && S_ISCHR(st.st_mode));
    const char *f1 = "/tmp/aether-fs-1", *f2 = "/tmp/aether-fs-2", *ln = "/tmp/aether-fs-ln";
    d = open(f1, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0640); CHECK(d >= 0);
    CHECK(ftruncate(d, 1 << 16) == 0 && flock(d, LOCK_EX | LOCK_NB) == 0);
    char *m = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE, MAP_SHARED, d, 0); CHECK(m != MAP_FAILED); memcpy(m, "mapped", 6); CHECK(msync(m, 4096, MS_SYNC) == 0);
    char c6[7] = {0}; CHECK(pread(d, c6, 6, 0) == 6 && strcmp(c6, "mapped") == 0); munmap(m, 1 << 16); close(d);
    CHECK(stat(f1, &st) == 0 && (st.st_mode & 0777) == 0640 && st.st_size == 1 << 16);
    CHECK(rename(f1, f2) == 0 && access(f1, F_OK) == -1);
    CHECK(symlink(f2, ln) == 0); char tg[PATH_MAX]; n = readlink(ln, tg, sizeof tg - 1); CHECK(n == (ssize_t)strlen(f2));
    unlink(ln); CHECK(symlink(ln, ln) == 0); errno = 0; CHECK(open(ln, O_RDONLY) == -1 && errno == ELOOP); unlink(ln); unlink(f2);
    struct statfs sf; CHECK(statfs("/", &sf) == 0 && sf.f_type != 0);
    DIR *dp = opendir("/proc"); CHECK(dp != NULL); int cnt = 0; struct dirent *e; while ((e = readdir(dp))) cnt++; closedir(dp); CHECK(cnt > 5);
    char cw[PATH_MAX]; CHECK(getcwd(cw, sizeof cw) != NULL);
    /* privilege drop: a 0700 root-owned directory is inaccessible to uid 1000 */
    CHECK(mkdir("/tmp/aether-priv", 0700) == 0);
    pid_t p = fork(); CHECK(p >= 0);
    if (p == 0) { if (setgid(1000) || setuid(1000)) _exit(2); _exit(opendir("/tmp/aether-priv") == NULL && errno == EACCES ? 0 : 3); }
    int s; CHECK(waitpid(p, &s, 0) == p && WIFEXITED(s) && WEXITSTATUS(s) == 0); rmdir("/tmp/aether-priv");
    DONE("fs_paths");
}
