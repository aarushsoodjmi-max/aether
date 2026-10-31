/* Unit tests for aether-pm modules: version, metadata parsing, package container, index, resolver. */
#define _GNU_SOURCE
#include "repo.h"
#include "resolve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int fails, total;
#define T(cond, name) do { total++; if (cond) printf("PASS %s\n", name); else { printf("FAIL %s (line %d)\n", name, __LINE__); fails++; } } while (0)

static int pm(const char *txt) { Meta m; char e[PM_ERRLEN]; int rc = meta_parse(txt, strlen(txt), &m, 1, e); if (rc == 0) meta_free(&m); return rc; }
static const char *HDR = "format 1\nname a\nversion 1.0\narch x86_64\nlibc none\n";
static const char *H1 = "0000000000000000000000000000000000000000000000000000000000000000";
static int pmx(const char *tail) { char *s = xsprintf("%s%s", HDR, tail); int rc = pm(s); free(s); return rc; }

static void test_versions(void)
{
    T(ver_cmp("1.0", "1.0") == 0, "ver: equal");
    T(ver_cmp("1.9", "1.10") < 0, "ver: numeric not lexical");
    T(ver_cmp("1.0~rc1", "1.0") < 0, "ver: tilde sorts before release");
    T(ver_cmp("1.0", "1.0-1") < 0, "ver: revision adds");
    T(ver_cmp("1:0.1", "2.0") > 0, "ver: epoch wins");
    T(ver_cmp("2.39-0ubuntu8.9", "2.39-0ubuntu8.10") < 0, "ver: debian-style revisions");
    T(ver_cmp("1.0a", "1.0") > 0, "ver: letter suffix newer");
    char *t1[] = { "foo", ">=", "1.2" }; Atom a; char e[PM_ERRLEN];
    T(atom_parse(t1, 3, &a, e) == 0 && atom_satisfied_by(&a, "1.3") && !atom_satisfied_by(&a, "1.1") && atom_satisfied_by(&a, "1.2"), "atom: >= semantics");
    atom_free(&a);
    char *t2[] = { "foo", "=>", "1" }; T(atom_parse(t2, 3, &a, e) != 0, "atom: bad operator rejected");
    char *t3[] = { "Foo" }; T(atom_parse(t3, 1, &a, e) != 0, "atom: uppercase name rejected");
}

static void test_meta(void)
{
    char ok[512]; snprintf(ok, sizeof ok, "file 0644 3 %s usr/share/a/x\ndir 0755 usr/share/a\n", H1);
    T(pmx(ok) == 0, "meta: minimal valid");
    T(pm("name a\n") != 0, "meta: format line required first");
    T(pm("format 2\nname a\n") != 0, "meta: unknown format rejected");
    T(pmx("bogus 1\n") != 0, "meta: unknown key rejected");
    T(pmx("dir 0755 usr/../etc\n") != 0, "meta: '..' path rejected");
    T(pmx("dir 0755 /usr\n") != 0, "meta: absolute path rejected");
    T(pmx("dir 0755 bin\n") != 0, "meta: /bin alias path rejected (usr-merge)");
    T(pmx("dir 0755 lib64/x\n") != 0, "meta: /lib64 alias path rejected");
    T(pmx("dir 0755 var/lib/aether/pm/db/x\n") != 0, "meta: package database path rejected");
    T(pmx("dir 0755 etc/aether/keys\n") != 0, "meta: trust store path rejected");
    T(pmx("dir 0755 usr//x\n") != 0, "meta: empty component rejected");
    char sd[256]; snprintf(sd, sizeof sd, "file 4755 1 %s usr/bin/x\n", H1); T(pmx(sd) != 0, "meta: setuid mode rejected");
    char dup[512]; snprintf(dup, sizeof dup, "file 0644 1 %s usr/x\nfile 0644 1 %s usr/x\n", H1, H1); T(pmx(dup) != 0, "meta: duplicate path rejected");
    T(pm("format 1\nname a\nversion 1.0\narch arm64\nlibc none\ndir 0755 usr\n") != 0, "meta: unsupported arch rejected");
    T(pm("format 1\nname a\nversion 1.0\narch x86_64\nlibc musl-1.2\ndir 0755 usr\n") != 0, "meta: unsupported libc rejected");
    T(pm("format 1\nname a\nversion 1.0\narch x86_64\nlibc none\ndir 0755 usr") != 0, "meta: missing trailing newline rejected");
    T(pm("format 1\nname a\nversion 1.0\narch x86_64\nlibc none\ndir  0755 usr\n") != 0, "meta: double space rejected");
    T(pm("format 1\nname a\x01\nversion 1.0\narch x86_64\nlibc none\ndir 0755 usr\n") != 0, "meta: control byte rejected");
    T(pmx("depends b >= 1\ndepends c\nconflicts d << 3\nprovides e = 2\nneeds-soname libx.so.1\nprovides-soname liby.so.2\ndir 0755 usr\n") == 0, "meta: relations parse");
    T(pmx("provides e >= 2\ndir 0755 usr\n") != 0, "meta: provides only allows '='");
    T(pmx("needs-soname lib/x.so\ndir 0755 usr\n") != 0, "meta: soname with slash rejected");
    T(pmx("link ../a usr/l\ndir 0755 usr\n") == 0, "meta: relative symlink ok");
    Meta m; char e[PM_ERRLEN]; char *txt = xsprintf("%sdepends z >= 2\ndir 0755 usr\nfile 0755 5 %s usr/f\nlink f usr/g\n", HDR, H1);
    T(meta_parse(txt, strlen(txt), &m, 1, e) == 0, "meta: parse for roundtrip");
    char *again = meta_serialize(&m, 1); T(strcmp(again, txt) == 0, "meta: canonical serialize round-trips"); T(m.payload_len == 5, "meta: payload length summed");
    free(again); free(txt); meta_free(&m);
}

static char tmpd[] = "/tmp/aether-pmunit-XXXXXX";
static void put(const char *rel, const char *content, mode_t mode)
{
    char p[512]; snprintf(p, sizeof p, "%s/%s", tmpd, rel); char *s = strrchr(p, '/'); *s = 0; char cmd[600]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", p); if (system(cmd)) exit(1); *s = '/';
    FILE *f = fopen(p, "w"); if (!f) exit(1); fputs(content, f); fclose(f); chmod(p, mode);
}
static void test_container(void)
{
    char err[PM_ERRLEN]; uint8_t sk[64], pk[32]; crypto_sign_keypair(pk, sk);
    put("tree/usr/bin/hello", "#!/bin/sh\necho hi\n", 0755); put("tree/usr/share/hello/readme", "hello\n", 0644); put("tree/etc/hello.conf", "", 0644);
    char info[512], tree[512], out[512]; snprintf(info, sizeof info, "%s/INFO", tmpd); snprintf(tree, sizeof tree, "%s/tree", tmpd); snprintf(out, sizeof out, "%s/hello.aether", tmpd);
    FILE *f = fopen(info, "w"); fputs("format 1\nname hello\nversion 1.0-1\narch x86_64\nlibc none\nsummary test package\n", f); fclose(f);
    T(pkg_build(info, tree, out, sk, err) == 0, "pkg: build");
    PkgFile p; T(pkg_open(out, &p, err) == 0, "pkg: open");
    T(p.meta.nentries >= 6 && p.is_signed, "pkg: manifest has dirs+files, signed");
    Keyring kr; kr.n = 1; TrustedKey tk; memcpy(tk.pk, pk, 32); key_id(pk, tk.id); kr.keys = &tk;
    T(pkg_verify_signature(&p, &kr, err) == 0, "pkg: signature verifies with trusted key");
    T(pkg_verify_payload(&p, err) == 0, "pkg: payload hashes verify");
    Keyring empty = {0}; T(pkg_verify_signature(&p, &empty, err) != 0 && strstr(err, "untrusted"), "pkg: untrusted key rejected");
    char out2[512]; snprintf(out2, sizeof out2, "%s/hello2.aether", tmpd); T(pkg_build(info, tree, out2, sk, err) == 0, "pkg: second build");
    uint8_t h1[32], h2[32]; pkg_hash_file(out, h1, NULL, err); pkg_hash_file(out2, h2, NULL, err); T(memcmp(h1, h2, 32) == 0, "pkg: build is deterministic (byte-identical)");
    pkg_close(&p);
    /* tampering */
    char cmd[1200]; snprintf(cmd, sizeof cmd, "cp %s %s/t1.aether && printf 'X' | dd of=%s/t1.aether bs=1 seek=$(( $(stat -c %%s %s/t1.aether) - 1 )) conv=notrunc 2>/dev/null", out, tmpd, tmpd, tmpd);
    if (system(cmd)) exit(1); char t1[512]; snprintf(t1, sizeof t1, "%s/t1.aether", tmpd);
    T(pkg_open(t1, &p, err) == 0 && pkg_verify_payload(&p, err) != 0 && strstr(err, "checksum"), "pkg: payload byte flip detected"); pkg_close(&p);
    snprintf(cmd, sizeof cmd, "cp %s %s/t2.aether && printf 'Z' | dd of=%s/t2.aether bs=1 seek=100 conv=notrunc 2>/dev/null", out, tmpd, tmpd); if (system(cmd)) exit(1);
    char t2[512]; snprintf(t2, sizeof t2, "%s/t2.aether", tmpd);
    int o = pkg_open(t2, &p, err); T(o != 0 || pkg_verify_signature(&p, &kr, err) != 0, "pkg: metadata modification detected (parse or signature)"); if (o == 0) pkg_close(&p);
    snprintf(cmd, sizeof cmd, "cp %s %s/t3.aether && truncate -s -5 %s/t3.aether", out, tmpd, tmpd); if (system(cmd)) exit(1);
    char t3[512]; snprintf(t3, sizeof t3, "%s/t3.aether", tmpd); T(pkg_open(t3, &p, err) != 0 && strstr(err, "size mismatch"), "pkg: truncation detected");
    snprintf(cmd, sizeof cmd, "cp %s %s/t4.aether && printf 'junk' >> %s/t4.aether", out, tmpd, tmpd); if (system(cmd)) exit(1);
    char t4[512]; snprintf(t4, sizeof t4, "%s/t4.aether", tmpd); T(pkg_open(t4, &p, err) != 0, "pkg: trailing data detected");
    snprintf(cmd, sizeof cmd, "head -c 40 /dev/zero > %s/t5.aether", tmpd); if (system(cmd)) exit(1);
    char t5[512]; snprintf(t5, sizeof t5, "%s/t5.aether", tmpd); T(pkg_open(t5, &p, err) != 0, "pkg: garbage rejected");
    /* repository index */
    char rd[512]; snprintf(rd, sizeof rd, "%s/repo", tmpd); snprintf(cmd, sizeof cmd, "mkdir -p %s && cp %s %s/", rd, out, rd); if (system(cmd)) exit(1);
    char *sig = NULL; char *ix = index_build(rd, "unit", 1000, sk, &sig, err); T(ix != NULL, "index: build");
    T(index_verify(ix, strlen(ix), sig, &kr, err) == 0, "index: signature verifies");
    char *bad = xstrdup(ix); bad[strlen(bad) / 2] ^= 1; T(index_verify(bad, strlen(bad), sig, &kr, err) != 0, "index: tamper detected");
    Index idx; T(index_parse(ix, strlen(ix), &idx, err) == 0 && idx.n == 1 && idx.timestamp == 1000 && !strcmp(idx.pkgs[0].meta.name, "hello"), "index: parse");
    index_free(&idx);
    T(index_verify(ix, strlen(ix), sig, &empty, err) != 0, "index: untrusted key rejected");
    free(ix); free(sig); free(bad);
}

/* ----- resolver ----- */
static Avail g_pool[64]; static int g_npool;
static Avail *g_flats[64]; static int g_nflats;   /* universes must outlive the Plans computed from them */
static void pool_free(void) { for (int i = 0; i < g_npool; i++) avail_free(&g_pool[i]); g_npool = 0; for (int i = 0; i < g_nflats; i++) free(g_flats[i]); g_nflats = 0; }
static Avail *mk(const char *name, const char *ver, const char *libc, const char *extra)
{
    Avail *a = &g_pool[g_npool++]; memset(a, 0, sizeof *a); char e[PM_ERRLEN];
    char *t = xsprintf("format 1\nname %s\nversion %s\narch x86_64\nlibc %s\n%sdir 0755 usr\n", name, ver, libc, extra);
    if (meta_parse(t, strlen(t), &a->meta, 1, e) != 0) { printf("fixture error: %s\n%s", e, t); exit(2); }
    a->filename = xstrdup(name); a->repo = xstrdup("r"); free(t); return a;
}
static const char *names(const Plan *p) { static char b[512]; b[0] = 0; for (size_t i = 0; i < p->ninstall; i++) { strcat(b, p->install[i].meta->name); strcat(b, "-"); strcat(b, p->install[i].meta->version); strcat(b, " "); } return b; }
static int run_install(const Meta **inst, size_t ni, Avail **av, size_t na, const char *want, Plan *plan, char *err)
{
    Avail *flat = xmalloc((na + 1) * sizeof *flat); for (size_t i = 0; i < na; i++) flat[i] = *av[i];
    Universe u = { inst, ni, flat, na }; char nm[80]; char *eq = strchr(want, '='); size_t l = eq ? (size_t)(eq - want) : strlen(want); memcpy(nm, want, l); nm[l] = 0;
    char *tok[3] = { nm, "=", eq ? eq + 1 : NULL }; Atom a; if (atom_parse(tok, eq ? 3 : 1, &a, err) != 0) return -2;
    int rc = resolve_install(&u, &a, 1, plan, err);
    atom_free(&a); g_flats[g_nflats++] = flat; return rc;
}
static void test_resolver(void)
{
    char err[PM_ERRLEN]; Plan p;
    Avail *libfoo1 = mk("libfoo", "1.0", "none", "provides-soname libfoo.so.1\n"), *libfoo2 = mk("libfoo", "2.0", "none", "provides-soname libfoo.so.2\n");
    Avail *app1 = mk("app", "1.0", "none", "depends libfoo >= 1.0\n"), *app2 = mk("app", "2.0", "none", "depends libfoo >= 2.0\n");
    Avail *av[] = { libfoo1, libfoo2, app1, app2 };
    T(run_install(NULL, 0, av, 4, "app", &p, err) == 0 && !strcmp(names(&p), "libfoo-2.0 app-2.0 "), "resolve: newest versions, dependencies first");
    plan_free(&p);
    T(run_install(NULL, 0, av, 4, "app=1.0", &p, err) == 0 && !strcmp(names(&p), "libfoo-2.0 app-1.0 "), "resolve: exact version pin pulls newest satisfying dep");
    plan_free(&p);
    T(run_install(NULL, 0, av, 4, "nothing", &p, err) != 0 && strstr(err, "no package provides"), "resolve: unknown package reported");
    /* backtracking: app3 needs libbar (any); libbar 2 conflicts with installed 'legacy'; must fall back to libbar 1 */
    Avail *legacy = mk("legacy", "1.0", "none", ""); Avail *bar1 = mk("libbar", "1.0", "none", ""), *bar2 = mk("libbar", "2.0", "none", "conflicts legacy\n");
    Avail *app3 = mk("app3", "1.0", "none", "depends libbar\n"); Avail *av2[] = { bar1, bar2, app3 }; const Meta *inst[] = { &legacy->meta };
    T(run_install(inst, 1, av2, 3, "app3", &p, err) == 0 && !strcmp(names(&p), "libbar-1.0 app3-1.0 "), "resolve: backtracks past a conflicting newer version");
    plan_free(&p);
    Avail *only2[] = { bar2, app3 }; T(run_install(inst, 1, only2, 2, "app3", &p, err) != 0 && strstr(err, "conflicts"), "resolve: unavoidable conflict explained");
    /* virtual provides + soname */
    Avail *mta = mk("mta-a", "1.0", "none", "provides mail-agent\n"), *client = mk("client", "1.0", "none", "depends mail-agent\n");
    Avail *av3[] = { mta, client }; T(run_install(NULL, 0, av3, 2, "client", &p, err) == 0 && !strcmp(names(&p), "mta-a-1.0 client-1.0 "), "resolve: virtual provides satisfy depends"); plan_free(&p);
    Avail *sob = mk("so-user", "1.0", "none", "needs-soname libfoo.so.2\n"); Avail *av4[] = { libfoo1, libfoo2, sob };
    T(run_install(NULL, 0, av4, 3, "so-user", &p, err) == 0 && !strcmp(names(&p), "libfoo-2.0 so-user-1.0 "), "resolve: needs-soname picks the library package providing it"); plan_free(&p);
    Avail *sob9 = mk("so-user9", "1.0", "none", "needs-soname libnone.so.9\n"); Avail *av5[] = { sob9 };
    T(run_install(NULL, 0, av5, 1, "so-user9", &p, err) != 0 && strstr(err, "shared library libnone.so.9"), "resolve: missing shared library reported by name");
    /* libc ABI mixing */
    Avail *ca = mk("libc-a", "1.0", "glibc-2.39", ""), *cb = mk("app-old", "1.0", "glibc-2.35", ""); const Meta *inst2[] = { &ca->meta }; Avail *av6[] = { cb };
    T(run_install(inst2, 1, av6, 1, "app-old", &p, err) != 0 && strstr(err, "libc ABI mismatch"), "resolve: refuses to mix libc ABIs");
    Avail *cc = mk("app-new", "1.0", "glibc-2.39", ""); Avail *av7[] = { cc }; T(run_install(inst2, 1, av7, 1, "app-new", &p, err) == 0, "resolve: same libc ABI is fine"); plan_free(&p);
    Avail *nolibc = mk("data-only", "1.0", "none", ""); Avail *av8[] = { nolibc }; T(run_install(inst2, 1, av8, 1, "data-only", &p, err) == 0, "resolve: libc-independent packages mix freely"); plan_free(&p);
    /* removal and essential */
    Avail *ess = mk("core", "1.0", "none", "essential yes\n"); Avail *dep = mk("needs-core", "1.0", "none", "depends core\n"); const Meta *inst3[] = { &ess->meta, &dep->meta };
    Universe u = { inst3, 2, NULL, 0 }; const char *rm1[] = { "core" }, *rm2[] = { "needs-core" };
    T(resolve_remove(&u, rm1, 1, 0, &p, err) != 0 && strstr(err, "essential"), "remove: essential package protected");
    T(resolve_remove(&u, rm1, 1, 1, &p, err) == 0, "remove: --force overrides"); plan_free(&p);
    Avail *plain = mk("plain", "1.0", "none", ""); const Meta *inst4[] = { &plain->meta, &dep->meta }; const char *rm3[] = { "plain" };
    Avail *prov = mk("prov", "1.0", "none", ""); const Meta *inst5[] = { &prov->meta, &mk("needs-prov", "1.0", "none", "depends prov\n")->meta }; Universe u5 = { inst5, 2, NULL, 0 }; const char *rm4[] = { "prov" };
    T(resolve_remove(&u5, rm4, 1, 0, &p, err) != 0 && strstr(err, "still requires"), "remove: reverse dependency blocks removal");
    T(resolve_remove(&u5, rm4, 1, 0, &p, err) != 0, "remove: stable error"); (void)rm2; (void)inst4; (void)rm3;
    /* upgrade */
    const Meta *inst6[] = { &libfoo1->meta, &app1->meta }; Universe u6 = { inst6, 2, (Avail[]){ *libfoo1, *libfoo2, *app1, *app2 }, 4 };
    T(resolve_upgrade(&u6, NULL, 0, &p, err) == 0 && p.ninstall == 2 && p.install[0].is_upgrade, "upgrade: all installed packages move to newest"); plan_free(&p);
}

int main(void)
{
    if (crypto_setup() != 0) return 2;
    if (!mkdtemp(tmpd)) return 2;
    test_versions(); test_meta(); test_container(); test_resolver(); pool_free();
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf %s", tmpd); if (system(cmd)) return 2;
    printf("%d/%d unit checks passed\n", total - fails, total); return fails ? 1 : 0;
}
