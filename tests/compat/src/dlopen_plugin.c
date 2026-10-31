#define _GNU_SOURCE
#include "t.h"
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>
static int saw_libc;
static int cb(struct dl_phdr_info *i, size_t s, void *d) { (void)s; (void)d; if (strstr(i->dlpi_name, "libc.so.6")) saw_libc = 1; return 0; }
int main(void) {
    void *h = dlopen("libplugin.so", RTLD_NOW);                 /* found via RUNPATH $ORIGIN/../lib */
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    int *ran = dlsym(h, "plugin_ctor_ran"); CHECK(ran && *ran == 1);
    int (*add)(int, int); *(void **)(&add) = dlsym(h, "plugin_add"); CHECK(add && add(40, 2) == 42);
    CHECK(dlsym(h, "no_such_symbol") == NULL && dlerror() != NULL);
    CHECK(dlopen("libdoes-not-exist.so.1", RTLD_NOW) == NULL);
    dl_iterate_phdr(cb, NULL); CHECK(saw_libc);
    CHECK(dlclose(h) == 0);
    DONE("dlopen_plugin");
}
