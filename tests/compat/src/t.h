/* Tiny assertion helpers for the Aether ELF-compat test programs. */
#ifndef AETHER_T_H
#define AETHER_T_H
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "CHECK FAILED %s:%d: %s (errno=%d %s)\n", __FILE__, __LINE__, #cond, errno, strerror(errno)); \
    exit(1); } } while (0)
#define DONE(name) do { printf("OK %s\n", name); fflush(stdout); return 0; } while (0)
#endif
