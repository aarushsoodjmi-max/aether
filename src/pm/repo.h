/* Repository metadata: signed package index, index build/parse, and file transport.
 * INDEX is canonical text; INDEX.sig is "keyid HEX16\nsig HEX128\n" over  "aether-index-v1" 0x00 INDEX.
 * The index carries full dependency metadata plus each package file's size and SHA-256, so the
 * resolver never has to download packages, and a verified index authenticates every package file. */
#ifndef AETHER_PM_REPO_H
#define AETHER_PM_REPO_H
#include "pkgfile.h"

typedef struct {
    Meta meta;                 /* no manifest */
    char *filename;            /* file name inside the repository (no '/') */
    uint64_t size; uint8_t sha[32];
    char *repo;                /* repository name; NULL for a local file */
    char *localpath;           /* set when the candidate is a user-supplied .aether file */
} Avail;
typedef struct { char *repo_name; uint64_t timestamp; Avail *pkgs; size_t n; } Index;

void avail_free(Avail *a);
void index_free(Index *ix);
int index_parse(const char *buf, size_t len, Index *ix, char *err);
/* Scan the .aether files in REPODIR (each opened and payload-checked) and return INDEX text; fills sigtext when sk != NULL. */
char *index_build(const char *repodir, const char *repo_name, uint64_t timestamp, const uint8_t *sk, char **sigtext, char *err);
int index_verify(const char *index, size_t len, const char *sigtext, const Keyring *kr, char *err);

/* Transport. base is "file:///abs", "/abs", "http://..." or "https://..."; rel must be a plain token.
 * http(s) shells out to wget/curl via execvp (no shell) and is NOT covered by automated tests. */
int repo_fetch(const char *base, const char *rel, const char *dst, char *err);
#endif
