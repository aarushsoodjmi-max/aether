#ifndef AETHER_SERVICE_H
#define AETHER_SERVICE_H

#include <sys/types.h>

#define SERVICE_NAME_MAX 63
#define SERVICE_DEPS_MAX 8

#ifndef MAX_SERVICES
#define MAX_SERVICES 16
#endif

typedef struct {
    const char *name;
    pid_t pid;
    int restart;
    int restart_count;
    int max_restarts;
    const char *program;
    int dependency_count;
    char dependency_storage[SERVICE_DEPS_MAX][SERVICE_NAME_MAX + 1];
    char name_storage[64];
    char program_storage[128];
    /* Appended fields: zero-initialised structs remain valid. */
    int interactive;          /* owns the console: keeps stdio, gets a tty */
    long long started_ms;     /* CLOCK_MONOTONIC time of last start */
    long long restart_at_ms;  /* >0: restart pending at this time */
    int manual_stop;          /* stopped by the operator: do not restart */
} AetherService;

/* Load and validate one service file. On failure *service is untouched. */
int service_load(const char *path, AetherService *service);

/*
 * Load every valid *.conf in dir (sorted by file name, so the result is
 * deterministic). Invalid files are reported and skipped; duplicate service
 * names are rejected. Returns the number of services loaded, or -1 if the
 * directory cannot be read. *console_index is -1 if no console service.
 * *rejected (optional) receives the number of files skipped.
 */
int service_load_dir(const char *dir, AetherService *services, int max,
                     int *console_index, int *rejected);

/* Topologically order services; console is always last. */
int service_order(AetherService *services, int count, int *console_index);

int service_start(AetherService *service);
int service_reap(AetherService *service);

long long service_now_ms(void);

#endif
