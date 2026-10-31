#define _GNU_SOURCE
#include "resolve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const Meta *m; const Avail *av; } WPkg;
typedef struct { WPkg *p; size_t n; } World;
typedef struct { int is_so; const char *name; VerOp op; const char *ver; const Meta *by; } Need;
typedef struct { const Universe *u; const Need *wants; size_t nwants; long budget; int bestdepth; char best[PM_ERRLEN]; } Ctx;

static int provides(const Meta *m, const Need *n)
{
    if (n->is_so) { for (size_t i = 0; i < m->nprovides_so; i++) if (!strcmp(m->provides_so[i], n->name)) return 1; return 0; }
    Atom a = { (char *)n->name, n->op, (char *)n->ver };
    if (!strcmp(m->name, n->name) && atom_satisfied_by(&a, m->version)) return 1;
    for (size_t i = 0; i < m->nprovides; i++) if (!strcmp(m->provides[i].name, n->name)) { if (n->op == OP_ANY) return 1; if (m->provides[i].ver && atom_satisfied_by(&a, m->provides[i].ver)) return 1; }
    return 0;
}
static int world_provides(const World *w, const Need *n) { for (size_t i = 0; i < w->n; i++) if (provides(w->p[i].m, n)) return 1; return 0; }

static int find_unsat(const World *w, const Need *wants, size_t nw, Need *out)
{
    for (size_t i = 0; i < nw; i++) if (!world_provides(w, &wants[i])) { *out = wants[i]; return 1; }
    for (size_t i = 0; i < w->n; i++) {
        const Meta *m = w->p[i].m;
        for (size_t j = 0; j < m->ndepends; j++) { Need n = { 0, m->depends[j].name, m->depends[j].op, m->depends[j].ver, m }; if (!world_provides(w, &n)) { *out = n; return 1; } }
        for (size_t j = 0; j < m->nneeds_so; j++) { Need n = { 1, m->needs_so[j], OP_ANY, NULL, m }; if (!world_provides(w, &n)) { *out = n; return 1; } }
    }
    return 0;
}
static char *need_str(const Need *n) { if (n->is_so) return xsprintf("shared library %s", n->name); Atom a = { (char *)n->name, n->op, (char *)n->ver }; return atom_str(&a); }

static int atom_hits(const Atom *a, const Meta *m)
{
    Need n = { 0, a->name, a->op, a->ver, NULL }; return provides(m, &n);
}
/* Returns NULL if cand may join the world next to every other package, else a malloc'd reason. */
static char *conflict_reason(const World *w, const Meta *cand)
{
    for (size_t i = 0; i < w->n; i++) {
        const Meta *o = w->p[i].m; if (!strcmp(o->name, cand->name)) continue;       /* same name: replacement */
        for (size_t j = 0; j < cand->nconflicts; j++) if (atom_hits(&cand->conflicts[j], o)) return xsprintf("%s %s conflicts with installed/selected %s %s", cand->name, cand->version, o->name, o->version);
        for (size_t j = 0; j < o->nconflicts; j++) if (atom_hits(&o->conflicts[j], cand)) return xsprintf("%s %s conflicts with %s %s", o->name, o->version, cand->name, cand->version);
        if (strcmp(cand->libc, "none") && strcmp(o->libc, "none") && strcmp(cand->libc, o->libc))
            return xsprintf("libc ABI mismatch: %s needs %s but %s uses %s (mixing libc environments is refused)", cand->name, cand->libc, o->name, o->libc);
    }
    return NULL;
}

typedef struct { const Avail *a; int exact; } Cand;
static int cand_cmp(const void *x, const void *y)
{
    const Cand *a = x, *b = y;
    if (a->exact != b->exact) return b->exact - a->exact;                     /* exact-name matches before virtual providers */
    int c = strcmp(a->a->meta.name, b->a->meta.name);
    if (c) return c;
    return -ver_cmp(a->a->meta.version, b->a->meta.version);                    /* newest first */
}

static int world_clone_with(const World *w, const WPkg *add, World *out)
{
    out->p = xmalloc((w->n + 1) * sizeof *out->p); out->n = 0; int replaced = 0;
    for (size_t i = 0; i < w->n; i++) { if (!strcmp(w->p[i].m->name, add->m->name)) { out->p[out->n++] = *add; replaced = 1; } else out->p[out->n++] = w->p[i]; }
    if (!replaced) out->p[out->n++] = *add;
    return 0;
}

static void note_fail(Ctx *c, int depth, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void note_fail(Ctx *c, int depth, const char *fmt, ...)
{
    if (depth < c->bestdepth && c->best[0]) return;
    c->bestdepth = depth; va_list ap; va_start(ap, fmt); vsnprintf(c->best, sizeof c->best, fmt, ap); va_end(ap);
}

static int solve(Ctx *c, const World *w, int depth, World *result)
{
    if (--c->budget < 0) { snprintf(c->best, sizeof c->best, "dependency resolution too complex (search budget exhausted)"); return 0; }
    Need n;
    if (!find_unsat(w, c->wants, c->nwants, &n)) { result->p = xmalloc((w->n ? w->n : 1) * sizeof *result->p); if (w->n) memcpy(result->p, w->p, w->n * sizeof *w->p);
        result->n = w->n;
        return 1;
    }
    Cand *cands = NULL; size_t nc = 0;
    for (size_t i = 0; i < c->u->navail; i++) {
        const Avail *a = &c->u->avail[i];
        if (!provides(&a->meta, &n)) continue;
        int dup = 0; for (size_t k = 0; k < w->n; k++) if (w->p[k].m == &a->meta) dup = 1;
        if (dup) continue;
        cands = xrealloc(cands, (nc + 1) * sizeof *cands); cands[nc].a = a; cands[nc].exact = n.is_so ? 1 : !strcmp(a->meta.name, n.name); nc++;
    }
    if (nc > 1) qsort(cands, nc, sizeof *cands, cand_cmp);
    char *ns = need_str(&n);
    if (nc == 0) { if (n.by) note_fail(c, depth, "nothing provides %s (required by %s %s)", ns, n.by->name, n.by->version); else note_fail(c, depth, "no package provides %s", ns); }
    int ok = 0;
    for (size_t i = 0; i < nc && !ok; i++) {
        const Meta *cm = &cands[i].a->meta; char *why = conflict_reason(w, cm);
        if (why) { note_fail(c, depth, "%s", why); free(why); continue; }
        WPkg add = { cm, cands[i].a }; World w2; world_clone_with(w, &add, &w2);
        ok = solve(c, &w2, depth + 1, result); free(w2.p);
    }
    free(ns); free(cands); return ok;
}

static int same_pkg(const Meta *a, const Meta *b) { return !strcmp(a->name, b->name) && !strcmp(a->version, b->version); }

static void emit_order(const World *w, size_t i, int *state, Plan *plan, const Universe *u)
{
    if (state[i]) return;
    state[i] = 1;
    const Meta *m = w->p[i].m;
    for (size_t j = 0; j < m->ndepends + m->nneeds_so; j++) {
        Need n = j < m->ndepends ? (Need){ 0, m->depends[j].name, m->depends[j].op, m->depends[j].ver, m } : (Need){ 1, m->needs_so[j - m->ndepends], OP_ANY, NULL, m };
        for (size_t k = 0; k < w->n; k++) if (k != i && provides(w->p[k].m, &n)) { emit_order(w, k, state, plan, u); break; }
    }
    if (w->p[i].av) {
        Step s = { m, w->p[i].av, 0, NULL };
        for (size_t k = 0; k < u->ninstalled; k++) if (!strcmp(u->installed[k]->name, m->name)) { s.replaces = u->installed[k]; s.is_upgrade = 1; }
        plan->install = xrealloc(plan->install, (plan->ninstall + 1) * sizeof *plan->install); plan->install[plan->ninstall++] = s;
    }
}

static int finish(const Universe *u, const World *fin, Plan *plan)
{
    int *state = calloc(fin->n ? fin->n : 1, sizeof *state); if (!state) exit(70);
    for (size_t i = 0; i < fin->n; i++) emit_order(fin, i, state, plan, u);
    free(state);
    /* drop steps that do not change anything */
    size_t o = 0;
    for (size_t i = 0; i < plan->ninstall; i++) { if (plan->install[i].replaces && same_pkg(plan->install[i].replaces, plan->install[i].meta)) continue; plan->install[o++] = plan->install[i]; }
    plan->ninstall = o; return 0;
}

static int run(const Universe *u, const Need *wants, size_t nw, Plan *plan, char *err)
{
    memset(plan, 0, sizeof *plan);
    World w; w.n = u->ninstalled; w.p = xmalloc((w.n ? w.n : 1) * sizeof *w.p);
    for (size_t i = 0; i < w.n; i++) { w.p[i].m = u->installed[i]; w.p[i].av = NULL; }
    Ctx c = { u, wants, nw, 200000, -1, {0} }; World res = {0};
    int ok = solve(&c, &w, 0, &res); free(w.p);
    if (!ok) { set_err(err, "%s", c.best[0] ? c.best : "cannot resolve dependencies"); return -1; }
    finish(u, &res, plan); free(res.p); return 0;
}

int resolve_install(const Universe *u, const Atom *want, size_t nwant, Plan *plan, char *err)
{
    Need *nw = xmalloc((nwant ? nwant : 1) * sizeof *nw);
    for (size_t i = 0; i < nwant; i++) nw[i] = (Need){ 0, want[i].name, want[i].op, want[i].ver, NULL };
    int rc = run(u, nw, nwant, plan, err); free(nw); return rc;
}

int resolve_upgrade(const Universe *u, const char **names, size_t nnames, Plan *plan, char *err)
{
    Need *nw = xmalloc((u->ninstalled + 1) * sizeof *nw); size_t n = 0;
    for (size_t i = 0; i < u->ninstalled; i++) {
        const Meta *m = u->installed[i];
        if (nnames) { int sel = 0; for (size_t k = 0; k < nnames; k++) if (!strcmp(names[k], m->name)) sel = 1; if (!sel) continue; }
        const char *best = NULL;
        for (size_t k = 0; k < u->navail; k++) { const Meta *a = &u->avail[k].meta; if (!strcmp(a->name, m->name) && ver_cmp(a->version, m->version) > 0 && (!best || ver_cmp(a->version, best) > 0)) best = a->version; }
        if (best) nw[n++] = (Need){ 0, m->name, OP_GE, best, NULL };
    }
    int rc = run(u, nw, n, plan, err); free(nw); return rc;
}

int resolve_remove(const Universe *u, const char **names, size_t nnames, int force, Plan *plan, char *err)
{
    memset(plan, 0, sizeof *plan);
    World w; w.p = xmalloc((u->ninstalled + 1) * sizeof *w.p); w.n = 0;
    for (size_t k = 0; k < nnames; k++) {
        const Meta *f = NULL; for (size_t i = 0; i < u->ninstalled; i++) if (!strcmp(u->installed[i]->name, names[k])) f = u->installed[i];
        if (!f) { set_err(err, "%s is not installed", names[k]); free(w.p); return -1; }
        if (f->essential && !force) { set_err(err, "%s is essential; refusing to remove it (use --force)", names[k]); free(w.p); return -1; }
        plan->remove = xrealloc(plan->remove, (plan->nremove + 1) * sizeof *plan->remove); plan->remove[plan->nremove++] = f;
    }
    for (size_t i = 0; i < u->ninstalled; i++) { int rm = 0; for (size_t k = 0; k < plan->nremove; k++) if (plan->remove[k] == u->installed[i]) rm = 1; if (!rm) { w.p[w.n].m = u->installed[i]; w.p[w.n].av = NULL; w.n++; } }
    Need n;
    if (!force && find_unsat(&w, NULL, 0, &n)) {
        char *s = need_str(&n); set_err(err, "cannot remove: %s %s still requires %s", n.by ? n.by->name : "?", n.by ? n.by->version : "", s); free(s); free(w.p); free(plan->remove); memset(plan, 0, sizeof *plan); return -1;
    }
    free(w.p); return 0;
}

void plan_free(Plan *p) { free(p->install); free(p->remove); memset(p, 0, sizeof *p); }
