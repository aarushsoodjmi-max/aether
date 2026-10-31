/* Installed-package database: var/lib/aether/pm/db/NAME.meta holds the exact signed-package metadata
 * (including the file manifest, which is the file-ownership record) of each installed package. */
#ifndef AETHER_PM_DB_H
#define AETHER_PM_DB_H
#include "fsops.h"

typedef struct { Meta *pkgs; size_t n; } Installed;
int db_load(const char *root, Installed *in, char *err);
void db_free(Installed *in);
const Meta *db_find(const Installed *in, const char *name);
const Meta *db_owner(const Installed *in, const char *path, const Entry **entry);   /* who owns this path? */

char *db_meta_path(const char *name);                                  /* "var/lib/aether/pm/db/NAME.meta" (malloc'd) */
int db_world_load(const char *root, char ***names, size_t *n);        /* explicitly requested packages */
char *db_world_serialize(char **names, size_t n);
#endif
