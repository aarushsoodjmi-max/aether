/* Unit tests: Aether Core configuration (parsing, validation, defaults,
 * loading, no-overwrite installation). */
#define _GNU_SOURCE
#include "../src/core/config.h"
#include "../src/core/service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int failures;
static char dir[] = "/tmp/aether-cfg-XXXXXX";

#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s\n", name); failures++; } } while (0)

static void path_of(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", dir, name);
}

static void write_file(const char *name, const char *content, size_t len)
{
    char path[256];
    path_of(path, sizeof(path), name);
    FILE *f = fopen(path, "wb");
    if (!f || (len && fwrite(content, 1, len, f) != len)) { perror(path); exit(2); }
    fclose(f);
}

static void put(const char *name, const char *content)
{
    write_file(name, content, strlen(content));
}

static int read_file(const char *name, char *buf, size_t n)
{
    char path[256];
    path_of(path, sizeof(path), name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, n - 1, f);
    buf[got] = '\0';
    fclose(f);
    return (int)got;
}

static void rm_all(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf -- %s", dir);
    if (system(cmd) != 0) fprintf(stderr, "cleanup failed\n");
}

static void clear_dir(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf -- %s/* %s/.[!.]*", dir, dir);
    if (system(cmd) != 0) fprintf(stderr, "clear failed\n");
}

/* Parse text into fresh defaults. */
static int parse(int section, const char *text, AetherConfig *cfg,
                 AetherConfigReport *rep)
{
    aether_config_defaults(cfg);
    memset(rep, 0, sizeof(*rep));
    return aether_config_parse(section, text, strlen(text), cfg, rep);
}

static int has_issue(const AetherConfigReport *r, const char *needle)
{
    for (int i = 0; i < r->issue_stored; i++)
        if (strstr(r->issues[i].message, needle))
            return 1;
    return 0;
}

static int same_defaults(const AetherConfig *a)
{
    AetherConfig d;
    aether_config_defaults(&d);
    return memcmp(a, &d, sizeof(d)) == 0;
}

static void test_defaults(void)
{
    AetherConfig c;
    aether_config_defaults(&c);

    CHECK(strcmp(c.system.hostname, "aether") == 0 &&
          strcmp(c.system.system_name, "Aether OS") == 0,
          "defaults: identity equals the previous hard-coded values");
    CHECK(c.boot.banner == 1 && c.boot.mount_fstab == 1,
          "defaults: banner and fstab on (previous behaviour)");
    CHECK(c.services.default_restart == 1 && c.services.default_max_restarts == 5 &&
          c.services.stop_grace_ms == 5000,
          "defaults: service manager values");
    CHECK(strcmp(c.logging.file, "/var/log/aether.log") == 0 &&
          c.logging.level == AETHER_LOG_INFO && c.logging.max_size_kb == 1024 &&
          c.logging.rotate_keep == 3,
          "defaults: logging values");

    /* The service manager's own defaults must agree with the config defaults. */
    put("svc.conf", "NAME=web\nEXEC=/bin/web\n");
    char p[256];
    AetherService s;
    path_of(p, sizeof(p), "svc.conf");
    CHECK(service_load(p, &s) == 0 &&
          s.restart == c.services.default_restart &&
          s.max_restarts == c.services.default_max_restarts,
          "defaults: match service.c defaults");

    aether_config_defaults(NULL);
    CHECK(1, "defaults: NULL is harmless");
}

static void test_validators(void)
{
    CHECK(aether_config_valid_hostname("aether") &&
          aether_config_valid_hostname("a") &&
          aether_config_valid_hostname("my-host.example.org") &&
          aether_config_valid_hostname("A1-b2"),
          "hostname: valid forms");

    char h63[64], h64[65];
    memset(h63, 'a', 63); h63[63] = '\0';
    memset(h64, 'a', 64); h64[64] = '\0';
    CHECK(aether_config_valid_hostname(h63) && !aether_config_valid_hostname(h64),
          "hostname: 63 ok, 64 rejected");
    CHECK(!aether_config_valid_hostname("") && !aether_config_valid_hostname(NULL) &&
          !aether_config_valid_hostname("-a") && !aether_config_valid_hostname("a-") &&
          !aether_config_valid_hostname(".a") && !aether_config_valid_hostname("a.") &&
          !aether_config_valid_hostname("a..b") && !aether_config_valid_hostname("a.-b") &&
          !aether_config_valid_hostname("a-.b") && !aether_config_valid_hostname("a b") &&
          !aether_config_valid_hostname("a_b") && !aether_config_valid_hostname("a/b") &&
          !aether_config_valid_hostname("h\xc3\xa9"),
          "hostname: bad forms rejected");

    char n64[65], n65[66];
    memset(n64, 'x', 64); n64[64] = '\0';
    memset(n65, 'x', 65); n65[65] = '\0';
    CHECK(aether_config_valid_system_name("Aether OS") &&
          aether_config_valid_system_name("A") &&
          aether_config_valid_system_name(n64) && !aether_config_valid_system_name(n65),
          "system name: valid and length limit");
    CHECK(!aether_config_valid_system_name("") && !aether_config_valid_system_name(NULL) &&
          !aether_config_valid_system_name(" x") && !aether_config_valid_system_name("x ") &&
          !aether_config_valid_system_name("a\tb") && !aether_config_valid_system_name("a\033b") &&
          !aether_config_valid_system_name("caf\xc3\xa9"),
          "system name: bad forms rejected");

    CHECK(aether_config_valid_log_file("/var/log/aether.log") &&
          aether_config_valid_log_file("/a") && aether_config_valid_log_file("/a/.hidden") &&
          aether_config_valid_log_file("/a/b..c"),
          "log file: valid paths");
    CHECK(!aether_config_valid_log_file("") && !aether_config_valid_log_file(NULL) &&
          !aether_config_valid_log_file("/") && !aether_config_valid_log_file("log") &&
          !aether_config_valid_log_file("/a/") && !aether_config_valid_log_file("//a") &&
          !aether_config_valid_log_file("/a//b") && !aether_config_valid_log_file("/a/./b") &&
          !aether_config_valid_log_file("/a/../b") && !aether_config_valid_log_file("/..") &&
          !aether_config_valid_log_file("/a b") && !aether_config_valid_log_file("/a\n"),
          "log file: bad paths rejected");
    char big[200];
    big[0] = '/';
    memset(big + 1, 'a', 127); big[128] = '\0';           /* 128 chars */
    CHECK(!aether_config_valid_log_file(big), "log file: over 127 characters rejected");
}

static void test_parse_valid(void)
{
    AetherConfig c;
    AetherConfigReport r;

    CHECK(parse(AETHER_SECTION_SYSTEM,
                "# identity\n\n  HOSTNAME = box1 \r\nSYSTEM_NAME=Aether Test #1\n",
                &c, &r) == 0 &&
          strcmp(c.system.hostname, "box1") == 0 &&
          strcmp(c.system.system_name, "Aether Test #1") == 0 && r.applied == 2,
          "parse: values, comments, blanks, spaces, CRLF; '#' inside a value is kept");

    CHECK(parse(AETHER_SECTION_SYSTEM, "HOSTNAME=last", &c, &r) == 0 &&
          strcmp(c.system.hostname, "last") == 0,
          "parse: last line without newline");

    CHECK(parse(AETHER_SECTION_BOOT, "BANNER=no\nMOUNT_FSTAB=no\n", &c, &r) == 0 &&
          c.boot.banner == 0 && c.boot.mount_fstab == 0,
          "parse: boot booleans");

    CHECK(parse(AETHER_SECTION_SERVICES,
                "DEFAULT_RESTART=never\nDEFAULT_MAX_RESTARTS=100\nSTOP_GRACE_MS=100\n",
                &c, &r) == 0 &&
          c.services.default_restart == 0 && c.services.default_max_restarts == 100 &&
          c.services.stop_grace_ms == 100,
          "parse: services values and lower/upper bounds");

    CHECK(parse(AETHER_SECTION_SERVICES, "DEFAULT_MAX_RESTARTS=0\nSTOP_GRACE_MS=60000\n",
                &c, &r) == 0 &&
          c.services.default_max_restarts == 0 && c.services.stop_grace_ms == 60000,
          "parse: services bounds (0 and 60000)");

    CHECK(parse(AETHER_SECTION_LOGGING,
                "FILE=/var/log/x.log\nLEVEL=debug\nMAX_SIZE_KB=0\nROTATE_KEEP=20\n",
                &c, &r) == 0 &&
          strcmp(c.logging.file, "/var/log/x.log") == 0 &&
          c.logging.level == AETHER_LOG_DEBUG && c.logging.max_size_kb == 0 &&
          c.logging.rotate_keep == 20,
          "parse: logging values");

    CHECK(parse(AETHER_SECTION_LOGGING, "LEVEL=error\nMAX_SIZE_KB=1048576\n", &c, &r) == 0 &&
          c.logging.level == AETHER_LOG_ERROR && c.logging.max_size_kb == 1048576,
          "parse: logging level error and max size bound");

    CHECK(parse(AETHER_SECTION_SYSTEM, "", &c, &r) == 0 && same_defaults(&c),
          "parse: empty text keeps defaults");
    CHECK(parse(AETHER_SECTION_SYSTEM, "# only a comment\n\n\n", &c, &r) == 0 &&
          same_defaults(&c),
          "parse: comments only keeps defaults");
}

static void test_parse_invalid(void)
{
    AetherConfig c;
    AetherConfigReport r;

    CHECK(parse(AETHER_SECTION_SYSTEM, "HOSTNAME=-bad\nSYSTEM_NAME=Custom\n", &c, &r) == 1 &&
          strcmp(c.system.hostname, "aether") == 0 &&
          strcmp(c.system.system_name, "Custom") == 0 &&
          r.issues[0].line == 1 && has_issue(&r, "HOSTNAME"),
          "invalid HOSTNAME keeps default; valid SYSTEM_NAME still applied (per-setting fallback)");

    CHECK(parse(AETHER_SECTION_SYSTEM, "HOSTNAME=\n", &c, &r) == 1 &&
          strcmp(c.system.hostname, "aether") == 0, "empty value rejected");
    CHECK(parse(AETHER_SECTION_SYSTEM, "SYSTEM_NAME=a\tb\n", &c, &r) == 1 &&
          strcmp(c.system.system_name, "Aether OS") == 0, "control char in name rejected");
    CHECK(parse(AETHER_SECTION_SYSTEM, "SYSTEM_NAME=caf\xc3\xa9\n", &c, &r) == 1 &&
          strcmp(c.system.system_name, "Aether OS") == 0, "non-ASCII name rejected");

    CHECK(parse(AETHER_SECTION_BOOT, "BANNER=true\n", &c, &r) == 1 && c.boot.banner == 1 &&
          parse(AETHER_SECTION_BOOT, "BANNER=YES\n", &c, &r) == 1 && c.boot.banner == 1 &&
          parse(AETHER_SECTION_BOOT, "MOUNT_FSTAB=0\n", &c, &r) == 1 && c.boot.mount_fstab == 1,
          "booleans: only yes/no accepted, default kept otherwise");

    CHECK(parse(AETHER_SECTION_SERVICES, "DEFAULT_RESTART=sometimes\n", &c, &r) == 1 &&
          c.services.default_restart == 1, "bad restart policy rejected");

    const char *bad_ints[] = { "-1", "+5", " ", "12x", "x", "1.5", "101", "99999999999",
                               "0x10", "1 0" };
    int all = 1;
    for (size_t i = 0; i < sizeof(bad_ints) / sizeof(bad_ints[0]); i++) {
        char text[64];
        snprintf(text, sizeof(text), "DEFAULT_MAX_RESTARTS=%s\n", bad_ints[i]);
        if (parse(AETHER_SECTION_SERVICES, text, &c, &r) != 1 ||
            c.services.default_max_restarts != 5)
            all = 0;
    }
    CHECK(all, "integers: sign, junk, float, hex, overflow and out-of-range rejected");

    CHECK(parse(AETHER_SECTION_SERVICES, "STOP_GRACE_MS=99\n", &c, &r) == 1 &&
          c.services.stop_grace_ms == 5000 &&
          parse(AETHER_SECTION_SERVICES, "STOP_GRACE_MS=60001\n", &c, &r) == 1 &&
          c.services.stop_grace_ms == 5000, "STOP_GRACE_MS range enforced");

    CHECK(parse(AETHER_SECTION_LOGGING, "FILE=relative.log\n", &c, &r) == 1 &&
          strcmp(c.logging.file, "/var/log/aether.log") == 0 &&
          parse(AETHER_SECTION_LOGGING, "LEVEL=trace\n", &c, &r) == 1 &&
          c.logging.level == AETHER_LOG_INFO &&
          parse(AETHER_SECTION_LOGGING, "ROTATE_KEEP=21\n", &c, &r) == 1 &&
          c.logging.rotate_keep == 3 &&
          parse(AETHER_SECTION_LOGGING, "MAX_SIZE_KB=1048577\n", &c, &r) == 1 &&
          c.logging.max_size_kb == 1024,
          "logging: bad path/level/ranges rejected, defaults kept");

    CHECK(parse(AETHER_SECTION_SYSTEM, "no equals sign\nHOSTNAME=ok\n", &c, &r) == 1 &&
          r.issues[0].line == 1 && strcmp(c.system.hostname, "ok") == 0,
          "line without '=' reported with its number; later lines still parsed");

    CHECK(parse(AETHER_SECTION_SYSTEM, "COLOR=blue\nHOSTNAME=ok\n", &c, &r) == 1 &&
          has_issue(&r, "unknown setting 'COLOR'") && strcmp(c.system.hostname, "ok") == 0,
          "unknown key reported and skipped");

    CHECK(parse(AETHER_SECTION_SYSTEM, "hostname=ok\n", &c, &r) == 1 && same_defaults(&c),
          "keys are case-sensitive");

    CHECK(parse(AETHER_SECTION_BOOT, "HOSTNAME=ok\n", &c, &r) == 1 && same_defaults(&c),
          "a key from another section is unknown here");

    CHECK(parse(AETHER_SECTION_SYSTEM, "HOSTNAME=first\nHOSTNAME=second\n", &c, &r) == 1 &&
          strcmp(c.system.hostname, "first") == 0 && r.issues[0].line == 2,
          "duplicate key: first value kept, duplicate reported");

    CHECK(parse(AETHER_SECTION_SYSTEM, "HOSTNAME=-bad\nHOSTNAME=good\n", &c, &r) == 2 &&
          strcmp(c.system.hostname, "aether") == 0,
          "duplicate after an invalid first value does not apply either");

    {
        char text[600];
        memset(text, 'a', sizeof(text));
        memcpy(text, "SYSTEM_NAME=", 12);
        text[300] = '\n';
        strcpy(text + 301, "HOSTNAME=after\n");
        aether_config_defaults(&c);
        memset(&r, 0, sizeof(r));
        int n = aether_config_parse(AETHER_SECTION_SYSTEM, text, 301 + 15, &c, &r);
        CHECK(n == 1 && has_issue(&r, "too long") && strcmp(c.system.hostname, "after") == 0 &&
              strcmp(c.system.system_name, "Aether OS") == 0,
              "over-long line ignored, parsing continues on the next line");
    }

    {
        aether_config_defaults(&c);
        memset(&r, 0, sizeof(r));
        const char text[] = "HOSTNAME=partial\n\0SYSTEM_NAME=x\n";
        int n = aether_config_parse(AETHER_SECTION_SYSTEM, text, sizeof(text) - 1, &c, &r);
        CHECK(n == 1 && has_issue(&r, "NUL") && same_defaults(&c),
              "NUL byte: whole file ignored, nothing applied");
    }

    {
        char issue_text[1024] = "";
        for (int i = 0; i < 40; i++)
            strcat(issue_text, "X=1\n");
        int n = parse(AETHER_SECTION_SYSTEM, issue_text, &c, &r);
        CHECK(n == 40 && r.issue_count == 40 &&
              r.issue_stored == AETHER_CONFIG_ISSUES_MAX,
              "report: every issue counted, storage bounded");
        char out[4096];
        FILE *f = fmemopen(out, sizeof(out), "w");
        aether_config_print_issues(f, &r, "P: ");
        fclose(f);
        CHECK(strstr(out, "P: system.conf:1: unknown setting 'X'") &&
              strstr(out, "and 8 more"),
              "report: printed with prefix, file:line and an overflow note");
    }

    {
        char text[] = "\033[31mEVIL=1\n";
        parse(AETHER_SECTION_SYSTEM, text, &c, &r);
        int clean = r.issue_stored == 1;
        for (const char *p = r.issues[0].message; *p; p++)
            if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e)
                clean = 0;
        CHECK(clean && strchr(r.issues[0].message, '?'),
              "issue messages never echo control characters from the file");
    }

    CHECK(aether_config_parse(AETHER_SECTION_SYSTEM, NULL, 0, &c, &r) == 0 &&
          aether_config_parse(99, "A=1", 3, &c, &r) == 0 &&
          aether_config_parse(AETHER_SECTION_SYSTEM, "A=1", 3, NULL, &r) == 0,
          "parse: NULL text/cfg and bad section are harmless");
    CHECK(aether_config_parse(AETHER_SECTION_SYSTEM, "A=1\n", 4, &c, NULL) == 1,
          "parse: report is optional");
}

static void test_load(void)
{
    AetherConfig c;
    AetherConfigReport r;
    char buf[AETHER_CONFIG_FILE_MAX + 100];

    /* missing directory and empty directory */
    CHECK(aether_config_load("/nonexistent/aether-cfg-dir", &c, &r) == 0 &&
          same_defaults(&c) && r.file_state[AETHER_SECTION_SYSTEM] == AETHER_FILE_MISSING &&
          r.file_state[AETHER_SECTION_LOGGING] == AETHER_FILE_MISSING,
          "load: missing directory -> defaults, no issues");
    CHECK(aether_config_load(dir, &c, &r) == 0 && same_defaults(&c) &&
          r.file_state[AETHER_SECTION_BOOT] == AETHER_FILE_MISSING,
          "load: empty directory -> defaults, no issues");

    /* full valid set */
    put("system.conf", "HOSTNAME=lab\nSYSTEM_NAME=Aether Lab\n");
    put("boot.conf", "BANNER=no\n");
    put("services.conf", "DEFAULT_MAX_RESTARTS=9\n");
    put("logging.conf", "LEVEL=warn\n");
    CHECK(aether_config_load(dir, &c, &r) == 0 &&
          strcmp(c.system.hostname, "lab") == 0 && c.boot.banner == 0 &&
          c.boot.mount_fstab == 1 && c.services.default_max_restarts == 9 &&
          c.logging.level == AETHER_LOG_WARN && r.applied == 5 &&
          r.file_state[AETHER_SECTION_SYSTEM] == AETHER_FILE_LOADED,
          "load: all four files applied on top of defaults");

    /* one broken file must not affect the others */
    put("system.conf", "HOSTNAME=!!!\n");
    CHECK(aether_config_load(dir, &c, &r) == 1 &&
          strcmp(c.system.hostname, "aether") == 0 && c.boot.banner == 0 &&
          c.logging.level == AETHER_LOG_WARN,
          "load: invalid file falls back to defaults, other files unaffected");

    /* unusable files: directory, oversized, NUL bytes, FIFO */
    clear_dir();
    char p[256];
    path_of(p, sizeof(p), "system.conf");
    mkdir(p, 0755);
    CHECK(aether_config_load(dir, &c, &r) == 1 && same_defaults(&c) &&
          r.file_state[AETHER_SECTION_SYSTEM] == AETHER_FILE_UNUSABLE,
          "load: a directory named system.conf is unusable, defaults kept");
    rmdir(p);

    memset(buf, '#', sizeof(buf));
    for (size_t i = 99; i < sizeof(buf); i += 100)
        buf[i] = '\n';                       /* comment lines of 99 characters */
    write_file("system.conf", buf, AETHER_CONFIG_FILE_MAX + 1);
    CHECK(aether_config_load(dir, &c, &r) == 1 && same_defaults(&c) &&
          has_issue(&r, "too large"),
          "load: file over the size limit is ignored");
    write_file("system.conf", buf, AETHER_CONFIG_FILE_MAX);
    CHECK(aether_config_load(dir, &c, &r) == 0,
          "load: file exactly at the size limit is accepted");

    write_file("system.conf", "HOSTNAME=x\0y\n", 13);
    CHECK(aether_config_load(dir, &c, &r) == 1 && same_defaults(&c),
          "load: binary file ignored");
    unlink(p);

    if (mkfifo(p, 0600) == 0) {
        CHECK(aether_config_load(dir, &c, &r) == 1 && same_defaults(&c) &&
              r.file_state[AETHER_SECTION_SYSTEM] == AETHER_FILE_UNUSABLE,
              "load: a FIFO is refused without blocking");
        unlink(p);
    }

    /* symlinks: valid target works, dangling is just missing */
    put("real.conf", "HOSTNAME=viasymlink\n");
    char real[256];
    path_of(real, sizeof(real), "real.conf");
    if (symlink(real, p) == 0) {
        CHECK(aether_config_load(dir, &c, &r) == 0 &&
              strcmp(c.system.hostname, "viasymlink") == 0,
              "load: symlinked config file is followed");
        unlink(p);
    }
    if (symlink("/nonexistent/target", p) == 0) {
        CHECK(aether_config_load(dir, &c, &r) == 0 && same_defaults(&c),
              "load: dangling symlink treated as missing");
        unlink(p);
    }

    /* robustness to bad arguments */
    CHECK(aether_config_load(dir, NULL, &r) == 0 && aether_config_load(dir, &c, NULL) == 0,
          "load: NULL cfg / NULL report are harmless");
}

static void test_format_get(void)
{
    AetherConfig c;
    char buf[2048], v[64];
    aether_config_defaults(&c);

    int n = aether_config_format(&c, buf, sizeof(buf));
    CHECK(n > 0 && strstr(buf, "system.hostname=aether\n") &&
          strstr(buf, "system.system_name=Aether OS\n") &&
          strstr(buf, "boot.banner=yes\n") && strstr(buf, "boot.mount_fstab=yes\n") &&
          strstr(buf, "services.default_restart=always\n") &&
          strstr(buf, "services.stop_grace_ms=5000\n") &&
          strstr(buf, "logging.file=/var/log/aether.log\n") &&
          strstr(buf, "logging.level=info\n") && strstr(buf, "logging.rotate_keep=3\n"),
          "format: effective configuration as section.key=value");
    CHECK(aether_config_format(&c, buf, 20) == -1 && aether_config_format(&c, buf, 0) == -1,
          "format: too-small buffer reported");

    CHECK(aether_config_get(&c, "system.hostname", v, sizeof(v)) == 0 &&
          strcmp(v, "aether") == 0 &&
          aether_config_get(&c, "SYSTEM.HostName", v, sizeof(v)) == 0 &&
          aether_config_get(&c, "boot.banner", v, sizeof(v)) == 0 && strcmp(v, "yes") == 0,
          "get: dotted names, case-insensitive");
    CHECK(aether_config_get(&c, "system.nope", v, sizeof(v)) == -1 &&
          aether_config_get(&c, "nope.hostname", v, sizeof(v)) == -1 &&
          aether_config_get(&c, "hostname", v, sizeof(v)) == -1 &&
          aether_config_get(&c, "system.hostname", v, 3) == -1 &&
          aether_config_get(&c, NULL, v, sizeof(v)) == -1,
          "get: unknown name, missing section, small buffer, NULL");
}

static int file_is(const char *name, const char *expect)
{
    char buf[4096];
    return read_file(name, buf, sizeof(buf)) >= 0 && strcmp(buf, expect) == 0;
}

static void test_install(void)
{
    int res[AETHER_SECTION_COUNT];
    AetherConfig c;
    AetherConfigReport r;
    char before[4096], after[4096];
    char p[256];
    struct stat st1, st2;

    clear_dir();
    CHECK(aether_config_install_defaults(dir, res) == 0 &&
          res[0] == AETHER_INSTALL_CREATED && res[1] == AETHER_INSTALL_CREATED &&
          res[2] == AETHER_INSTALL_CREATED && res[3] == AETHER_INSTALL_CREATED,
          "install: creates all four files in an empty directory");
    CHECK(aether_config_load(dir, &c, &r) == 0 && same_defaults(&c) &&
          r.file_state[0] == AETHER_FILE_LOADED && r.file_state[3] == AETHER_FILE_LOADED,
          "install: generated files are valid and equal the defaults");

    read_file("system.conf", before, sizeof(before));
    CHECK(strstr(before, "HOSTNAME=aether") && strstr(before, "SYSTEM_NAME=Aether OS") &&
          strstr(before, "# Host name:"),
          "install: files are commented and show the defaults");
    path_of(p, sizeof(p), "system.conf");
    stat(p, &st1);
    CHECK(S_ISREG(st1.st_mode) && (st1.st_mode & 0600) == 0600,
          "install: regular file, owner read/write");

    CHECK(aether_config_install_defaults(dir, res) == 0 &&
          res[0] == AETHER_INSTALL_KEPT && res[1] == AETHER_INSTALL_KEPT &&
          res[2] == AETHER_INSTALL_KEPT && res[3] == AETHER_INSTALL_KEPT,
          "install: second run keeps every file");
    read_file("system.conf", after, sizeof(after));
    stat(p, &st2);
    CHECK(strcmp(before, after) == 0 && st1.st_ino == st2.st_ino &&
          st1.st_mtime == st2.st_mtime,
          "install: second run leaves content, inode and mtime untouched");

    /* user-edited files survive, even invalid or empty ones */
    put("system.conf", "HOSTNAME=mine\n# my note\n");
    put("boot.conf", "this is not valid\n");
    write_file("logging.conf", "", 0);
    path_of(p, sizeof(p), "services.conf");
    unlink(p);
    CHECK(aether_config_install_defaults(dir, res) == 0 &&
          res[0] == AETHER_INSTALL_KEPT && res[1] == AETHER_INSTALL_KEPT &&
          res[2] == AETHER_INSTALL_CREATED && res[3] == AETHER_INSTALL_KEPT,
          "install: only the missing file is created");
    CHECK(file_is("system.conf", "HOSTNAME=mine\n# my note\n") &&
          file_is("boot.conf", "this is not valid\n") && file_is("logging.conf", ""),
          "install: edited, invalid and empty user files are byte-for-byte unchanged");

    /* symlinks are never written through */
    clear_dir();
    put("target.txt", "precious\n");
    char tgt[256], link[256];
    path_of(tgt, sizeof(tgt), "target.txt");
    path_of(link, sizeof(link), "system.conf");
    if (symlink(tgt, link) == 0) {
        path_of(link, sizeof(link), "boot.conf");
        if (symlink("/tmp/aether-cfg-dangling-should-not-exist", link) != 0)
            perror("symlink");
        CHECK(aether_config_install_defaults(dir, res) == 0 &&
              res[0] == AETHER_INSTALL_KEPT && res[1] == AETHER_INSTALL_KEPT &&
              file_is("target.txt", "precious\n") &&
              access("/tmp/aether-cfg-dangling-should-not-exist", F_OK) != 0,
              "install: existing and dangling symlinks are kept, never written through");
    }

    /* directory creation and failures */
    clear_dir();
    char sub[256];
    path_of(sub, sizeof(sub), "newdir");
    CHECK(aether_config_install_defaults(sub, res) == 0 && res[0] == AETHER_INSTALL_CREATED,
          "install: creates the directory when missing");
    char deep[300];
    snprintf(deep, sizeof(deep), "%s/a/b", dir);
    CHECK(aether_config_install_defaults(deep, res) == -1 &&
          res[0] == AETHER_INSTALL_FAILED,
          "install: missing parent directory fails cleanly");
    put("afile", "x");
    path_of(deep, sizeof(deep), "afile");
    CHECK(aether_config_install_defaults(deep, res) == -1 &&
          res[0] == AETHER_INSTALL_FAILED && file_is("afile", "x"),
          "install: target directory path that is a file fails, file untouched");
}

int main(void)
{
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }

    test_defaults();
    test_validators();
    test_parse_valid();
    test_parse_invalid();
    test_load();
    test_format_get();
    test_install();

    rm_all();
    if (failures) {
        printf("FAILED (%d failures)\n", failures);
        return 1;
    }
    printf("ALL PASSED (0 failures)\n");
    return 0;
}
