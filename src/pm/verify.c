#define _GNU_SOURCE
#include "verify.h"
#include "util.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int crypto_setup(void) { return sodium_init() < 0 ? -1 : 0; }
void sha_init(Sha *s) { crypto_hash_sha256_init(&s->st); }
void sha_update(Sha *s, const void *p, size_t n) { crypto_hash_sha256_update(&s->st, p, n); }
void sha_final(Sha *s, uint8_t out[32]) { crypto_hash_sha256_final(&s->st, out); }
void sha_buf(const void *p, size_t n, uint8_t out[32]) { crypto_hash_sha256(out, p, n); }

void key_id(const uint8_t pk[32], uint8_t id[PM_KEYID_LEN]) { uint8_t h[32]; sha_buf(pk, 32, h); memcpy(id, h, PM_KEYID_LEN); }

int key_generate(const char *prefix, char *err)
{
    uint8_t pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pk, sk);
    char skh[2 * 64 + 2], pkh[2 * 32 + 2];
    hex_encode(sk, 64, skh); skh[128] = '\n'; skh[129] = 0;
    hex_encode(pk, 32, pkh); pkh[64] = '\n'; pkh[65] = 0;
    char *sp = xsprintf("%s.sec", prefix), *pp = xsprintf("%s.pub", prefix);
    int rc = write_file_atomic(sp, skh, strlen(skh), 0600, err);
    if (rc == 0) rc = write_file_atomic(pp, pkh, strlen(pkh), 0644, err);
    sodium_memzero(sk, sizeof sk); sodium_memzero(skh, sizeof skh);
    free(sp); free(pp); return rc;
}
int key_load_secret(const char *path, uint8_t sk[64], char *err)
{
    char *t = read_file(path, NULL, 4096, err); if (!t) return -1;
    char *nl = strpbrk(t, "\r\n"); if (nl) *nl = 0;
    int rc = hex_decode(t, sk, 64);
    sodium_memzero(t, strlen(t)); free(t);
    if (rc) { set_err(err, "%s: not a 64-byte hex secret key", path); return -1; }
    return 0;
}

static char *domain_msg(const char *domain, const void *msg, size_t len, size_t *outlen)
{
    size_t dl = strlen(domain) + 1; char *b = xmalloc(dl + len);
    memcpy(b, domain, dl); memcpy(b + dl, msg, len); *outlen = dl + len; return b;
}
int sign_detached(const char *domain, const void *msg, size_t len, const uint8_t sk[64], uint8_t sig[PM_SIG_LEN])
{
    size_t n; char *b = domain_msg(domain, msg, len, &n);
    int rc = crypto_sign_detached(sig, NULL, (const unsigned char *)b, n, sk); free(b); return rc;
}
int verify_detached(const char *domain, const void *msg, size_t len, const uint8_t pk[32], const uint8_t sig[PM_SIG_LEN])
{
    size_t n; char *b = domain_msg(domain, msg, len, &n);
    int rc = crypto_sign_verify_detached(sig, (const unsigned char *)b, n, pk); free(b); return rc == 0 ? 0 : -1;
}

int keyring_load(const char *dir, Keyring *kr, char *err)
{
    kr->keys = NULL; kr->n = 0;
    DIR *d = opendir(dir);
    if (!d) { if (errno == ENOENT) return 0; set_err(err, "%s: %s", dir, strerror(errno)); return -1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".pub") != 0) continue;
        char *p = xsprintf("%s/%s", dir, e->d_name);
        char *t = read_file(p, NULL, 4096, err); free(p);
        if (!t) { closedir(d); keyring_free(kr); return -1; }
        char buf[65]; size_t i = 0; while (i < 64 && t[i]) { buf[i] = t[i]; i++; } buf[i] = 0; free(t);
        TrustedKey k;
        if (hex_decode(buf, k.pk, 32) != 0) { set_err(err, "%s/%s: not a 32-byte hex public key", dir, e->d_name); closedir(d); keyring_free(kr); return -1; }
        key_id(k.pk, k.id);
        kr->keys = xrealloc(kr->keys, (kr->n + 1) * sizeof *kr->keys); kr->keys[kr->n++] = k;
    }
    closedir(d); return 0;
}
const TrustedKey *keyring_find(const Keyring *kr, const uint8_t id[PM_KEYID_LEN])
{
    for (size_t i = 0; i < kr->n; i++) if (memcmp(kr->keys[i].id, id, PM_KEYID_LEN) == 0) return &kr->keys[i];
    return NULL;
}
void keyring_free(Keyring *kr) { free(kr->keys); kr->keys = NULL; kr->n = 0; }
