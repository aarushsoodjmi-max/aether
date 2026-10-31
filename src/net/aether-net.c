/*
 * aether-net - minimal network bring-up for Aether.
 *
 *   aether-net up [DIR]      configure lo and every .conf file in DIR (default
 *                            /etc/aether/network)
 *   aether-net check [DIR]   validate configuration only
 *   aether-net status        list interfaces (state, IPv4 address)
 *
 * Interface file (KEY=VALUE, '#' comments):
 *   IFACE=eth0                  required, name as in the kernel (<= 15 chars)
 *   ADDRESS=10.0.2.15/24        static IPv4 address with prefix length
 *   GATEWAY=10.0.2.2            optional default route (needs ADDRESS)
 *   DNS=10.0.2.3,1.1.1.1        optional, up to 3, written to $AETHER_RESOLV_CONF
 *                               (default /etc/resolv.conf)
 *   DHCP=yes                    run "udhcpc" instead of ADDRESS/GATEWAY
 * Uses ioctl(2) directly; no dependency on iproute2/ifconfig.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/route.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>

typedef struct {
    char iface[IFNAMSIZ];
    struct in_addr addr, mask, gw, dns[3];
    int has_addr, has_gw, n_dns, dhcp;
} NetCfg;

static int valid_iface(const char *s)
{
    size_t n = strlen(s);

    if (n == 0 || n >= IFNAMSIZ)
        return 0;
    for (; *s; s++)
        if (*s == '/' || *s == ' ' || *s == ':' || (unsigned char)*s < 0x21)
            return 0;
    return 1;
}

static int prefix_to_mask(unsigned long len, struct in_addr *mask)
{
    if (len > 32)
        return -1;
    mask->s_addr = len == 0 ? 0 : htonl(~0u << (32 - len));
    return 0;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static int load_cfg(const char *path, NetCfg *c)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int ln = 0, seen_iface = 0;

    if (!f) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); return -1; }
    memset(c, 0, sizeof(*c));

    while (fgets(line, sizeof(line), f)) {
        ln++;
        if (!strchr(line, '\n') && !feof(f)) goto bad_line;
        char *k = trim(line);
        if (!*k || *k == '#') continue;
        char *v = strchr(k, '=');
        if (!v) goto bad_line;
        *v++ = '\0';
        k = trim(k); v = trim(v);

        if (!strcmp(k, "IFACE")) {
            if (seen_iface++ || !valid_iface(v)) goto bad_line;
            snprintf(c->iface, sizeof(c->iface), "%s", v);
        } else if (!strcmp(k, "ADDRESS")) {
            char *slash = strchr(v, '/');
            char *end;
            if (c->has_addr || !slash) goto bad_line;
            *slash++ = '\0';
            unsigned long len = strtoul(slash, &end, 10);
            if (*slash == '\0' || *end || inet_pton(AF_INET, v, &c->addr) != 1 ||
                prefix_to_mask(len, &c->mask) < 0) goto bad_line;
            c->has_addr = 1;
        } else if (!strcmp(k, "GATEWAY")) {
            if (c->has_gw || inet_pton(AF_INET, v, &c->gw) != 1) goto bad_line;
            c->has_gw = 1;
        } else if (!strcmp(k, "DNS")) {
            char *save = NULL;
            if (c->n_dns) goto bad_line;
            for (char *t = strtok_r(v, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
                if (c->n_dns >= 3 || inet_pton(AF_INET, trim(t), &c->dns[c->n_dns]) != 1)
                    goto bad_line;
                c->n_dns++;
            }
            if (!c->n_dns) goto bad_line;
        } else if (!strcmp(k, "DHCP")) {
            if (!strcmp(v, "yes")) c->dhcp = 1;
            else if (strcmp(v, "no")) goto bad_line;
        } else {
            goto bad_line;
        }
        continue;
bad_line:
        fprintf(stderr, "%s:%d: invalid or duplicate setting\n", path, ln);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (!seen_iface) { fprintf(stderr, "%s: IFACE is required\n", path); return -1; }
    if (c->dhcp && (c->has_addr || c->has_gw)) {
        fprintf(stderr, "%s: DHCP=yes conflicts with ADDRESS/GATEWAY\n", path);
        return -1;
    }
    if (c->has_gw && !c->has_addr) {
        fprintf(stderr, "%s: GATEWAY requires ADDRESS\n", path);
        return -1;
    }
    if (c->has_gw && c->has_addr &&
        ((c->gw.s_addr ^ c->addr.s_addr) & c->mask.s_addr) != 0) {
        fprintf(stderr, "%s: GATEWAY is not on the ADDRESS subnet\n", path);
        return -1;
    }
    if (!c->dhcp && !c->has_addr) {
        fprintf(stderr, "%s: need ADDRESS or DHCP=yes\n", path);
        return -1;
    }
    return 0;
}

static void set_sin(struct sockaddr *sa, struct in_addr a)
{
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr = a;
    memcpy(sa, &sin, sizeof(sin));
}

static int iface_up(int s, const char *name)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) return -1;
    ifr.ifr_flags = (short)(ifr.ifr_flags | IFF_UP | IFF_RUNNING);
    return ioctl(s, SIOCSIFFLAGS, &ifr);
}

static int iface_addr(int s, const char *name, struct in_addr a, struct in_addr m)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    set_sin(&ifr.ifr_addr, a);
    if (ioctl(s, SIOCSIFADDR, &ifr) < 0) return -1;
    set_sin(&ifr.ifr_netmask, m);
    return ioctl(s, SIOCSIFNETMASK, &ifr);
}

static int add_default_route(int s, const char *name, struct in_addr gw)
{
    struct rtentry rt;
    struct in_addr zero = { 0 };
    char dev[IFNAMSIZ];

    memset(&rt, 0, sizeof(rt));
    set_sin(&rt.rt_dst, zero);
    set_sin(&rt.rt_genmask, zero);
    set_sin(&rt.rt_gateway, gw);
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    snprintf(dev, sizeof(dev), "%s", name);
    rt.rt_dev = dev;
    if (ioctl(s, SIOCADDRT, &rt) < 0 && errno != EEXIST) return -1;
    return 0;
}

static int write_resolv(const NetCfg *c)
{
    const char *path = getenv("AETHER_RESOLV_CONF");
    if (!path) path = "/etc/resolv.conf";
    FILE *f = fopen(path, "w");
    char buf[INET_ADDRSTRLEN];

    if (!f) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); return -1; }
    for (int i = 0; i < c->n_dns; i++)
        fprintf(f, "nameserver %s\n", inet_ntop(AF_INET, &c->dns[i], buf, sizeof(buf)));
    return fclose(f);
}

static int run_dhcp(const char *iface)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/udhcpc", "udhcpc", "-i", iface, "-n", "-q", "-s",
              "/etc/aether/udhcpc.script", (char *)NULL);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int dir_configs(const char *dir, NetCfg **out, int *n)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    char *names[64];
    int cnt = 0, rc = 0;

    *n = 0;
    *out = NULL;
    if (!d) {
        if (errno == ENOENT) return 0;     /* no network config: lo only */
        fprintf(stderr, "%s: %s\n", dir, strerror(errno));
        return -1;
    }
    while ((de = readdir(d)) != NULL && cnt < 64) {
        size_t l = strlen(de->d_name);
        if (l > 5 && !strcmp(de->d_name + l - 5, ".conf"))
            names[cnt++] = strdup(de->d_name);
    }
    closedir(d);
    for (int i = 0; i < cnt; i++)         /* sort for determinism */
        for (int j = i + 1; j < cnt; j++)
            if (strcmp(names[i], names[j]) > 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; }

    *out = calloc(65, sizeof(NetCfg));
    for (int i = 0; i < cnt; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        NetCfg c;
        if (load_cfg(path, &c) < 0) { rc = -1; free(names[i]); continue; }
        for (int j = 0; j < *n; j++)
            if (!strcmp((*out)[j].iface, c.iface)) {
                fprintf(stderr, "%s: interface %s configured twice\n", path, c.iface);
                rc = -1;
            }
        (*out)[(*n)++] = c;
        free(names[i]);
    }
    return rc;
}

static int cmd_up(const char *dir, int check_only)
{
    NetCfg *cfgs; int n;
    int rc = dir_configs(dir, &cfgs, &n);

    if (check_only) {
        printf("check: %d interface(s), %s\n", n, rc ? "FAILED" : "ok");
        return rc ? 1 : 0;
    }
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) { perror("socket"); return 1; }

    struct in_addr lo = { htonl(INADDR_LOOPBACK) }, lomask = { htonl(0xff000000u) };
    if (iface_addr(s, "lo", lo, lomask) < 0 || iface_up(s, "lo") < 0) {
        fprintf(stderr, "lo: %s\n", strerror(errno));
        rc = -1;
    } else {
        printf("lo: up 127.0.0.1/8\n");
    }

    for (int i = 0; i < n; i++) {
        NetCfg *c = &cfgs[i];
        if (iface_up(s, c->iface) < 0) {
            fprintf(stderr, "%s: cannot bring up: %s\n", c->iface, strerror(errno));
            rc = -1;
            continue;
        }
        if (c->dhcp) {
            if (run_dhcp(c->iface) < 0) { fprintf(stderr, "%s: DHCP failed\n", c->iface); rc = -1; }
            else printf("%s: up (dhcp)\n", c->iface);
            continue;
        }
        if (iface_addr(s, c->iface, c->addr, c->mask) < 0) {
            fprintf(stderr, "%s: cannot set address: %s\n", c->iface, strerror(errno));
            rc = -1;
            continue;
        }
        if (c->has_gw && add_default_route(s, c->iface, c->gw) < 0) {
            fprintf(stderr, "%s: cannot add default route: %s\n", c->iface, strerror(errno));
            rc = -1;
        }
        if (c->n_dns && write_resolv(c) < 0) rc = -1;
        char a[INET_ADDRSTRLEN];
        printf("%s: up %s/%d\n", c->iface, inet_ntop(AF_INET, &c->addr, a, sizeof(a)),
               __builtin_popcount(ntohl(c->mask.s_addr)));
    }
    close(s);
    return rc ? 1 : 0;
}

static int cmd_status(void)
{
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) < 0) { perror("getifaddrs"); return 1; }
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        char a[INET_ADDRSTRLEN];
        struct sockaddr_in sin;
        memcpy(&sin, p->ifa_addr, sizeof(sin));
        printf("%s: %s %s %s\n", p->ifa_name, (p->ifa_flags & IFF_UP) ? "up" : "down",
               inet_ntop(AF_INET, &sin.sin_addr, a, sizeof(a)),
               (p->ifa_flags & IFF_LOOPBACK) ? "loopback" : "");
    }
    freeifaddrs(ifa);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = "/etc/aether/network";
    const char *slash = strrchr(argv[0], '/');

    /* Installed as /bin/aether-net-up: the service entry point (no arguments). */
    if (argc == 1 && !strcmp(slash ? slash + 1 : argv[0], "aether-net-up"))
        return cmd_up(dir, 0);

    if (argc >= 2 && !strcmp(argv[1], "status") && argc == 2) return cmd_status();
    if (argc >= 2 && argc <= 3 && (!strcmp(argv[1], "up") || !strcmp(argv[1], "check")))
        return cmd_up(argc == 3 ? argv[2] : dir, !strcmp(argv[1], "check"));
    fprintf(stderr, "Usage: aether-net up|check [DIR] | status\n");
    return 2;
}
