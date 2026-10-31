/* Dependency resolution. Pure logic: no filesystem, no network, no crypto.
 * Input: installed metadata, candidate packages, explicit requests. Output: an ordered plan.
 * Handles versioned depends, provides, conflicts, shared-library (soname) needs, and refuses to
 * mix libc ABIs. Search is a bounded backtracking solver preferring the installed version, then the newest. */
#ifndef AETHER_PM_RESOLVE_H
#define AETHER_PM_RESOLVE_H
#include "repo.h"

/* A Plan holds pointers into the Universe it was computed from; keep the Universe alive while using the Plan. */
typedef struct {
    const Meta **installed; size_t ninstalled;
    const Avail *avail; size_t navail;
} Universe;

typedef struct { const Meta *meta; const Avail *avail; int is_upgrade; const Meta *replaces; } Step;   /* avail NULL => already-installed (unused in plans) */
typedef struct { Step *install; size_t ninstall; const Meta **remove; size_t nremove; } Plan;

int resolve_install(const Universe *u, const Atom *want, size_t nwant, Plan *plan, char *err);
/* Upgrade every installed package (or only names[]) to the newest candidate that keeps the system consistent. */
int resolve_upgrade(const Universe *u, const char **names, size_t nnames, Plan *plan, char *err);
int resolve_remove(const Universe *u, const char **names, size_t nnames, int force, Plan *plan, char *err);
void plan_free(Plan *p);
#endif
