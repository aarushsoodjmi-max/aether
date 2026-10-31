/* .aether package format v1: parsing, validation, reading and building. No installation logic here.
 *
 *   offset 0   "AETHERPK"            8 bytes magic
 *          8   u32le format (1)
 *         12   u32le meta_len       (<= 16 MiB)
 *         16   u8[8]  signing key id  (all zero = unsigned)
 *         24   u8[64] Ed25519 signature over  "aether-pkg-v1" 0x00 meta
 *         88   meta  (canonical text, see docs/aether-package-format.md)
 *      88+meta payload: contents of every `file` entry, concatenated in manifest order
 * The file size must equal 88 + meta_len + sum(file sizes) exactly. */
#ifndef AETHER_PM_PKGFILE_H
#define AETHER_PM_PKGFILE_H
#include <stdint.h>
#include <stddef.h>
#include "version.h"
#include "verify.h"
#include "util.h"

#define PKG_MAGIC "AETHERPK"
#define PKG_HDR_LEN 88
#define PKG_META_MAX (16u * 1024u * 1024u)

typedef struct { char type; unsigned mode; uint64_t size; uint8_t sha[32]; char *path; char *target; uint64_t off; } Entry; /* type: d f l */
typedef struct { char *name; char *ver; } Prov;
typedef struct {
    char *name, *version, *arch, *libc, *summary;
    int essential;
    Atom *depends; size_t ndepends;
    Atom *conflicts; size_t nconflicts;
    Prov *provides; size_t nprovides;
    char **needs_so; size_t nneeds_so;
    char **provides_so; size_t nprovides_so;
    Entry *entries; size_t nentries;
    uint64_t payload_len;
} Meta;

void meta_free(Meta *m);
/* Apply one non-manifest line (already split on single spaces). Shared by package meta and repository index. */
int meta_apply_line(Meta *m, char **tok, int n, char *err);
int meta_validate(const Meta *m, int need_manifest, char *err);
int meta_parse(const char *buf, size_t len, Meta *m, int allow_manifest, char *err);
char *meta_serialize(const Meta *m, int with_manifest);   /* canonical text */
int path_allowed_for_package(const char *path);            /* policy: no /bin,/lib aliases, no trust store or pm database */

typedef struct {
    Meta meta; int fd; char *path;
    uint8_t *raw_meta; size_t raw_meta_len;
    uint8_t keyid[PM_KEYID_LEN]; uint8_t sig[PM_SIG_LEN]; int is_signed;
    uint64_t payload_off; uint8_t meta_sha[32]; uint8_t file_sha[32];
} PkgFile;

int pkg_open(const char *path, PkgFile *p, char *err);     /* header + metadata only; O(meta) */
void pkg_close(PkgFile *p);
int pkg_verify_signature(const PkgFile *p, const Keyring *kr, char *err);
int pkg_verify_payload(const PkgFile *p, char *err);       /* hash every file entry (streaming) */
int pkg_hash_file(const char *path, uint8_t out[32], uint64_t *size, char *err);

/* Build a package from INFO (name/version/arch/libc/depends... no manifest) and a payload tree.
 * sk may be NULL (unsigned). Deterministic: sorted entries, no timestamps, root-owned semantics. */
int pkg_build(const char *infopath, const char *rootdir, const char *out, const uint8_t *sk, char *err);
#endif
