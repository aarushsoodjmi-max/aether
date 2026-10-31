/* Cryptographic verification. All primitives come from libsodium (SHA-256, Ed25519);
 * nothing here implements cryptography, it only frames inputs with domain separation. */
#ifndef AETHER_PM_VERIFY_H
#define AETHER_PM_VERIFY_H
#include <stddef.h>
#include <stdint.h>
#include <sodium.h>

#define PM_KEYID_LEN 8
#define PM_SIG_LEN 64
#define PM_DOMAIN_PKG "aether-pkg-v1"     /* signed message = domain \0 meta-bytes */
#define PM_DOMAIN_INDEX "aether-index-v1"  /* signed message = domain \0 index-bytes */

int crypto_setup(void);                                    /* 0 on success */

typedef struct { crypto_hash_sha256_state st; } Sha;
void sha_init(Sha *s);
void sha_update(Sha *s, const void *p, size_t n);
void sha_final(Sha *s, uint8_t out[32]);
void sha_buf(const void *p, size_t n, uint8_t out[32]);

void key_id(const uint8_t pk[32], uint8_t id[PM_KEYID_LEN]);
int key_generate(const char *prefix, char *err);           /* writes PREFIX.sec (0600) and PREFIX.pub */
int key_load_secret(const char *path, uint8_t sk[64], char *err);

int sign_detached(const char *domain, const void *msg, size_t len, const uint8_t sk[64], uint8_t sig[PM_SIG_LEN]);
int verify_detached(const char *domain, const void *msg, size_t len, const uint8_t pk[32], const uint8_t sig[PM_SIG_LEN]);

typedef struct { uint8_t id[PM_KEYID_LEN]; uint8_t pk[32]; } TrustedKey;
typedef struct { TrustedKey *keys; size_t n; } Keyring;
int keyring_load(const char *dir, Keyring *kr, char *err);  /* every *.pub: 64 hex chars (+ optional trailing text) */
const TrustedKey *keyring_find(const Keyring *kr, const uint8_t id[PM_KEYID_LEN]);
void keyring_free(Keyring *kr);
#endif
