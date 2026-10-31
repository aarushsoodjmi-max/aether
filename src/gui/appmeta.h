/* Desktop application metadata: /usr/share/aether/apps/NAME.app (independent of the package manager).
 *   [Aether App]
 *   name=Settings
 *   comment=System settings
 *   exec=/bin/aether-settings
 *   icon=settings            (optional: name of a built-in Aether glyph; otherwise the app's initial is shown)
 */
#ifndef AETHER_APPMETA_H
#define AETHER_APPMETA_H
#include <stddef.h>
typedef struct { char name[48]; char comment[96]; char exec[160]; char icon[32]; } AppEntry;
int app_parse(const char *text, AppEntry *out);                     /* 0 ok; -1 invalid (needs name and exec, absolute exec path) */
size_t app_load_dir(const char *dir, AppEntry *out, size_t max);    /* sorted by name; invalid files skipped */
int app_runnable(const AppEntry *a);                                /* exec exists and is executable */
size_t app_load_runnable(const char *dir, AppEntry *out, size_t max);   /* app_load_dir minus entries that cannot launch */
#endif
