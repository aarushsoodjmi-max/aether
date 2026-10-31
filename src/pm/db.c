#define _GNU_SOURCE
#include "db.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *db_meta_path(const char *name) { return xsprintf(PM_DB_DIR "/db/%s.meta", name); }

int db_load(const char *root, Installed *in, char *err)
{
    in->pkgs = NULL; in->n = 0;
    char *dir = xsprintf("%s/" PM_DB_DIR "/db", root); DIR *d = opendir(dir);
    if (!d) { int e = errno; free(dir); if (e == ENOENT) return 0; set_err(err, "package database: %s", strerror(e)); return -1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t nl = strlen(e->d_name);
        if (e->d_name[0] == '.' || nl < 6 || strcmp(e->d_name + nl - 5, ".meta") != 0) continue;   /* ignores staging leftovers */
        char name[80]; if (nl - 5 >= sizeof name) { set_err(err, "package database contains an oversized entry"); goto fail; }
        memcpy(name, e->d_name, nl - 5); name[nl - 5] = 0;
        if (!valid_pkg_name(name)) { set_err(err, "package database contains an invalid entry '%s'", e->d_name); goto fail; }
        char *mp = xsprintf("%s/%s", dir, e->d_name); size_t len; char perr[PM_ERRLEN];
        char *txt = read_file(mp, &len, PKG_META_MAX, perr); free(mp);
        if (!txt) { set_err(err, "package database entry %s unreadable: %s (run 'aether-pm recover')", name, perr); goto fail; }
        Meta m;
        if (meta_parse(txt, len, &m, 1, perr) != 0) { set_err(err, "package database entry %s is corrupt: %s", name, perr); free(txt); goto fail; }
        free(txt);
        if (strcmp(m.name, name) != 0) { set_err(err, "package database entry %s names package %s", name, m.name); meta_free(&m); goto fail; }
        in->pkgs = xrealloc(in->pkgs, (in->n + 1) * sizeof *in->pkgs); in->pkgs[in->n++] = m;
    }
    closedir(d); free(dir); return 0;
fail:
    closedir(d); free(dir); db_free(in); return -1;
}
void db_free(Installed *in) { for (size_t i = 0; i < in->n; i++) meta_free(&in->pkgs[i]); free(in->pkgs); in->pkgs = NULL; in->n = 0; }
const Meta *db_find(const Installed *in, const char *name) { for (size_t i = 0; i < in->n; i++) if (!strcmp(in->pkgs[i].name, name)) return &in->pkgs[i]; return NULL; }
const Meta *db_owner(const Installed *in, const char *path, const Entry **entry)
{
    for (size_t i = 0; i < in->n; i++) for (size_t j = 0; j < in->pkgs[i].nentries; j++) if (!strcmp(in->pkgs[i].entries[j].path, path)) { if (entry) *entry = &in->pkgs[i].entries[j]; return &in->pkgs[i]; }
    return NULL;
}

int db_world_load(const char *root, char ***names, size_t *n)
{
    *names = NULL; *n = 0; char *p = xsprintf("%s/" PM_DB_DIR "/world", root); char err[PM_ERRLEN];
    char *t = read_file(p, NULL, 1 << 20, err); free(p); if (!t) return 0;
    char *save = NULL;
    for (char *l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) if (valid_pkg_name(l)) { *names = xrealloc(*names, (*n + 1) * sizeof **names); (*names)[(*n)++] = xstrdup(l); }
    free(t); return 0;
}
char *db_world_serialize(char **names, size_t n)
{
    size_t cap = 1; for (size_t i = 0; i < n; i++) cap += strlen(names[i]) + 1;
    char *s = xmalloc(cap); s[0] = 0; for (size_t i = 0; i < n; i++) { strcat(s, names[i]); strcat(s, "\n"); } return s;
}
