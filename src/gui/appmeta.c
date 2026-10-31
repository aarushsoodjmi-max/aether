#define _GNU_SOURCE
#include "appmeta.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void copy(char *dst, size_t n, const char *v) { snprintf(dst, n, "%s", v); }
int app_parse(const char *text, AppEntry *out)
{
    memset(out, 0, sizeof *out);
    char *copyt = strdup(text); if (!copyt) return -1;
    int in_section = 0, saw_header = 0; char *save = NULL;
    for (char *l = strtok_r(copyt, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        while (*l == ' ' || *l == '\t') l++;
        size_t n = strlen(l); while (n && (l[n - 1] == '\r' || l[n - 1] == ' ')) l[--n] = 0;
        if (!*l || *l == '#') continue;
        if (*l == '[') { in_section = !strcmp(l, "[Aether App]"); if (in_section) saw_header = 1; continue; }
        if (!in_section) continue;
        char *eq = strchr(l, '='); if (!eq) continue; *eq = 0;
        if (!strcmp(l, "name")) copy(out->name, sizeof out->name, eq + 1);
        else if (!strcmp(l, "comment")) copy(out->comment, sizeof out->comment, eq + 1);
        else if (!strcmp(l, "exec")) copy(out->exec, sizeof out->exec, eq + 1);
        else if (!strcmp(l, "icon")) copy(out->icon, sizeof out->icon, eq + 1);
    }
    free(copyt);
    if (!saw_header || !out->name[0] || out->exec[0] != '/') return -1;
    return 0;
}
static int cmp(const void *a, const void *b) { return strcmp(((const AppEntry *)a)->name, ((const AppEntry *)b)->name); }
size_t app_load_dir(const char *dir, AppEntry *out, size_t max)
{
    DIR *d = opendir(dir); if (!d) return 0; size_t n = 0; struct dirent *e;
    while (n < max && (e = readdir(d))) {
        size_t l = strlen(e->d_name); if (l < 5 || strcmp(e->d_name + l - 4, ".app")) continue;
        char p[512]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); FILE *f = fopen(p, "r"); if (!f) continue;
        char buf[2048]; size_t got = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[got] = 0;
        if (app_parse(buf, &out[n]) == 0) n++;
    }
    closedir(d); if (n > 1) qsort(out, n, sizeof *out, cmp); return n;
}
int app_runnable(const AppEntry *a) { return a && a->exec[0] == '/' && access(a->exec, X_OK) == 0; }
size_t app_load_runnable(const char *dir, AppEntry *out, size_t max)
{
    size_t n = app_load_dir(dir, out, max), k = 0;
    for (size_t i = 0; i < n; i++) if (app_runnable(&out[i])) { if (k != i) out[k] = out[i]; k++; }
    return k;
}
