#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

int main(int argc, char **argv)
{
    if (argc != 2 || !argv[1][0]) {
        fprintf(stderr, "Usage: service-status <service>\n");
        return 2;
    }

    for (const char *p = argv[1]; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
            fprintf(stderr, "Invalid service name\n");
            return 2;
        }
    }

    char path[128];
    int n = snprintf(path, sizeof(path), "/run/aether-%s.pid", argv[1]);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        fprintf(stderr, "Service name too long\n");
        return 2;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("%s: not running (PID file missing)\n", argv[1]);
        return 1;
    }

    long pid;
    int valid = fscanf(f, "%ld", &pid) == 1 &&
                pid > 1 && pid <= INT_MAX;
    fclose(f);

    if (!valid) {
        printf("%s: invalid PID file\n", argv[1]);
        return 1;
    }

    if (kill((pid_t)pid, 0) != 0 && errno != EPERM) {
        printf("%s: not running (stale PID %ld)\n", argv[1], pid);
        return 1;
    }

    n = snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        fprintf(stderr, "PID path too long\n");
        return 2;
    }

    f = fopen(path, "r");
    if (!f) {
        printf("%s: cannot verify process state\n", argv[1]);
        return 1;
    }

    char line[512];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        printf("%s: cannot read process state\n", argv[1]);
        return 1;
    }
    fclose(f);

    char *end = strrchr(line, ')');
    if (!end || end[1] != ' ' || end[2] == '\0') {
        printf("%s: invalid process state\n", argv[1]);
        return 1;
    }

    char state = end[2];
    if (state == 'Z' || state == 'X') {
        printf("%s: not running (PID %ld, state %c)\n",
               argv[1], pid, state);
        return 1;
    }

    printf("%s: running (PID %ld, state %c)\n", argv[1], pid, state);
    return 0;
}
