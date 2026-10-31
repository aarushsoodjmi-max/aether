#define _GNU_SOURCE
#include "service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>

static int valid_service_name(const char *name)
{
    if (!name[0])
        return 0;

    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-'))
            return 0;
    }
    return 1;
}

int service_load(const char *path, AetherService *out)
{
    AetherService loaded;
    AetherService *service = &loaded;
    memset(&loaded, 0, sizeof(loaded));

    FILE *file = fopen(path, "r");
    if (!file) {
        perror(path);
        return -1;
    }

    char line[256];
    char name[64] = "";
    char program[128] = "";
    char requires[520] = "";
    int restart = 1;
    int max_restarts = 5;
    int seen_name = 0, seen_exec = 0;
    int seen_restart = 0, seen_max = 0, seen_requires = 0;
    int line_no = 0;
    int failed = 0;

    while (fgets(line, sizeof(line), file)) {
        line_no++;

        if (!strchr(line, '\n') && !feof(file)) {
            fprintf(stderr, "%s:%d: line too long\n", path, line_no);
            failed = 1;
            break;
        }

        line[strcspn(line, "\r\n")] = '\0';

        char *key = line;
        while (*key == ' ' || *key == '\t')
            key++;

        if (*key == '\0' || *key == '#')
            continue;

        char *value = strchr(key, '=');
        if (!value) {
            fprintf(stderr, "%s:%d: expected KEY=VALUE\n", path, line_no);
            failed = 1;
            break;
        }

        *value++ = '\0';

        char *key_end = key + strlen(key);
        while (key_end > key &&
               (key_end[-1] == ' ' || key_end[-1] == '\t'))
            *--key_end = '\0';

        while (*value == ' ' || *value == '\t')
            value++;

        char *value_end = value + strlen(value);
        while (value_end > value &&
               (value_end[-1] == ' ' || value_end[-1] == '\t'))
            *--value_end = '\0';

        if (strcmp(key, "NAME") == 0) {
            if (seen_name++ || strlen(value) >= sizeof(name)) {
                failed = 1;
            } else {
                snprintf(name, sizeof(name), "%s", value);
            }
        } else if (strcmp(key, "EXEC") == 0) {
            if (seen_exec++ || strlen(value) >= sizeof(program)) {
                failed = 1;
            } else {
                snprintf(program, sizeof(program), "%s", value);
            }
        } else if (strcmp(key, "RESTART") == 0) {
            if (seen_restart++) {
                failed = 1;
            } else if (strcmp(value, "always") == 0) {
                restart = 1;
            } else if (strcmp(value, "never") == 0) {
                restart = 0;
            } else {
                failed = 1;
            }
        } else if (strcmp(key, "MAX_RESTARTS") == 0) {
            char *endptr;
            errno = 0;
            long parsed = strtol(value, &endptr, 10);

            if (seen_max++ || errno || *value == '\0' ||
                *endptr != '\0' || parsed < 0 || parsed > 100) {
                failed = 1;
            } else {
                max_restarts = (int)parsed;
            }
        } else if (strcmp(key, "REQUIRES") == 0) {
            if (seen_requires++ || strlen(value) >= sizeof(requires)) {
                failed = 1;
            } else {
                snprintf(requires, sizeof(requires), "%s", value);
            }
        } else {
            fprintf(stderr, "%s:%d: unknown setting '%s'\n",
                    path, line_no, key);
            failed = 1;
        }

        if (failed) {
            fprintf(stderr, "%s:%d: invalid or duplicate setting '%s'\n",
                    path, line_no, key);
            break;
        }
    }

    if (ferror(file)) {
        perror(path);
        failed = 1;
    }
    fclose(file);

    if (!seen_name || !seen_exec || !valid_service_name(name) ||
        program[0] != '/' || !program[1]) {
        fprintf(stderr, "%s: missing or invalid NAME/EXEC\n", path);
        failed = 1;
    }

    if (failed)
        return -1;

    if (requires[0]) {
        char *saveptr = NULL;
        char *dep = strtok_r(requires, ",", &saveptr);

        while (dep) {
            while (*dep == ' ' || *dep == '\t')
                dep++;
            char *dep_end = dep + strlen(dep);
            while (dep_end > dep &&
                   (dep_end[-1] == ' ' || dep_end[-1] == '\t'))
                *--dep_end = '\0';

            if (!valid_service_name(dep) ||
                strlen(dep) > SERVICE_NAME_MAX ||
                service->dependency_count >= SERVICE_DEPS_MAX ||
                strcmp(dep, name) == 0) {
                fprintf(stderr, "%s: invalid REQUIRES dependency\n", path);
                return -1;
            }

            for (int i = 0; i < service->dependency_count; i++) {
                if (strcmp(service->dependency_storage[i], dep) == 0) {
                    fprintf(stderr, "%s: duplicate REQUIRES dependency\n",
                            path);
                    return -1;
                }
            }

            snprintf(service->dependency_storage[service->dependency_count],
                     sizeof(service->dependency_storage[0]), "%s", dep);
            service->dependency_count++;
            dep = strtok_r(NULL, ",", &saveptr);
        }
    }

    snprintf(service->name_storage, sizeof(service->name_storage), "%s", name);
    snprintf(service->program_storage, sizeof(service->program_storage),
             "%s", program);

    service->name = service->name_storage;
    service->program = service->program_storage;
    service->restart = restart;
    service->restart_count = 0;
    service->max_restarts = max_restarts;
    service->pid = -1;
    service->interactive = strcmp(name, "console") == 0;

    *out = loaded;
    out->name = out->name_storage;
    out->program = out->program_storage;
    return 0;
}

int service_start(AetherService *service)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("Aether: service fork");
        return -1;
    }

    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);

        if (!service->interactive) {
            if (chdir("/") != 0)
                _exit(126);

            if (freopen("/dev/null", "r", stdin) == NULL)
                _exit(126);

            if (freopen("/var/log/aether.log", "a", stdout) == NULL)
                _exit(126);

            if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0)
                _exit(126);
        }

        if (service->interactive) {
            execl("/bin/setsid", "setsid", "/bin/cttyhack",
                  service->program, (char *)NULL);
        } else {
            execl("/bin/setsid", "setsid",
                  service->program, (char *)NULL);
        }
        _exit(127);
    }

    service->pid = pid;
    service->started_ms = service_now_ms();
    service->restart_at_ms = 0;

    char pidfile[128];
    snprintf(pidfile, sizeof(pidfile),
             "/run/aether-%s.pid", service->name);

    FILE *f = fopen(pidfile, "w");
    if (!f) {
        perror("Aether: PID file");
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        service->pid = -1;
        return -1;
    }

    fprintf(f, "%d\n", (int)pid);
    fclose(f);

    printf("Aether: started %s (PID %d)\n",
           service->name, (int)pid);
    fflush(stdout);
    return 0;
}

int service_reap(AetherService *service)
{
    int status;
    pid_t result = waitpid(service->pid, &status, WNOHANG);

    if (result == 0 || (result < 0 && errno == EINTR))
        return 0;

    if (result < 0 && errno != ECHILD)
        return -1;

    service->pid = -1;
    return 1;
}

long long service_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int service_load_dir(const char *dir, AetherService *services, int max,
                     int *console_index, int *rejected)
{
    DIR *d = opendir(dir);
    if (!d) {
        perror(dir);
        return -1;
    }

    char **names = NULL;
    size_t n_names = 0, cap = 0;
    struct dirent *entry;

    while ((entry = readdir(d)) != NULL) {
        size_t len = strlen(entry->d_name);

        if (len < 6 || strcmp(entry->d_name + len - 5, ".conf") != 0)
            continue;
        if (n_names == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            char **tmp = realloc(names, ncap * sizeof(*names));
            if (!tmp) {
                perror("Aether: realloc");
                goto fail;
            }
            names = tmp;
            cap = ncap;
        }
        names[n_names] = strdup(entry->d_name);
        if (!names[n_names]) {
            perror("Aether: strdup");
            goto fail;
        }
        n_names++;
    }
    closedir(d);
    d = NULL;

    qsort(names, n_names, sizeof(*names), name_cmp);

    int count = 0, skipped = 0;
    *console_index = -1;

    for (size_t i = 0; i < n_names; i++) {
        char path[512];
        int plen = snprintf(path, sizeof(path), "%s/%s", dir, names[i]);

        if (plen < 0 || (size_t)plen >= sizeof(path)) {
            fprintf(stderr, "Aether: service path too long; skipping %s\n",
                    names[i]);
            skipped++;
            continue;
        }

        if (count >= max) {
            fprintf(stderr, "Aether: service limit (%d) reached; "
                    "skipping %s\n", max, names[i]);
            skipped++;
            continue;
        }

        AetherService svc;
        if (service_load(path, &svc) < 0) {
            fprintf(stderr, "Aether: invalid service config: %s\n", names[i]);
            skipped++;
            continue;
        }

        int dup = 0;
        for (int j = 0; j < count; j++) {
            if (strcmp(services[j].name, svc.name) == 0)
                dup = 1;
        }
        if (dup) {
            fprintf(stderr, "Aether: duplicate service name '%s' in %s; "
                    "skipping\n", svc.name, names[i]);
            skipped++;
            continue;
        }

        if (strcmp(svc.name, "console") == 0)
            *console_index = count;

        services[count] = svc;
        services[count].name = services[count].name_storage;
        services[count].program = services[count].program_storage;
        count++;
    }

    for (size_t i = 0; i < n_names; i++)
        free(names[i]);
    free(names);
    if (rejected)
        *rejected = skipped;
    return count;

fail:
    for (size_t i = 0; i < n_names; i++)
        free(names[i]);
    free(names);
    if (d)
        closedir(d);
    return -1;
}
