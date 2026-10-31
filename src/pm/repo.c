#define _GNU_SOURCE
#include "repo.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

void avail_free(Avail *a) { meta_free(&a->meta); free(a->filename); free(a->repo); free(a->localpath); memset(a, 0, sizeof *a); }
void index_free(Index *ix) { for (size_t i = 0; i < ix->n; i++) avail_free(&ix->pkgs[i]); free(ix->pkgs); free(ix->repo_name); memset(ix, 0, sizeof *ix); }

int index_parse(const char *buf, size_t len, Index *ix, char *err)
{
    memset(ix, 0, sizeof *ix);
    if (len == 0 || buf[len - 1] != '\n') { set_err(err, "index must end with a newline"); return -1; }
    for (size_t i = 0; i < len; i++) { unsigned char c = (unsigned char)buf[i]; if (c == 0 || (c < 32 && c != '\n') || c > 126) { set_err(err, "index contains non-printable byte"); return -1; } }
    char *copy = xmalloc(len + 1); memcpy(copy, buf, len); copy[len] = 0;
    int rc = -1, lineno = 0, have_fmt = 0; Avail *cur = NULL; char *save = NULL;
    /* strtok_r would swallow blank separator lines, so split manually. */
    char *p = copy;
    while (*p) {
        char *nl = strchr(p, '\n'); *nl = 0; char *line = p; p = nl + 1; lineno++;
        if (*line == 0) { cur = NULL; continue; }
        char *tok[8]; int n = 0; char *q = line;
        if (!strncmp(line, "summary ", 8)) {
            if (!cur || cur->meta.summary || strlen(line + 8) > 200) { set_err(err, "line %d: unexpected summary", lineno); goto out; }
            cur->meta.summary = xstrdup(line + 8); continue;
        }
        while (1) { if (n == 8) { set_err(err, "line %d: too many fields", lineno); goto out; } tok[n++] = q; char *sp = strchr(q, ' '); if (!sp) break; *sp = 0; q = sp + 1; if (*q == 0 || *q == ' ') { set_err(err, "line %d: empty field", lineno); goto out; } }
        char lerr[PM_ERRLEN] = {0};
        if (!have_fmt) { if (n != 2 || strcmp(tok[0], "format") || strcmp(tok[1], "1")) { set_err(err, "index line 1 must be 'format 1'"); goto out; } have_fmt = 1; continue; }
        if (!strcmp(tok[0], "repo") && !cur) { if (n != 2 || !valid_pkg_name(tok[1]) || ix->repo_name) { set_err(err, "line %d: bad repo line", lineno); goto out; } ix->repo_name = xstrdup(tok[1]); continue; }
        if (!strcmp(tok[0], "timestamp") && !cur) {
            char *e; errno = 0; unsigned long long t = strtoull(n == 2 ? tok[1] : "x", &e, 10);
            if (n != 2 || *e || errno || ix->timestamp) { set_err(err, "line %d: bad timestamp", lineno); goto out; }
            ix->timestamp = t; continue;
        }
        if (!strcmp(tok[0], "package")) {
            if (n != 2) { set_err(err, "line %d: bad package line", lineno); goto out; }
            ix->pkgs = xrealloc(ix->pkgs, (ix->n + 1) * sizeof *ix->pkgs); cur = &ix->pkgs[ix->n++]; memset(cur, 0, sizeof *cur);
            tok[0] = "name"; if (meta_apply_line(&cur->meta, tok, n, lerr) != 0) { set_err(err, "line %d: %s", lineno, lerr); goto out; }
            continue;
        }
        if (!cur) { set_err(err, "line %d: field outside a package stanza", lineno); goto out; }
        if (!strcmp(tok[0], "filename")) { if (n != 2 || !valid_token(tok[1]) || strchr(tok[1], '/') || cur->filename) { set_err(err, "line %d: bad filename", lineno); goto out; } cur->filename = xstrdup(tok[1]); continue; }
        if (!strcmp(tok[0], "size")) { char *e; errno = 0; unsigned long long s = strtoull(n == 2 ? tok[1] : "x", &e, 10); if (n != 2 || *e || errno) { set_err(err, "line %d: bad size", lineno); goto out; } cur->size = s; continue; }
        if (!strcmp(tok[0], "sha256")) { if (n != 2 || hex_decode(tok[1], cur->sha, 32) != 0) { set_err(err, "line %d: bad sha256", lineno); goto out; } continue; }
        if (meta_apply_line(&cur->meta, tok, n, lerr) != 0) { set_err(err, "line %d: %s", lineno, lerr); goto out; }
    }
    (void)save;
    if (!have_fmt || !ix->repo_name || !ix->timestamp) { set_err(err, "index missing format/repo/timestamp"); goto out; }
    for (size_t i = 0; i < ix->n; i++) {
        Avail *a = &ix->pkgs[i];
        if (meta_validate(&a->meta, 0, err) != 0 || !a->filename || a->size == 0) { if (!err || !*err) set_err(err, "incomplete stanza for '%s'", a->meta.name ? a->meta.name : "?"); goto out; }
        a->repo = xstrdup(ix->repo_name);
        for (size_t j = 0; j < i; j++) if (!strcmp(ix->pkgs[j].meta.name, a->meta.name) && !strcmp(ix->pkgs[j].meta.version, a->meta.version)) { set_err(err, "duplicate package %s %s", a->meta.name, a->meta.version); goto out; }
    }
    rc = 0;
out:
    free(copy); if (rc) index_free(ix); return rc;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
char *index_build(const char *repodir, const char *repo_name, uint64_t timestamp, const uint8_t *sk, char **sigtext, char *err)
{
    if (!valid_pkg_name(repo_name) || timestamp == 0) { set_err(err, "bad repo name or timestamp"); return NULL; }
    DIR *d = opendir(repodir); if (!d) { set_err(err, "%s: %s", repodir, strerror(errno)); return NULL; }
    char **names = NULL; size_t nn = 0; struct dirent *e;
    while ((e = readdir(d))) { size_t l = strlen(e->d_name); if (l > 7 && !strcmp(e->d_name + l - 7, ".aether")) { names = xrealloc(names, (nn + 1) * sizeof *names); names[nn++] = xstrdup(e->d_name); } }
    closedir(d); qsort(names, nn, sizeof *names, cmp_str);
    size_t cap = 4096, len = 0; char *out = xmalloc(cap);
    len = (size_t)snprintf(out, cap, "format 1\nrepo %s\ntimestamp %llu\n", repo_name, (unsigned long long)timestamp);
    int ok = 1;
    for (size_t i = 0; i < nn && ok; i++) {
        char *path = xsprintf("%s/%s", repodir, names[i]); PkgFile pf; char perr[PM_ERRLEN];
        if (pkg_open(path, &pf, perr) != 0 || pkg_verify_payload(&pf, perr) != 0) { set_err(err, "%s: %s", names[i], perr); ok = 0; free(path); continue; }
        uint8_t fsha[32]; uint64_t fsize;
        if (pkg_hash_file(path, fsha, &fsize, err) != 0) { ok = 0; pkg_close(&pf); free(path); continue; }
        char *ms = meta_serialize(&pf.meta, 0); char *body = strchr(ms, '\n'); body = body ? body + 1 : ms;   /* drop "format 1" */
        char hex[65]; hex_encode(fsha, 32, hex);
        char *stanza = xsprintf("\npackage %s\n%sfilename %s\nsize %llu\nsha256 %s\n", pf.meta.name, strchr(body, '\n') + 1, names[i], (unsigned long long)fsize, hex);
        if (len + strlen(stanza) + 1 > cap) { cap = (len + strlen(stanza) + 1) * 2; out = xrealloc(out, cap); }
        memcpy(out + len, stanza, strlen(stanza) + 1); len += strlen(stanza);
        free(stanza); free(ms); free(path); pkg_close(&pf);
    }
    for (size_t i = 0; i < nn; i++) free(names[i]);
    free(names);
    if (!ok) { free(out); return NULL; }
    Index chk; char cerr[PM_ERRLEN];
    if (index_parse(out, strlen(out), &chk, cerr) != 0) { set_err(err, "generated index is invalid: %s", cerr); free(out); return NULL; }
    index_free(&chk);
    if (sk && sigtext) {
        uint8_t sig[PM_SIG_LEN], pk[32], id[PM_KEYID_LEN]; char ih[2 * PM_KEYID_LEN + 1], sh[2 * PM_SIG_LEN + 1];
        crypto_sign_ed25519_sk_to_pk(pk, sk); key_id(pk, id);
        if (sign_detached(PM_DOMAIN_INDEX, out, strlen(out), sk, sig) != 0) { set_err(err, "signing failed"); free(out); return NULL; }
        hex_encode(id, PM_KEYID_LEN, ih); hex_encode(sig, PM_SIG_LEN, sh);
        *sigtext = xsprintf("keyid %s\nsig %s\n", ih, sh);
    }
    return out;
}

int index_verify(const char *index, size_t len, const char *sigtext, const Keyring *kr, char *err)
{
    char *c = xstrdup(sigtext); char *save = NULL; uint8_t id[PM_KEYID_LEN], sig[PM_SIG_LEN]; int gi = 0, gs = 0;
    for (char *l = strtok_r(c, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(l, "keyid ", 6) && !gi) gi = hex_decode(l + 6, id, PM_KEYID_LEN) == 0;
        else if (!strncmp(l, "sig ", 4) && !gs) gs = hex_decode(l + 4, sig, PM_SIG_LEN) == 0;
        else { free(c); set_err(err, "malformed index signature file"); return -1; }
    }
    free(c);
    if (!gi || !gs) { set_err(err, "malformed index signature file"); return -1; }
    const TrustedKey *k = keyring_find(kr, id);
    if (!k) { char h[2 * PM_KEYID_LEN + 1]; hex_encode(id, PM_KEYID_LEN, h); set_err(err, "index signed by untrusted key %s", h); return -1; }
    if (verify_detached(PM_DOMAIN_INDEX, index, len, k->pk, sig) != 0) { set_err(err, "BAD SIGNATURE on repository index"); return -1; }
    return 0;
}

static int copy_file(const char *src, const char *dst, char *err)
{
    int in = open(src, O_RDONLY | O_CLOEXEC); if (in < 0) { set_err(err, "%s: %s", src, strerror(errno)); return -1; }
    char *tmp = xsprintf("%s.part", dst); int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) { set_err(err, "%s: %s", tmp, strerror(errno)); close(in); free(tmp); return -1; }
    char buf[65536]; int rc = 0;
    for (;;) { ssize_t r = read(in, buf, sizeof buf); if (r < 0 && errno == EINTR) continue; if (r < 0) { rc = -1; break; } if (r == 0) break;
        for (ssize_t o = 0; o < r;) { ssize_t w = write(out, buf + o, (size_t)(r - o)); if (w < 0 && errno == EINTR) continue; if (w <= 0) { rc = -1; break; } o += w; } if (rc) break; }
    close(in); if (rc == 0 && (fsync(out) != 0)) rc = -1;
    if (close(out) != 0) rc = -1;
    if (rc == 0 && rename(tmp, dst) != 0) rc = -1;
    if (rc) { set_err(err, "copy %s -> %s: %s", src, dst, strerror(errno)); unlink(tmp); }
    free(tmp); return rc;
}
int repo_fetch(const char *base, const char *rel, const char *dst, char *err)
{
    if (!valid_token(rel) || strchr(rel, '/') || !valid_token(base)) { set_err(err, "bad repository location"); return -1; }
    if (!strncmp(base, "http://", 7) || !strncmp(base, "https://", 8)) {
        char *url = xsprintf("%s/%s", base, rel), *tmp = xsprintf("%s.part", dst); pid_t pid = fork();
        if (pid < 0) { set_err(err, "fork: %s", strerror(errno)); free(url); free(tmp); return -1; }
        if (pid == 0) { execlp("wget", "wget", "-q", "-O", tmp, url, (char *)NULL); execlp("curl", "curl", "-fsSL", "-o", tmp, url, (char *)NULL); _exit(127); }
        int st; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || rename(tmp, dst) != 0) { set_err(err, "download failed: %s", url); unlink(tmp); free(url); free(tmp); return -1; }
        free(url); free(tmp); return 0;
    }
    const char *dir = !strncmp(base, "file://", 7) ? base + 7 : base;
    if (dir[0] != '/') { set_err(err, "repository must be an absolute path, file:// or http(s):// URL"); return -1; }
    char *src = xsprintf("%s/%s", dir, rel); int rc = copy_file(src, dst, err); free(src); return rc;
}
