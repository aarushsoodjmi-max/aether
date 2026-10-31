/* aether-pm: command-line front end. Only orchestration lives here; parsing, verification, repository
 * access, resolution and filesystem work are in their own modules. */
#define _GNU_SOURCE
#include "db.h"
#include "repo.h"
#include "resolve.h"
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define VERSION_STR "0.1"
typedef struct { char *name, *url; } RepoCfg;
typedef struct {
    const char *root, *keydir; int dry, force, allow_unsigned;
    RepoCfg *repos; size_t nrepos; Keyring kr;
} Cfg;

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void die(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fputs("aether-pm: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap); exit(1); }
static char *rp(const Cfg *c, const char *rel) { return xsprintf("%s/%s", c->root, rel); }

static void usage(void)
{
    fputs("aether-pm " VERSION_STR " - Aether package manager (.aether)\n"
"usage: aether-pm [--root DIR] [--keys DIR] [--dry-run] [--force] [--allow-unsigned] COMMAND ...\n"
"  unprivileged inspection (no root needed):\n"
"    inspect FILE.aether [--files]     show metadata (and manifest)\n"
"    verify FILE.aether                check signature (trusted keys) and every file hash\n"
"    list | info NAME | files NAME | owner PATH | check [NAME...]\n"
"  repository / author tools:\n"
"    keygen PREFIX                     write PREFIX.sec (secret, 0600) and PREFIX.pub\n"
"    build INFO TREE OUT.aether [--key SECRET]\n"
"    index REPODIR --name NAME --key SECRET [--timestamp N]\n"
"  system changes (root for --root /):\n"
"    update | install TARGET... | upgrade [NAME...] | remove NAME... | recover\n"
"  TARGET is NAME, NAME=VERSION, or a path to a .aether file.\n", stderr);
}

/* ---------- configuration ---------- */
static void load_cfg(Cfg *c)
{
    char err[PM_ERRLEN] = {0};
    char *kd = c->keydir ? xstrdup(c->keydir) : rp(c, "etc/aether/keys");
    if (keyring_load(kd, &c->kr, err) != 0) die("%s", err);
    free(kd);
    char *cf = rp(c, "etc/aether/pm.conf"); size_t len; char *t = read_file(cf, &len, 1 << 20, err); free(cf);
    if (!t) return;
    char *save = NULL;
    for (char *l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        if (*l == '#' || *l == 0) continue;
        char a[32], n[80], u[1024];
        if (sscanf(l, "%31s %79s %1023s", a, n, u) == 3 && !strcmp(a, "repo") && valid_pkg_name(n) && valid_token(u)) {
            c->repos = xrealloc(c->repos, (c->nrepos + 1) * sizeof *c->repos); c->repos[c->nrepos].name = xstrdup(n); c->repos[c->nrepos].url = xstrdup(u); c->nrepos++;
        } else die("bad line in etc/aether/pm.conf: %s", l);
    }
    free(t);
}

static void need_privilege(const Cfg *c)
{
    if (!strcmp(c->root, "/") && geteuid() != 0) die("this command changes the system and needs root (inspection commands do not)");
}
static void begin_mutation(const Cfg *c)
{
    char err[PM_ERRLEN] = {0}; need_privilege(c);
    if (pm_init_dirs(c->root, err) != 0) die("%s", err);
}
static int g_lockfd = -1;
static void lock_and_recover(const Cfg *c)
{
    char err[PM_ERRLEN] = {0};
    if (pm_lock(c->root, &g_lockfd, err) != 0) die("%s", err);
    int r = txn_recover(c->root, err);
    if (r < 0) die("recovery failed: %s", err);
    if (r == 1) fputs("aether-pm: rolled back an interrupted transaction\n", stderr);
    if (r == 2) fputs("aether-pm: completed an interrupted (already committed) transaction\n", stderr);
}

/* ---------- authoring ---------- */
static const char *opt_value(int *i, int argc, char **argv, const char *flag)
{
    if (*i + 1 >= argc) die("%s needs a value", flag);
    return argv[++*i];
}
static int cmd_keygen(int argc, char **argv)
{
    if (argc != 1) { usage(); return 2; }
    char err[PM_ERRLEN] = {0}; if (key_generate(argv[0], err) != 0) die("%s", err);
    printf("wrote %s.sec (keep secret) and %s.pub (install in etc/aether/keys/)\n", argv[0], argv[0]); return 0;
}
static int cmd_build(int argc, char **argv)
{
    const char *key = NULL; const char *pos[3]; int np = 0;
    for (int i = 0; i < argc; i++) { if (!strcmp(argv[i], "--key")) key = opt_value(&i, argc, argv, "--key"); else if (np < 3) pos[np++] = argv[i]; else { usage(); return 2; } }
    if (np != 3) { usage(); return 2; }
    uint8_t sk[64]; char err[PM_ERRLEN] = {0};
    if (key && key_load_secret(key, sk, err) != 0) die("%s", err);
    if (pkg_build(pos[0], pos[1], pos[2], key ? sk : NULL, err) != 0) die("build failed: %s", err);
    PkgFile p; if (pkg_open(pos[2], &p, err) != 0) die("built package does not re-open: %s", err);
    printf("built %s: %s %s (%zu entries, %llu payload bytes)%s\n", pos[2], p.meta.name, p.meta.version, p.meta.nentries, (unsigned long long)p.meta.payload_len, key ? ", signed" : ", UNSIGNED");
    pkg_close(&p); return 0;
}
static int cmd_index(int argc, char **argv)
{
    const char *key = NULL, *name = NULL, *dir = NULL; unsigned long long ts = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--key")) key = opt_value(&i, argc, argv, "--key");
        else if (!strcmp(argv[i], "--name")) name = opt_value(&i, argc, argv, "--name");
        else if (!strcmp(argv[i], "--timestamp")) ts = strtoull(opt_value(&i, argc, argv, "--timestamp"), NULL, 10);
        else if (!dir) dir = argv[i]; else { usage(); return 2; }
    }
    if (!dir || !name || !key) { usage(); return 2; }
    if (!ts) ts = (unsigned long long)time(NULL);
    uint8_t sk[64]; char err[PM_ERRLEN] = {0}, *sig = NULL;
    if (key_load_secret(key, sk, err) != 0) die("%s", err);
    char *ix = index_build(dir, name, ts, sk, &sig, err); if (!ix) die("index failed: %s", err);
    char *ip = xsprintf("%s/INDEX", dir), *sp = xsprintf("%s/INDEX.sig", dir);
    if (write_file_atomic(ip, ix, strlen(ix), 0644, err) != 0 || write_file_atomic(sp, sig, strlen(sig), 0644, err) != 0) die("%s", err);
    printf("wrote %s and %s (timestamp %llu)\n", ip, sp, ts); return 0;
}

/* ---------- unprivileged inspection ---------- */
static void print_meta(const Meta *m, int files)
{
    printf("Name:     %s\nVersion:  %s\nArch:     %s\nLibc:     %s\n", m->name, m->version, m->arch, m->libc);
    if (m->summary) printf("Summary:  %s\n", m->summary);
    if (m->essential) printf("Essential: yes\n");
    for (size_t i = 0; i < m->ndepends; i++) { char *a = atom_str(&m->depends[i]); printf("Depends:  %s\n", a); free(a); }
    for (size_t i = 0; i < m->nconflicts; i++) { char *a = atom_str(&m->conflicts[i]); printf("Conflicts: %s\n", a); free(a); }
    for (size_t i = 0; i < m->nprovides; i++) printf("Provides: %s%s%s\n", m->provides[i].name, m->provides[i].ver ? " = " : "", m->provides[i].ver ? m->provides[i].ver : "");
    for (size_t i = 0; i < m->nneeds_so; i++) printf("Needs library:    %s\n", m->needs_so[i]);
    for (size_t i = 0; i < m->nprovides_so; i++) printf("Provides library: %s\n", m->provides_so[i]);
    printf("Entries:  %zu (%llu payload bytes)\n", m->nentries, (unsigned long long)m->payload_len);
    if (files) for (size_t i = 0; i < m->nentries; i++) { const Entry *e = &m->entries[i]; if (e->type == 'l') printf("  l %s -> %s\n", e->path, e->target); else printf("  %c %04o %s\n", e->type, e->mode, e->path); }
}
static int cmd_inspect(int argc, char **argv)
{
    int files = 0; const char *f = NULL;
    for (int i = 0; i < argc; i++) { if (!strcmp(argv[i], "--files")) files = 1; else if (!f) f = argv[i]; else { usage(); return 2; } }
    if (!f) { usage(); return 2; }
    PkgFile p; char err[PM_ERRLEN] = {0}; if (pkg_open(f, &p, err) != 0) die("%s", err);
    print_meta(&p.meta, files);
    char h[2 * PM_KEYID_LEN + 1]; hex_encode(p.keyid, PM_KEYID_LEN, h);
    printf("Signature: %s\n", p.is_signed ? h : "none"); printf("(signature not verified by 'inspect'; use 'verify')\n");
    pkg_close(&p); return 0;
}
static int cmd_verify(Cfg *c, int argc, char **argv)
{
    if (argc != 1) { usage(); return 2; }
    PkgFile p; char err[PM_ERRLEN] = {0}; if (pkg_open(argv[0], &p, err) != 0) die("%s", err);
    if (pkg_verify_signature(&p, &c->kr, err) != 0) die("%s: %s", argv[0], err);
    if (pkg_verify_payload(&p, err) != 0) die("%s: %s", argv[0], err);
    printf("%s: OK (%s %s, signature valid, %zu entries verified)\n", argv[0], p.meta.name, p.meta.version, p.meta.nentries);
    pkg_close(&p); return 0;
}

/* ---------- repository state ---------- */
static char *cache_dir(const Cfg *c, const char *repo) { return xsprintf("%s/var/cache/aether/pm/%s", c->root, repo); }
static int cmd_update(Cfg *c)
{
    begin_mutation(c); lock_and_recover(c);
    if (c->nrepos == 0) die("no repositories configured in etc/aether/pm.conf");
    for (size_t i = 0; i < c->nrepos; i++) {
        char err[PM_ERRLEN] = {0}; RepoCfg *r = &c->repos[i];
        char *cd = cache_dir(c, r->name); mkdir(cd, 0755);
        char *tix = xsprintf("%s/INDEX.new", cd), *tsg = xsprintf("%s/INDEX.sig.new", cd);
        if (repo_fetch(r->url, "INDEX", tix, err) != 0 || repo_fetch(r->url, "INDEX.sig", tsg, err) != 0) die("repo %s: %s", r->name, err);
        size_t il, sl; char *ix = read_file(tix, &il, 64u << 20, err); char *sg = ix ? read_file(tsg, &sl, 4096, err) : NULL;
        if (!ix || !sg) die("repo %s: %s", r->name, err);
        if (index_verify(ix, il, sg, &c->kr, err) != 0) { unlink(tix); unlink(tsg); die("repo %s: %s (index rejected)", r->name, err); }
        Index idx; if (index_parse(ix, il, &idx, err) != 0) die("repo %s: %s", r->name, err);
        if (strcmp(idx.repo_name, r->name) != 0) die("repo %s: index claims to be repository '%s'", r->name, idx.repo_name);
        char *tsf = xsprintf("%s/" PM_DB_DIR "/state/%s.ts", c->root, r->name); char *old = read_file(tsf, NULL, 64, err);
        if (old && strtoull(old, NULL, 10) > idx.timestamp && !c->force) die("repo %s: index timestamp %llu is OLDER than the last seen %s (replay/rollback attack?); use --force to accept", r->name, (unsigned long long)idx.timestamp, old);
        char *fi = xsprintf("%s/INDEX", cd), *fs = xsprintf("%s/INDEX.sig", cd);
        if (rename(tix, fi) != 0 || rename(tsg, fs) != 0) die("cannot store index: %s", strerror(errno));
        char tb[32]; int tl = snprintf(tb, sizeof tb, "%llu\n", (unsigned long long)idx.timestamp);
        if (write_file_atomic(tsf, tb, (size_t)tl, 0644, err) != 0) die("%s", err);
        printf("%s: %zu packages (index verified, timestamp %llu)\n", r->name, idx.n, (unsigned long long)idx.timestamp);
        index_free(&idx);
    }
    return 0;
}

typedef struct {
    Installed inst; const Meta **instp; Index *idx; size_t nidx;
    Avail *avail; size_t navail; PkgFile *locals; size_t nlocals;
} World;

static void world_load(Cfg *c, World *w)
{
    char err[PM_ERRLEN] = {0}; memset(w, 0, sizeof *w);
    if (db_load(c->root, &w->inst, err) != 0) die("%s", err);
    w->instp = xmalloc((w->inst.n + 1) * sizeof *w->instp); for (size_t i = 0; i < w->inst.n; i++) w->instp[i] = &w->inst.pkgs[i];
    for (size_t i = 0; i < c->nrepos; i++) {
        char *cd = cache_dir(c, c->repos[i].name); char *fi = xsprintf("%s/INDEX", cd), *fs = xsprintf("%s/INDEX.sig", cd);
        size_t il, sl; char *ix = read_file(fi, &il, 64u << 20, err);
        if (!ix) { fprintf(stderr, "aether-pm: warning: repository %s has no cached index (run 'update')\n", c->repos[i].name); continue; }
        char *sg = read_file(fs, &sl, 4096, err); if (!sg) die("repo %s: %s", c->repos[i].name, err);
        if (index_verify(ix, il, sg, &c->kr, err) != 0) die("repo %s: cached %s (re-run 'update')", c->repos[i].name, err);
        w->idx = xrealloc(w->idx, (w->nidx + 1) * sizeof *w->idx);
        if (index_parse(ix, il, &w->idx[w->nidx], err) != 0) die("repo %s: %s", c->repos[i].name, err);
        Index *x = &w->idx[w->nidx++];
        for (size_t k = 0; k < x->n; k++) {
            if (strcmp(x->pkgs[k].meta.arch, "x86_64") && strcmp(x->pkgs[k].meta.arch, "any")) continue;
            w->avail = xrealloc(w->avail, (w->navail + 1) * sizeof *w->avail); w->avail[w->navail++] = x->pkgs[k];   /* shallow: owned by idx */
        }
    }
}
static void add_local(Cfg *c, World *w, const char *path, Atom *want)
{
    char err[PM_ERRLEN] = {0}; w->locals = xrealloc(w->locals, (w->nlocals + 1) * sizeof *w->locals);
    PkgFile *p = &w->locals[w->nlocals++];
    if (pkg_open(path, p, err) != 0) die("%s", err);
    if (strcmp(p->meta.arch, "x86_64") && strcmp(p->meta.arch, "any")) die("%s: unsupported architecture %s", path, p->meta.arch);
    if (pkg_verify_signature(p, &c->kr, err) != 0) { if (!c->allow_unsigned) die("%s: %s (use --allow-unsigned only for packages you built yourself)", path, err); fprintf(stderr, "aether-pm: warning: installing %s WITHOUT signature verification\n", path); }
    if (pkg_verify_payload(p, err) != 0) die("%s: %s", path, err);
    Avail a; memset(&a, 0, sizeof a); a.meta = p->meta; const char *b = strrchr(path, '/'); a.filename = xstrdup(b ? b + 1 : path); a.localpath = xstrdup(path);
    w->avail = xrealloc(w->avail, (w->navail + 1) * sizeof *w->avail); w->avail[w->navail++] = a;
    want->name = xstrdup(p->meta.name); want->op = OP_EQ; want->ver = xstrdup(p->meta.version);
}

/* ---------- plan execution ---------- */
static void print_plan(const Plan *p)
{
    for (size_t i = 0; i < p->nremove; i++) printf("  remove   %s %s\n", p->remove[i]->name, p->remove[i]->version);
    for (size_t i = 0; i < p->ninstall; i++) {
        const Step *s = &p->install[i];
        if (s->replaces) printf("  %-8s %s %s -> %s\n", ver_cmp(s->meta->version, s->replaces->version) >= 0 ? "upgrade" : "downgrade", s->meta->name, s->replaces->version, s->meta->version);
        else printf("  install  %s %s\n", s->meta->name, s->meta->version);
    }
}
static PkgFile *open_step_package(Cfg *c, World *w, const Step *s)
{
    char err[PM_ERRLEN] = {0};
    if (s->avail->localpath) { for (size_t i = 0; i < w->nlocals; i++) if (&w->locals[i].meta == s->meta) return &w->locals[i]; die("internal: local package not found"); }
    char *cd = cache_dir(c, s->avail->repo); mkdir(cd, 0755); char *pd = xsprintf("%s/pkgs", cd); mkdir(pd, 0755);
    char *dst = xsprintf("%s/%s", pd, s->avail->filename);
    const RepoCfg *rc = NULL; for (size_t i = 0; i < c->nrepos; i++) if (!strcmp(c->repos[i].name, s->avail->repo)) rc = &c->repos[i];
    if (!rc) die("repository %s no longer configured", s->avail->repo);
    uint8_t h[32]; uint64_t sz; int have = pkg_hash_file(dst, h, &sz, NULL) == 0 && sz == s->avail->size && sodium_memcmp(h, s->avail->sha, 32) == 0;
    if (!have) {
        printf("  fetching %s\n", s->avail->filename);
        if (repo_fetch(rc->url, s->avail->filename, dst, err) != 0) die("%s", err);
        if (pkg_hash_file(dst, h, &sz, err) != 0) die("%s", err);
        if (sz != s->avail->size || sodium_memcmp(h, s->avail->sha, 32) != 0) { unlink(dst); die("%s: size/SHA-256 does not match the signed index (corrupt or tampered download)", s->avail->filename); }
    }
    PkgFile *p = xmalloc(sizeof *p); if (pkg_open(dst, p, err) != 0) die("%s", err);
    char *a = meta_serialize(&p->meta, 0), *b = meta_serialize(&s->avail->meta, 0);
    if (strcmp(a, b) != 0) die("%s: package metadata differs from the signed index entry", s->avail->filename);
    free(a); free(b);
    if (p->is_signed) { if (pkg_verify_signature(p, &c->kr, err) != 0) die("%s: %s", s->avail->filename, err); }
    if (pkg_verify_payload(p, err) != 0) die("%s: %s", s->avail->filename, err);
    return p;
}

typedef struct { const char *path; const char *pkg; } PathOwner;
static int po_cmp(const void *a, const void *b) { return strcmp(((const PathOwner *)a)->path, ((const PathOwner *)b)->path); }

static void preflight(Cfg *c, const World *w, const Plan *plan, PkgFile **pf)
{
    size_t total = 0; for (size_t i = 0; i < plan->ninstall; i++) total += pf[i]->meta.nentries;
    PathOwner *po = xmalloc((total + 1) * sizeof *po); size_t k = 0;
    for (size_t i = 0; i < plan->ninstall; i++) for (size_t j = 0; j < pf[i]->meta.nentries; j++) { const Entry *e = &pf[i]->meta.entries[j]; if (e->type == 'd') continue; po[k].path = e->path; po[k++].pkg = pf[i]->meta.name; }
    qsort(po, k, sizeof *po, po_cmp);
    for (size_t i = 1; i < k; i++) if (!strcmp(po[i - 1].path, po[i].path)) die("file conflict inside this transaction: %s is provided by both %s and %s", po[i].path, po[i - 1].pkg, po[i].pkg);
    free(po);
    for (size_t i = 0; i < plan->ninstall; i++) for (size_t j = 0; j < pf[i]->meta.nentries; j++) {
        const Entry *e = &pf[i]->meta.entries[j]; struct stat st;
        if (fs_lstat(c->root, e->path, &st) != 0) {
            if (errno == ENOENT) continue;
            die("%s: %s: %s (a parent is a symlink or not a directory)", pf[i]->meta.name, e->path, strerror(errno));
        }
        if (e->type == 'd') { if (!S_ISDIR(st.st_mode)) die("%s: %s exists and is not a directory", pf[i]->meta.name, e->path); continue; }
        if (S_ISDIR(st.st_mode)) die("%s: %s is an existing directory", pf[i]->meta.name, e->path);
        const Meta *own = db_owner(&w->inst, e->path, NULL); int ok = 0;
        if (own) { if (plan->install[i].replaces == own) ok = 1; for (size_t r = 0; r < plan->nremove; r++) if (plan->remove[r] == own) ok = 1; for (size_t s = 0; s < plan->ninstall; s++) if (plan->install[s].replaces == own) ok = 1; }
        if (!ok) die("refusing to overwrite %s: it is %s (never silently replaces files, especially core libraries)", e->path, own ? "owned by another package" : "not owned by any package");
    }
}

static int has_path(const Meta *m, const char *path) { for (size_t i = 0; i < m->nentries; i++) if (!strcmp(m->entries[i].path, path)) return 1; return 0; }

static void execute(Cfg *c, World *w, Plan *plan, char **world_add, size_t nadd, char **world_del, size_t ndel)
{
    char err[PM_ERRLEN] = {0};
    PkgFile **pf = xmalloc((plan->ninstall + 1) * sizeof *pf);
    for (size_t i = 0; i < plan->ninstall; i++) pf[i] = open_step_package(c, w, &plan->install[i]);
    preflight(c, w, plan, pf);
    signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN); signal(SIGPIPE, SIG_IGN);
    Txn *t = txn_begin(c->root, err); if (!t) die("%s", err);
#define TX(call) do { if ((call) != 0) { txn_abort(t); die("transaction rolled back: %s", err); } } while (0)
    for (size_t i = 0; i < plan->ninstall; i++) for (size_t j = 0; j < pf[i]->meta.nentries; j++) TX(txn_stage_entry(t, pf[i], &pf[i]->meta.entries[j], err));
    TX(txn_apply(t, err));
    for (size_t i = 0; i < plan->ninstall; i++) {                         /* files the old version had and the new one drops */
        const Meta *old = plan->install[i].replaces; if (!old) continue;
        for (size_t j = 0; j < old->nentries; j++) if (old->entries[j].type != 'd' && !has_path(&pf[i]->meta, old->entries[j].path)) TX(txn_remove_path(t, old->entries[j].path, err));
        for (size_t j = old->nentries; j-- > 0; ) if (old->entries[j].type == 'd' && !has_path(&pf[i]->meta, old->entries[j].path)) TX(txn_rmdir_if_empty(t, old->entries[j].path, err));
    }
    for (size_t r = 0; r < plan->nremove; r++) {
        const Meta *m = plan->remove[r];
        for (size_t j = 0; j < m->nentries; j++) if (m->entries[j].type != 'd') TX(txn_remove_path(t, m->entries[j].path, err));
        for (size_t j = m->nentries; j-- > 0; ) if (m->entries[j].type == 'd') TX(txn_rmdir_if_empty(t, m->entries[j].path, err));
        char *mp = db_meta_path(m->name); TX(txn_remove_path(t, mp, err)); free(mp);
    }
    for (size_t i = 0; i < plan->ninstall; i++) { char *mp = db_meta_path(pf[i]->meta.name); TX(txn_put_bytes(t, mp, pf[i]->raw_meta, pf[i]->raw_meta_len, 0644, err)); free(mp); }
    { /* explicit-install list */
        char **names = NULL; size_t n = 0; db_world_load(c->root, &names, &n);
        for (size_t i = 0; i < ndel; i++) for (size_t j = 0; j < n; j++) if (!strcmp(names[j], world_del[i])) { free(names[j]); names[j] = names[--n]; j--; }
        for (size_t i = 0; i < nadd; i++) { int have = 0; for (size_t j = 0; j < n; j++) if (!strcmp(names[j], world_add[i])) have = 1; if (!have) { names = xrealloc(names, (n + 1) * sizeof *names); names[n++] = xstrdup(world_add[i]); } }
        char *ws = db_world_serialize(names, n); TX(txn_put_bytes(t, PM_DB_DIR "/world", ws, strlen(ws), 0644, err)); free(ws);
    }
    TX(txn_apply(t, err));
    if (txn_commit(t, err) != 0) die("commit failed: %s (run 'aether-pm recover')", err);
#undef TX
    printf("done: %zu installed/upgraded, %zu removed\n", plan->ninstall, plan->nremove);
}

static void parse_targets(Cfg *c, World *w, int argc, char **argv, Atom **wants, size_t *nw, char ***names)
{
    *wants = xmalloc((size_t)(argc + 1) * sizeof **wants); *nw = 0; *names = xmalloc((size_t)(argc + 1) * sizeof **names);
    for (int i = 0; i < argc; i++) {
        Atom a; memset(&a, 0, sizeof a); size_t l = strlen(argv[i]);
        if (strchr(argv[i], '/') || (l > 7 && !strcmp(argv[i] + l - 7, ".aether"))) add_local(c, w, argv[i], &a);
        else {
            char *eq = strchr(argv[i], '='); char nm[80]; char err[PM_ERRLEN] = {0};
            size_t nl = eq ? (size_t)(eq - argv[i]) : l; if (nl >= sizeof nm) die("bad package name");
            memcpy(nm, argv[i], nl); nm[nl] = 0;
            char *tok[3] = { nm, "=", eq ? eq + 1 : NULL };
            if (atom_parse(tok, eq ? 3 : 1, &a, err) != 0) die("%s: %s", argv[i], err);
        }
        (*wants)[(*nw)] = a; (*names)[(*nw)++] = xstrdup(a.name);
    }
}

static int cmd_install(Cfg *c, int argc, char **argv)
{
    if (argc < 1) { usage(); return 2; }
    begin_mutation(c); lock_and_recover(c);
    World w; world_load(c, &w);
    Atom *wants; size_t nw; char **names; parse_targets(c, &w, argc, argv, &wants, &nw, &names);
    Universe u = { w.instp, w.inst.n, w.avail, w.navail }; Plan plan; char err[PM_ERRLEN] = {0};
    if (resolve_install(&u, wants, nw, &plan, err) != 0) die("cannot resolve: %s", err);
    if (plan.ninstall == 0 && plan.nremove == 0) { printf("nothing to do (already installed)\n"); return 0; }
    printf("plan:\n"); print_plan(&plan);
    if (c->dry) { printf("(dry run; nothing changed)\n"); return 0; }
    execute(c, &w, &plan, names, nw, NULL, 0); return 0;
}
static int cmd_upgrade(Cfg *c, int argc, char **argv)
{
    begin_mutation(c); lock_and_recover(c);
    World w; world_load(c, &w); Universe u = { w.instp, w.inst.n, w.avail, w.navail }; Plan plan; char err[PM_ERRLEN] = {0};
    if (resolve_upgrade(&u, (const char **)argv, (size_t)argc, &plan, err) != 0) die("cannot upgrade: %s", err);
    if (plan.ninstall == 0) { printf("everything is up to date\n"); return 0; }
    printf("plan:\n"); print_plan(&plan);
    if (c->dry) { printf("(dry run; nothing changed)\n"); return 0; }
    execute(c, &w, &plan, NULL, 0, NULL, 0); return 0;
}
static int cmd_remove(Cfg *c, int argc, char **argv)
{
    if (argc < 1) { usage(); return 2; }
    begin_mutation(c); lock_and_recover(c);
    World w; world_load(c, &w); Universe u = { w.instp, w.inst.n, w.avail, w.navail }; Plan plan; char err[PM_ERRLEN] = {0};
    if (resolve_remove(&u, (const char **)argv, (size_t)argc, c->force, &plan, err) != 0) die("%s", err);
    printf("plan:\n"); print_plan(&plan);
    if (c->dry) { printf("(dry run; nothing changed)\n"); return 0; }
    execute(c, &w, &plan, NULL, 0, argv, (size_t)argc); return 0;
}
static int cmd_recover(Cfg *c)
{
    begin_mutation(c); char err[PM_ERRLEN] = {0};
    if (pm_lock(c->root, &g_lockfd, err) != 0) die("%s", err);
    int r = txn_recover(c->root, err); if (r < 0) die("%s", err);
    puts(r == 0 ? "nothing to recover" : r == 1 ? "rolled back an interrupted transaction" : "completed a committed transaction"); return 0;
}

/* ---------- queries (unprivileged) ---------- */
static int cmd_list(Cfg *c)
{
    Installed in; char err[PM_ERRLEN] = {0}; if (db_load(c->root, &in, err) != 0) die("%s", err);
    for (size_t i = 0; i < in.n; i++) printf("%s %s\n", in.pkgs[i].name, in.pkgs[i].version);
    return 0;
}
static int cmd_info(Cfg *c, int argc, char **argv, int files)
{
    if (argc != 1) { usage(); return 2; }
    Installed in; char err[PM_ERRLEN] = {0}; if (db_load(c->root, &in, err) != 0) die("%s", err);
    const Meta *m = db_find(&in, argv[0]); if (!m) die("%s is not installed", argv[0]);
    if (files) { for (size_t i = 0; i < m->nentries; i++) if (m->entries[i].type != 'd') printf("/%s\n", m->entries[i].path); }
    else print_meta(m, 0);
    return 0;
}
static int cmd_owner(Cfg *c, int argc, char **argv)
{
    if (argc != 1) { usage(); return 2; }
    Installed in; char err[PM_ERRLEN] = {0}; if (db_load(c->root, &in, err) != 0) die("%s", err);
    const char *p = argv[0]; while (*p == '/') p++;
    const Meta *m = db_owner(&in, p, NULL); if (!m) { printf("%s is not owned by any package\n", argv[0]); return 1; }
    printf("%s is owned by %s %s\n", argv[0], m->name, m->version); return 0;
}
static int cmd_check(Cfg *c, int argc, char **argv)
{
    Installed in; char err[PM_ERRLEN] = {0}; if (db_load(c->root, &in, err) != 0) die("%s", err);
    int bad = 0, checked = 0;
    for (size_t i = 0; i < in.n; i++) {
        const Meta *m = &in.pkgs[i];
        if (argc) { int sel = 0; for (int k = 0; k < argc; k++) if (!strcmp(argv[k], m->name)) sel = 1; if (!sel) continue; }
        for (size_t j = 0; j < m->nentries; j++) {
            const Entry *e = &m->entries[j]; struct stat st; checked++;
            if (fs_lstat(c->root, e->path, &st) != 0) { printf("MISSING   %s (%s)\n", e->path, m->name); bad++; continue; }
            if (e->type == 'd') { if (!S_ISDIR(st.st_mode)) { printf("TYPE      %s (%s)\n", e->path, m->name); bad++; } continue; }
            if (e->type == 'l') { char b[4096]; if (!S_ISLNK(st.st_mode) || fs_readlink(c->root, e->path, b, sizeof b) != 0 || strcmp(b, e->target)) { printf("CHANGED   %s (%s)\n", e->path, m->name); bad++; } continue; }
            uint8_t h[32];
            if (!S_ISREG(st.st_mode) || fs_hash(c->root, e->path, h) != 0 || sodium_memcmp(h, e->sha, 32) != 0) { printf("MODIFIED  %s (%s)\n", e->path, m->name); bad++; }
            else if ((st.st_mode & 0777) != e->mode) { printf("MODE      %s (%s) is %04o, expected %04o\n", e->path, m->name, st.st_mode & 0777, e->mode); bad++; }
        }
    }
    printf("%d entries checked, %d problems\n", checked, bad); return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    Cfg c; memset(&c, 0, sizeof c); c.root = "/";
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--root")) c.root = opt_value(&i, argc, argv, "--root");
        else if (!strcmp(argv[i], "--keys")) c.keydir = opt_value(&i, argc, argv, "--keys");
        else if (!strcmp(argv[i], "--dry-run")) c.dry = 1;
        else if (!strcmp(argv[i], "--force")) c.force = 1;
        else if (!strcmp(argv[i], "--allow-unsigned")) c.allow_unsigned = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else { usage(); return 2; }
    }
    if (i >= argc) { usage(); return 2; }
    if (crypto_setup() != 0) die("cannot initialise libsodium");
    const char *cmd = argv[i++]; int n = argc - i; char **a = argv + i;
    if (!strcmp(cmd, "keygen")) return cmd_keygen(n, a);
    if (!strcmp(cmd, "build")) return cmd_build(n, a);
    if (!strcmp(cmd, "index")) return cmd_index(n, a);
    if (!strcmp(cmd, "inspect")) return cmd_inspect(n, a);
    load_cfg(&c);
    if (!strcmp(cmd, "verify")) return cmd_verify(&c, n, a);
    if (!strcmp(cmd, "update")) return cmd_update(&c);
    if (!strcmp(cmd, "install")) return cmd_install(&c, n, a);
    if (!strcmp(cmd, "upgrade")) return cmd_upgrade(&c, n, a);
    if (!strcmp(cmd, "remove")) return cmd_remove(&c, n, a);
    if (!strcmp(cmd, "recover")) return cmd_recover(&c);
    if (!strcmp(cmd, "list")) return cmd_list(&c);
    if (!strcmp(cmd, "info")) return cmd_info(&c, n, a, 0);
    if (!strcmp(cmd, "files")) return cmd_info(&c, n, a, 1);
    if (!strcmp(cmd, "owner")) return cmd_owner(&c, n, a);
    if (!strcmp(cmd, "check")) return cmd_check(&c, n, a);
    usage(); return 2;
}
