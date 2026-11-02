#ifndef AETHER_CONFIG_H
#define AETHER_CONFIG_H

/*
 * Aether Core configuration.
 *
 * System configuration lives in /etc/aether/ as one small file per area:
 *
 *   system.conf    identity: HOSTNAME, SYSTEM_NAME
 *   boot.conf      boot behaviour: BANNER, MOUNT_FSTAB
 *   services.conf  service manager defaults (parsed and validated; init does
 *                  not consume them yet)
 *   logging.conf   logging policy (parsed and validated; not consumed yet)
 *
 * Format: KEY=VALUE, one per line. Blank lines are ignored; a line whose first
 * non-blank character is '#' is a comment ('#' elsewhere is part of the value).
 *
 * Guarantees:
 *   - Loading never fails and never aborts: a missing file, an unreadable or
 *     malformed file, or a bad value leaves the built-in default in place.
 *   - Bad lines are reported in an AetherConfigReport; the rest of the file is
 *     still used (per-setting fallback).
 *   - Nothing here ever modifies an existing file. aether_config_install_defaults()
 *     only creates files that do not exist yet.
 *   - The library prints nothing by itself except in aether_config_print_issues().
 */

#include <stdio.h>
#include <stddef.h>

#ifndef AETHER_CONFIG_DIR
#define AETHER_CONFIG_DIR "/etc/aether"
#endif

#define AETHER_HOSTNAME_MAX     63
#define AETHER_SYSNAME_MAX      64
#define AETHER_LOGFILE_MAX      127
#define AETHER_CONFIG_FILE_MAX  65536   /* larger files are ignored */
#define AETHER_CONFIG_LINE_MAX  255     /* longer lines are ignored */
#define AETHER_CONFIG_ISSUES_MAX 32     /* issues kept in a report */

enum {
    AETHER_SECTION_SYSTEM = 0,
    AETHER_SECTION_BOOT,
    AETHER_SECTION_SERVICES,
    AETHER_SECTION_LOGGING,
    AETHER_SECTION_COUNT
};

typedef enum {
    AETHER_LOG_ERROR = 0,
    AETHER_LOG_WARN,
    AETHER_LOG_INFO,
    AETHER_LOG_DEBUG
} AetherLogLevel;

typedef struct {
    char hostname[AETHER_HOSTNAME_MAX + 1];
    char system_name[AETHER_SYSNAME_MAX + 1];
} AetherSystemConfig;

typedef struct {
    int banner;        /* print the boot banner */
    int mount_fstab;   /* mount /etc/aether/fstab */
} AetherBootConfig;

typedef struct {
    int default_restart;        /* 1 = always, 0 = never */
    int default_max_restarts;   /* 0..100 */
    int stop_grace_ms;          /* SIGTERM -> SIGKILL grace, 100..60000 */
} AetherServicesConfig;

typedef struct {
    char file[AETHER_LOGFILE_MAX + 1];
    AetherLogLevel level;
    int max_size_kb;            /* 0 = no size limit */
    int rotate_keep;            /* rotated files kept, 0..20 */
} AetherLoggingConfig;

typedef struct {
    AetherSystemConfig system;
    AetherBootConfig boot;
    AetherServicesConfig services;
    AetherLoggingConfig logging;
} AetherConfig;

/* State of each configuration file after aether_config_load(). */
enum {
    AETHER_FILE_NOT_LOADED = 0,
    AETHER_FILE_MISSING,     /* normal: defaults apply */
    AETHER_FILE_LOADED,
    AETHER_FILE_UNUSABLE     /* present but unreadable/too big/binary: defaults apply */
};

typedef struct {
    char file[24];           /* "system.conf" */
    int line;                /* 1-based, 0 = whole file */
    char message[112];
} AetherConfigIssue;

typedef struct {
    int issue_count;         /* every issue found; may exceed the stored ones */
    int issue_stored;
    AetherConfigIssue issues[AETHER_CONFIG_ISSUES_MAX];
    int file_state[AETHER_SECTION_COUNT];
    int applied;             /* settings taken from files */
} AetherConfigReport;

/* Built-in defaults: identical to the behaviour before configuration existed. */
void aether_config_defaults(AetherConfig *cfg);

/*
 * Load every file from dir (NULL = AETHER_CONFIG_DIR) on top of the defaults.
 * cfg is always left fully valid. report is optional. Returns the number of
 * issues (0 = clean); it never returns an error and never exits.
 */
int aether_config_load(const char *dir, AetherConfig *cfg,
                       AetherConfigReport *report);

/*
 * Parse the text of one section file into cfg (text need not be
 * NUL-terminated). Settings that fail validation are skipped and reported.
 * report is optional and is accumulated into, not reset. Returns the number of
 * issues found by this call.
 */
int aether_config_parse(int section, const char *text, size_t len,
                        AetherConfig *cfg, AetherConfigReport *report);

/* Validators, usable by future Settings tools before they write anything. */
int aether_config_valid_hostname(const char *s);
int aether_config_valid_system_name(const char *s);
int aether_config_valid_log_file(const char *s);

const char *aether_config_section_name(int section);  /* "system" */
const char *aether_config_section_file(int section);  /* "system.conf" */

/* Effective configuration as "section.key=value\n" lines. Returns the length,
 * or -1 if buf is too small. */
int aether_config_format(const AetherConfig *cfg, char *buf, size_t size);

/* One value by dotted name ("system.hostname"). 0 on success, -1 if unknown
 * or buf is too small. */
int aether_config_get(const AetherConfig *cfg, const char *name,
                      char *buf, size_t size);

/* "file:line: message" lines, each preceded by prefix (may be NULL). Text
 * originating from files is sanitised, so it is safe for a terminal. */
void aether_config_print_issues(FILE *out, const AetherConfigReport *report,
                                const char *prefix);

enum {
    AETHER_INSTALL_CREATED = 0,
    AETHER_INSTALL_KEPT,      /* already exists: left untouched */
    AETHER_INSTALL_FAILED
};

/*
 * Create each missing section file in dir with its commented defaults.
 * Existing files (including symlinks) are never opened for writing, never
 * truncated and never replaced. dir is created (one level) if it is missing.
 * result[] receives one AETHER_INSTALL_* per section. Returns 0, or -1 if any
 * file or the directory could not be created.
 */
int aether_config_install_defaults(const char *dir,
                                   int result[AETHER_SECTION_COUNT]);

#endif
