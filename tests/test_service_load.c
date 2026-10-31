/* Unit tests: service configuration parsing and directory loading. */
#define _GNU_SOURCE
#include "../src/core/service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int failures;
static char dir[] = "/tmp/aether-load-XXXXXX";

#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s\n", name); failures++; } } while (0)

static void write_file(const char *name, const char *content)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f || fputs(content, f) == EOF) { perror(path); exit(2); }
    fclose(f);
}

static int load(const char *content, AetherService *s)
{
    write_file("t.conf", content);
    char path[256];
    snprintf(path, sizeof(path), "%s/t.conf", dir);
    return service_load(path, s);
}

static void rm_all(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf -- %s", dir);
    if (system(cmd) != 0) fprintf(stderr, "cleanup failed\n");
}

int main(void)
{
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    AetherService s;

    /* --- valid configurations --- */
    memset(&s, 0xAA, sizeof(s));
    CHECK(load("NAME=web\nEXEC=/bin/web\n", &s) == 0 &&
          strcmp(s.name, "web") == 0 && strcmp(s.program, "/bin/web") == 0 &&
          s.restart == 1 && s.max_restarts == 5 && s.pid == -1 &&
          s.dependency_count == 0 && !s.interactive,
          "minimal config with defaults (dependency_count clean)");

    CHECK(load("# c\n\n  NAME = web \r\nEXEC=/bin/web  \nRESTART=never\n"
               "MAX_RESTARTS=0\n", &s) == 0 &&
          s.restart == 0 && s.max_restarts == 0,
          "comments, blanks, whitespace and CRLF accepted");

    CHECK(load("NAME=console\nEXEC=/bin/sh\n", &s) == 0 && s.interactive,
          "console is interactive");

    CHECK(load("NAME=app\nEXEC=/bin/app\nREQUIRES=a, b ,c\n", &s) == 0 &&
          s.dependency_count == 3 && strcmp(s.dependency_storage[1], "b") == 0,
          "REQUIRES list with spaces");

    CHECK(load("NAME=app\nEXEC=/bin/app\nMAX_RESTARTS=100\n", &s) == 0 &&
          s.max_restarts == 100, "MAX_RESTARTS upper bound accepted");

    /* --- invalid input: must fail and leave *s untouched --- */
    AetherService keep;
    memset(&keep, 0x5A, sizeof(keep));
    s = keep;
    CHECK(load("NAME=app\nEXEC=/bin/app\nREQUIRES=a\nBOGUS=1\n", &s) < 0 &&
          memcmp(&s, &keep, sizeof(s)) == 0,
          "failed load leaves service untouched");

    CHECK(load("EXEC=/bin/app\n", &s) < 0, "missing NAME rejected");
    CHECK(load("NAME=app\n", &s) < 0, "missing EXEC rejected");
    CHECK(load("NAME=app\nEXEC=bin/app\n", &s) < 0, "relative EXEC rejected");
    CHECK(load("NAME=app\nEXEC=/\n", &s) < 0, "EXEC '/' rejected");
    CHECK(load("NAME=bad name\nEXEC=/bin/app\n", &s) < 0, "space in NAME rejected");
    CHECK(load("NAME=bad/name\nEXEC=/bin/app\n", &s) < 0, "slash in NAME rejected");
    CHECK(load("NAME=\nEXEC=/bin/app\n", &s) < 0, "empty NAME rejected");
    CHECK(load("NAME=a\nNAME=b\nEXEC=/bin/app\n", &s) < 0, "duplicate NAME rejected");
    CHECK(load("NAME=a\nEXEC=/bin/a\nRESTART=sometimes\n", &s) < 0, "bad RESTART rejected");
    CHECK(load("NAME=a\nEXEC=/bin/a\nMAX_RESTARTS=101\n", &s) < 0, "MAX_RESTARTS too big");
    CHECK(load("NAME=a\nEXEC=/bin/a\nMAX_RESTARTS=-1\n", &s) < 0, "MAX_RESTARTS negative");
    CHECK(load("NAME=a\nEXEC=/bin/a\nMAX_RESTARTS=3x\n", &s) < 0, "MAX_RESTARTS junk");
    CHECK(load("NAME=a\nEXEC=/bin/a\nMAX_RESTARTS=\n", &s) < 0, "MAX_RESTARTS empty");
    CHECK(load("NAME=a\nEXEC=/bin/a\nnoequals\n", &s) < 0, "line without '=' rejected");
    CHECK(load("NAME=a\nEXEC=/bin/a\nREQUIRES=a\n", &s) < 0, "self dependency rejected");
    CHECK(load("NAME=a\nEXEC=/bin/a\nREQUIRES=b,b\n", &s) < 0, "duplicate dependency rejected");
    CHECK(load("NAME=a\nEXEC=/bin/a\nREQUIRES=b/c\n", &s) < 0, "bad dependency name rejected");
    CHECK(load("NAME=a\nEXEC=/bin/a\nREQUIRES=1,2,3,4,5,6,7,8,9\n", &s) < 0,
          "more than SERVICE_DEPS_MAX dependencies rejected");

    char longline[400];
    memset(longline, 'x', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = '\0';
    char content[600];
    snprintf(content, sizeof(content), "NAME=a\nEXEC=/bin/a\n# %s\n", longline);
    CHECK(load(content, &s) < 0, "over-long line rejected");

    char longname[100];
    memset(longname, 'n', 70); longname[70] = '\0';
    snprintf(content, sizeof(content), "NAME=%s\nEXEC=/bin/a\n", longname);
    CHECK(load(content, &s) < 0, "over-long NAME rejected");

    char path[256];
    snprintf(path, sizeof(path), "%s/does-not-exist.conf", dir);
    CHECK(service_load(path, &s) < 0, "missing file rejected");

    /* --- directory loading --- */
    char sub[256];
    snprintf(sub, sizeof(sub), "%s/svc", dir);
    mkdir(sub, 0755);
    snprintf(path, sizeof(path), "%s/svc", dir);

#define WRITE_SVC(file, text) do { \
        char p_[300]; snprintf(p_, sizeof(p_), "svc/%s", file); \
        write_file(p_, text); } while (0)

    WRITE_SVC("20-b.conf", "NAME=bravo\nEXEC=/bin/b\n");
    WRITE_SVC("10-a.conf", "NAME=alpha\nEXEC=/bin/a\n");
    WRITE_SVC("console.conf", "NAME=console\nEXEC=/bin/sh\n");
    WRITE_SVC("notes.txt", "not a service\n");
    WRITE_SVC("broken.conf", "NAME=broken\n");
    WRITE_SVC("dup.conf", "NAME=alpha\nEXEC=/bin/other\n");

    AetherService list[MAX_SERVICES];
    memset(list, 0, sizeof(list));
    int console = -2, rejected = -1;
    int n = service_load_dir(path, list, MAX_SERVICES, &console, &rejected);
    CHECK(n == 3 && rejected == 2, "load_dir: 3 valid, 2 rejected (broken, duplicate)");
    CHECK(n == 3 && console >= 0 && strcmp(list[console].name, "console") == 0,
          "load_dir: console index found");
    CHECK(n == 3 && strcmp(list[0].name, "alpha") == 0 &&
          strcmp(list[0].program, "/bin/a") == 0,
          "load_dir: sorted by file name, first duplicate wins");

    memset(list, 0, sizeof(list));
    n = service_load_dir(path, list, 2, &console, &rejected);
    CHECK(n == 2, "load_dir: respects max");

    char missing[256];
    snprintf(missing, sizeof(missing), "%s/nope", dir);
    CHECK(service_load_dir(missing, list, MAX_SERVICES, &console, NULL) < 0,
          "load_dir: missing directory fails");

    rm_all();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
