#define _GNU_SOURCE
#include "config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ---------------- small helpers ---------------- */

static int is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static int is_printable(char c)
{
    return c >= 0x20 && c <= 0x7e;
}

/* Copy text that came from a file, making it safe to print on a terminal. */
static void copy_safe(char *dst, size_t size, const char *src)
{
    size_t i = 0;

    if (size == 0)
        return;
    for (; src[i] && i + 1 < size; i++)
        dst[i] = is_printable(src[i]) ? src[i] : '?';
    dst[i] = '\0';
}

static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t' || *s == '\r')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        *--end = '\0';
    return s;
}

static void add_issue(AetherConfigReport *r, int section, int line,
                      const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static void add_issue(AetherConfigReport *r, int section, int line,
                      const char *fmt, ...)
{
    va_list ap;

    if (!r)
        return;
    r->issue_count++;
    if (r->issue_stored >= AETHER_CONFIG_ISSUES_MAX)
        return;

    AetherConfigIssue *is = &r->issues[r->issue_stored++];

    snprintf(is->file, sizeof(is->file), "%s", aether_config_section_file(section));
    is->line = line;
    va_start(ap, fmt);
    vsnprintf(is->message, sizeof(is->message), fmt, ap);
    va_end(ap);
}

/* Strict decimal: digits only, at most 9 of them, within [min, max]. */
static int parse_uint(const char *v, long min, long max, int *out,
                      char *err, size_t errsz)
{
    long n = 0;
    size_t len = strlen(v);

    if (len == 0 || len > 9) {
        snprintf(err, errsz, "expected a number from %ld to %ld", min, max);
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9') {
            snprintf(err, errsz, "expected a number from %ld to %ld", min, max);
            return -1;
        }
        n = n * 10 + (v[i] - '0');
    }
    if (n < min || n > max) {
        snprintf(err, errsz, "%ld is out of range (%ld to %ld)", n, min, max);
        return -1;
    }
    *out = (int)n;
    return 0;
}

static int parse_bool(const char *v, int *out, char *err, size_t errsz)
{
    if (strcmp(v, "yes") == 0) {
        *out = 1;
        return 0;
    }
    if (strcmp(v, "no") == 0) {
        *out = 0;
        return 0;
    }
    snprintf(err, errsz, "expected yes or no");
    return -1;
}

/* ---------------- validators ---------------- */

int aether_config_valid_hostname(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    size_t label = 0;
    char prev = 0;

    if (n == 0 || n > AETHER_HOSTNAME_MAX)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];

        if (c == '.') {
            if (label == 0 || prev == '-')
                return 0;
            label = 0;
        } else if (is_alnum(c)) {
            label++;
        } else if (c == '-') {
            if (label == 0)
                return 0;
            label++;
        } else {
            return 0;
        }
        prev = c;
    }
    return label > 0 && prev != '-';
}

int aether_config_valid_system_name(const char *s)
{
    size_t n = s ? strlen(s) : 0;

    if (n == 0 || n > AETHER_SYSNAME_MAX)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!is_printable(s[i]))
            return 0;
    return s[0] != ' ' && s[n - 1] != ' ';
}

int aether_config_valid_log_file(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    size_t comp = 0;          /* length of the current path component */
    int dots_only = 1;

    if (n < 2 || n > AETHER_LOGFILE_MAX || s[0] != '/')
        return 0;
    for (size_t i = 1; i <= n; i++) {
        char c = s[i];

        if (c == '/' || c == '\0') {
            if (comp == 0)                       /* "//" or trailing '/' */
                return 0;
            if (dots_only && comp <= 2)          /* "." and ".." */
                return 0;
            comp = 0;
            dots_only = 1;
        } else if (is_alnum(c) || c == '.' || c == '_' || c == '-') {
            comp++;
            if (c != '.')
                dots_only = 0;
        } else {
            return 0;
        }
    }
    return 1;
}

/* ---------------- settings table ---------------- */

typedef int (*parse_fn)(AetherConfig *, const char *, char *, size_t);
typedef void (*format_fn)(const AetherConfig *, char *, size_t);

typedef struct {
    int section;
    const char *key;
    parse_fn parse;
    format_fn format;
    const char *help;
} Setting;

static const char *const kSectionName[AETHER_SECTION_COUNT] = {
    "system", "boot", "services", "logging"
};
static const char *const kSectionFile[AETHER_SECTION_COUNT] = {
    "system.conf", "boot.conf", "services.conf", "logging.conf"
};
static const char *const kSectionTitle[AETHER_SECTION_COUNT] = {
    "System identity",
    "Boot behaviour",
    "Service manager defaults (read and validated; not used by init yet)",
    "Logging policy (read and validated; not used yet)"
};
static const char *const kLevelName[] = { "error", "warn", "info", "debug" };

static int set_hostname(AetherConfig *c, const char *v, char *err, size_t n)
{
    if (!aether_config_valid_hostname(v)) {
        snprintf(err, n, "must be 1-%d letters, digits, '-' or '.' "
                 "(labels may not start or end with '-')", AETHER_HOSTNAME_MAX);
        return -1;
    }
    snprintf(c->system.hostname, sizeof(c->system.hostname), "%s", v);
    return 0;
}

static void fmt_hostname(const AetherConfig *c, char *o, size_t n)
{
    snprintf(o, n, "%s", c->system.hostname);
}

static int set_system_name(AetherConfig *c, const char *v, char *err, size_t n)
{
    if (!aether_config_valid_system_name(v)) {
        snprintf(err, n, "must be 1-%d printable ASCII characters",
                 AETHER_SYSNAME_MAX);
        return -1;
    }
    snprintf(c->system.system_name, sizeof(c->system.system_name), "%s", v);
    return 0;
}

static void fmt_system_name(const AetherConfig *c, char *o, size_t n)
{
    snprintf(o, n, "%s", c->system.system_name);
}

static int set_banner(AetherConfig *c, const char *v, char *err, size_t n)
{
    return parse_bool(v, &c->boot.banner, err, n);
}

static void fmt_yesno(int value, char *o, size_t n)
{
    snprintf(o, n, "%s", value ? "yes" : "no");
}

static void fmt_banner(const AetherConfig *c, char *o, size_t n)
{
    fmt_yesno(c->boot.banner, o, n);
}

static int set_mount_fstab(AetherConfig *c, const char *v, char *err, size_t n)
{
    return parse_bool(v, &c->boot.mount_fstab, err, n);
}

static void fmt_mount_fstab(const AetherConfig *c, char *o, size_t n)
{
    fmt_yesno(c->boot.mount_fstab, o, n);
}

static int set_default_restart(AetherConfig *c, const char *v, char *err,
                               size_t n)
{
    if (strcmp(v, "always") == 0) {
        c->services.default_restart = 1;
    } else if (strcmp(v, "never") == 0) {
        c->services.default_restart = 0;
    } else {
        snprintf(err, n, "expected always or never");
        return -1;
    }
    return 0;
}

static void fmt_default_restart(const AetherConfig *c, char *o, size_t n)
{
    snprintf(o, n, "%s", c->services.default_restart ? "always" : "never");
}

static int set_default_max_restarts(AetherConfig *c, const char *v, char *err,
                                    size_t n)
{
    return parse_uint(v, 0, 100, &c->services.default_max_restarts, err, n);
}

static void fmt_default_max_restarts(const AetherConfig *c, char *o, size_t n)
{
    snprintf(o, n, "%d", c->services.default_max_restarts);
}

static int set_stop_grace(AetherConfig *c, const char *v, char *err, size_t n)
{
    return parse_uint(v, 100, 60000, &c->services.stop_grace_ms, err, n);
}

static void fmt_stop_grace(const AetherConfig *c, char *o, size_t n)
{
    snprintf(o, n, "%d", c->services.stop_grace_ms);
}

static int set_log_file(AetherConfig *c, const char *v, char *err, size_t n)
{
    if (!aether_config_valid_log_file(v)) {
        snprintf(err, n, "must be an absolute path of letters, digits, "
                 "'.', '_', '-' and '/' (no '.' or '..' components)");
        return -1;
    }
    snprintf(c->logging.file, sizeof(c->logging.file), "%s", v);
    return 0;
}

static void fmt_log_file(const AetherConfig *c, char *o, size_t n)
{
    snprintf(o, n, "%s", c->logging.file);
}

static int set_log_level(AetherConfig *c, const char *v, char *err, size_t n)
{
    for (int i = 0; i < 4; i++) {
        if (strcmp(v, kLevelName[i]) == 0) {
            c->logging.level = (AetherLogLevel)i;
            return 0;
        }
    }
    snprintf(err, n, "expected error, warn, info or debug");
    return -1;
}

static void fmt_log_level(const AetherConfig *c, char *o, size_t n)
{
    snprintf(o, n, "%s", kLevelName[c->logging.level]);
}

static int set_max_size(AetherConfig *c, const char *v, char *err, size_t n)
{
    return parse_uint(v, 0, 1048576, &c->logging.max_size_kb, err, n);
}

static void fmt_max_size(const AetherConfig *c, char *o, size_t n)
{
    snprintf(o, n, "%d", c->logging.max_size_kb);
}

static int set_rotate_keep(AetherConfig *c, const char *v, char *err, size_t n)
{
    return parse_uint(v, 0, 20, &c->logging.rotate_keep, err, n);
}

static void fmt_rotate_keep(const AetherConfig *c, char *o, size_t n)
{
    snprintf(o, n, "%d", c->logging.rotate_keep);
}

static const Setting kSettings[] = {
    { AETHER_SECTION_SYSTEM, "HOSTNAME", set_hostname, fmt_hostname,
      "Host name: letters, digits, '-' and '.', 1-63 characters." },
    { AETHER_SECTION_SYSTEM, "SYSTEM_NAME", set_system_name, fmt_system_name,
      "Product name shown at boot: 1-64 printable ASCII characters." },
    { AETHER_SECTION_BOOT, "BANNER", set_banner, fmt_banner,
      "Print the boot banner: yes or no." },
    { AETHER_SECTION_BOOT, "MOUNT_FSTAB", set_mount_fstab, fmt_mount_fstab,
      "Mount the filesystems listed in /etc/aether/fstab: yes or no." },
    { AETHER_SECTION_SERVICES, "DEFAULT_RESTART", set_default_restart,
      fmt_default_restart, "Default restart policy: always or never." },
    { AETHER_SECTION_SERVICES, "DEFAULT_MAX_RESTARTS", set_default_max_restarts,
      fmt_default_max_restarts, "Default restart budget: 0 to 100." },
    { AETHER_SECTION_SERVICES, "STOP_GRACE_MS", set_stop_grace, fmt_stop_grace,
      "Milliseconds between SIGTERM and SIGKILL: 100 to 60000." },
    { AETHER_SECTION_LOGGING, "FILE", set_log_file, fmt_log_file,
      "Log file: absolute path." },
    { AETHER_SECTION_LOGGING, "LEVEL", set_log_level, fmt_log_level,
      "Minimum level: error, warn, info or debug." },
    { AETHER_SECTION_LOGGING, "MAX_SIZE_KB", set_max_size, fmt_max_size,
      "Rotate at this size in KiB: 0 (no limit) to 1048576." },
    { AETHER_SECTION_LOGGING, "ROTATE_KEEP", set_rotate_keep, fmt_rotate_keep,
      "Rotated files to keep: 0 to 20." },
};
#define NSETTINGS (sizeof(kSettings) / sizeof(kSettings[0]))

const char *aether_config_section_name(int section)
{
    return (section >= 0 && section < AETHER_SECTION_COUNT)
               ? kSectionName[section] : "?";
}

const char *aether_config_section_file(int section)
{
    return (section >= 0 && section < AETHER_SECTION_COUNT)
               ? kSectionFile[section] : "?";
}

/* ---------------- defaults ---------------- */

void aether_config_defaults(AetherConfig *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->system.hostname, sizeof(cfg->system.hostname), "aether");
    snprintf(cfg->system.system_name, sizeof(cfg->system.system_name),
             "Aether OS");
    cfg->boot.banner = 1;
    cfg->boot.mount_fstab = 1;
    cfg->services.default_restart = 1;       /* service.c: RESTART=always */
    cfg->services.default_max_restarts = 5;  /* service.c: MAX_RESTARTS=5 */
    cfg->services.stop_grace_ms = 5000;      /* init.c: AETHER_GRACE_MS */
    snprintf(cfg->logging.file, sizeof(cfg->logging.file),
             "/var/log/aether.log");
    cfg->logging.level = AETHER_LOG_INFO;
    cfg->logging.max_size_kb = 1024;
    cfg->logging.rotate_keep = 3;
}

/* ---------------- parsing ---------------- */

int aether_config_parse(int section, const char *text, size_t len,
                        AetherConfig *cfg, AetherConfigReport *report)
{
    int issues = 0;
    int seen[NSETTINGS] = { 0 };
    size_t pos = 0;
    int lineno = 0;

    if (!cfg || !text || section < 0 || section >= AETHER_SECTION_COUNT)
        return 0;

#define ISSUE(line, ...) do { issues++; add_issue(report, section, (line), __VA_ARGS__); } while (0)

    if (memchr(text, '\0', len)) {
        ISSUE(0, "file contains a NUL byte; ignored, defaults kept");
        return issues;
    }

    while (pos < len) {
        const char *start = text + pos;
        const char *nl = memchr(start, '\n', len - pos);
        size_t linelen = nl ? (size_t)(nl - start) : len - pos;
        char line[AETHER_CONFIG_LINE_MAX + 1];
        char shown[40];

        pos += linelen + (nl ? 1 : 0);
        lineno++;

        if (linelen > AETHER_CONFIG_LINE_MAX) {
            ISSUE(lineno, "line too long (over %d bytes); ignored",
                  AETHER_CONFIG_LINE_MAX);
            continue;
        }
        memcpy(line, start, linelen);
        line[linelen] = '\0';

        char *p = trim(line);

        if (*p == '\0' || *p == '#')
            continue;

        char *eq = strchr(p, '=');

        if (!eq) {
            ISSUE(lineno, "expected KEY=VALUE; line ignored");
            continue;
        }
        *eq = '\0';

        char *key = trim(p);
        char *value = trim(eq + 1);
        size_t idx = NSETTINGS;

        for (size_t i = 0; i < NSETTINGS; i++) {
            if (kSettings[i].section == section &&
                strcmp(kSettings[i].key, key) == 0) {
                idx = i;
                break;
            }
        }
        if (idx == NSETTINGS) {
            copy_safe(shown, sizeof(shown), key);
            ISSUE(lineno, "unknown setting '%s'; ignored", shown);
            continue;
        }
        if (seen[idx]) {
            ISSUE(lineno, "%s given more than once; first value kept", key);
            continue;
        }
        seen[idx] = 1;

        char err[96] = "";

        if (kSettings[idx].parse(cfg, value, err, sizeof(err)) != 0) {
            ISSUE(lineno, "%s: %s; default kept", key, err);
            continue;
        }
        if (report)
            report->applied++;
    }
#undef ISSUE
    return issues;
}

/* ---------------- loading ---------------- */

enum { RF_OK = 0, RF_MISSING, RF_UNUSABLE };

/* Read a small regular file. Never blocks: non-regular files are refused. */
static int read_config_file(const char *dir, const char *name, char *buf,
                            size_t cap, size_t *len, const char **why)
{
    char path[256];
    struct stat st;
    size_t got = 0;
    int fd;

    *len = 0;
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
        *why = "path too long";
        return RF_UNUSABLE;
    }
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR)
            return RF_MISSING;
        *why = "cannot be opened";
        return RF_UNUSABLE;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        *why = "not a regular file";
        return RF_UNUSABLE;
    }
    if (st.st_size > (off_t)cap) {
        close(fd);
        *why = "file too large";
        return RF_UNUSABLE;
    }
    for (;;) {
        ssize_t n = read(fd, buf + got, cap + 1 - got);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            *why = "read error";
            return RF_UNUSABLE;
        }
        if (n == 0)
            break;
        got += (size_t)n;
        if (got > cap) {
            close(fd);
            *why = "file too large";
            return RF_UNUSABLE;
        }
    }
    close(fd);
    *len = got;
    return RF_OK;
}

int aether_config_load(const char *dir, AetherConfig *cfg,
                       AetherConfigReport *report)
{
    AetherConfigReport local;
    char *buf;
    int total;

    if (!cfg)
        return 0;
    aether_config_defaults(cfg);
    if (!report)
        report = &local;
    memset(report, 0, sizeof(*report));
    if (!dir)
        dir = AETHER_CONFIG_DIR;

    buf = malloc(AETHER_CONFIG_FILE_MAX + 1);
    if (!buf) {
        add_issue(report, AETHER_SECTION_SYSTEM, 0,
                  "out of memory; all defaults kept");
        return report->issue_count;
    }
    for (int s = 0; s < AETHER_SECTION_COUNT; s++) {
        const char *why = "";
        size_t len = 0;
        int rc = read_config_file(dir, kSectionFile[s], buf,
                                  AETHER_CONFIG_FILE_MAX, &len, &why);

        if (rc == RF_MISSING) {
            report->file_state[s] = AETHER_FILE_MISSING;
        } else if (rc == RF_UNUSABLE) {
            report->file_state[s] = AETHER_FILE_UNUSABLE;
            add_issue(report, s, 0, "%s; defaults kept", why);
        } else {
            report->file_state[s] = AETHER_FILE_LOADED;
            aether_config_parse(s, buf, len, cfg, report);
        }
    }
    free(buf);
    total = report->issue_count;
    return total;
}

/* ---------------- reporting ---------------- */

int aether_config_format(const AetherConfig *cfg, char *buf, size_t size)
{
    size_t used = 0;

    if (!cfg || !buf || size == 0)
        return -1;
    buf[0] = '\0';
    for (size_t i = 0; i < NSETTINGS; i++) {
        char value[AETHER_LOGFILE_MAX + 1];
        char key[48];
        size_t k = 0;
        int n;

        kSettings[i].format(cfg, value, sizeof(value));
        for (const char *p = kSectionName[kSettings[i].section]; *p; p++)
            key[k++] = *p;
        key[k++] = '.';
        for (const char *p = kSettings[i].key; *p && k + 1 < sizeof(key); p++)
            key[k++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
        key[k] = '\0';

        n = snprintf(buf + used, size - used, "%s=%s\n", key, value);
        if (n < 0 || (size_t)n >= size - used) {
            buf[used] = '\0';
            return -1;
        }
        used += (size_t)n;
    }
    return (int)used;
}

int aether_config_get(const AetherConfig *cfg, const char *name, char *buf,
                      size_t size)
{
    const char *dot;

    if (!cfg || !name || !buf || size == 0)
        return -1;
    dot = strchr(name, '.');
    if (!dot)
        return -1;
    for (size_t i = 0; i < NSETTINGS; i++) {
        const char *sec = kSectionName[kSettings[i].section];
        char value[AETHER_LOGFILE_MAX + 1];

        if ((size_t)(dot - name) != strlen(sec) ||
            strncasecmp(name, sec, strlen(sec)) != 0 ||
            strcasecmp(dot + 1, kSettings[i].key) != 0)
            continue;
        kSettings[i].format(cfg, value, sizeof(value));
        if (strlen(value) >= size)
            return -1;
        memcpy(buf, value, strlen(value) + 1);
        return 0;
    }
    return -1;
}

void aether_config_print_issues(FILE *out, const AetherConfigReport *r,
                                const char *prefix)
{
    if (!out || !r)
        return;
    if (!prefix)
        prefix = "";
    for (int i = 0; i < r->issue_stored; i++) {
        const AetherConfigIssue *is = &r->issues[i];

        if (is->line > 0)
            fprintf(out, "%s%s:%d: %s\n", prefix, is->file, is->line,
                    is->message);
        else
            fprintf(out, "%s%s: %s\n", prefix, is->file, is->message);
    }
    if (r->issue_count > r->issue_stored)
        fprintf(out, "%s... and %d more configuration problem(s)\n", prefix,
                r->issue_count - r->issue_stored);
}

/* ---------------- installing defaults (never overwrites) ---------------- */

static int default_text(int section, char *buf, size_t size)
{
    AetherConfig cfg;
    size_t used = 0;
    int n;

    aether_config_defaults(&cfg);
    n = snprintf(buf, size,
                 "# Aether OS %s configuration: /etc/aether/%s\n"
                 "# %s.\n"
                 "# One KEY=VALUE per line. '#' at the start of a line begins a comment.\n"
                 "# Missing or invalid settings fall back to the values shown here.\n",
                 kSectionName[section], kSectionFile[section],
                 kSectionTitle[section]);
    if (n < 0 || (size_t)n >= size)
        return -1;
    used = (size_t)n;

    for (size_t i = 0; i < NSETTINGS; i++) {
        char value[AETHER_LOGFILE_MAX + 1];

        if (kSettings[i].section != section)
            continue;
        kSettings[i].format(&cfg, value, sizeof(value));
        n = snprintf(buf + used, size - used, "\n# %s\n%s=%s\n",
                     kSettings[i].help, kSettings[i].key, value);
        if (n < 0 || (size_t)n >= size - used)
            return -1;
        used += (size_t)n;
    }
    return (int)used;
}

static int install_one(const char *dir, int section)
{
    char path[256];
    char text[2048];
    int len, fd;
    size_t off = 0;

    if (snprintf(path, sizeof(path), "%s/%s", dir, kSectionFile[section]) >=
        (int)sizeof(path))
        return AETHER_INSTALL_FAILED;
    len = default_text(section, text, sizeof(text));
    if (len < 0)
        return AETHER_INSTALL_FAILED;

    /* O_EXCL: fails if anything (file, directory, even a dangling symlink)
     * already exists at the path, so an existing file is never touched. */
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return errno == EEXIST ? AETHER_INSTALL_KEPT : AETHER_INSTALL_FAILED;

    while (off < (size_t)len) {
        ssize_t n = write(fd, text + off, (size_t)len - off);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            goto fail;
        off += (size_t)n;
    }
    if (fsync(fd) != 0)
        goto fail;
    if (close(fd) != 0) {
        unlink(path);                 /* our own half-written file */
        return AETHER_INSTALL_FAILED;
    }
    return AETHER_INSTALL_CREATED;

fail:
    close(fd);
    unlink(path);                     /* only a file this call just created */
    return AETHER_INSTALL_FAILED;
}

int aether_config_install_defaults(const char *dir,
                                   int result[AETHER_SECTION_COUNT])
{
    int rc = 0;

    if (!dir)
        dir = AETHER_CONFIG_DIR;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        for (int s = 0; s < AETHER_SECTION_COUNT; s++)
            if (result)
                result[s] = AETHER_INSTALL_FAILED;
        return -1;
    }
    for (int s = 0; s < AETHER_SECTION_COUNT; s++) {
        int r = install_one(dir, s);

        if (result)
            result[s] = r;
        if (r == AETHER_INSTALL_FAILED)
            rc = -1;
    }
    return rc;
}
