#ifndef AETHER_SHA256_H
#define AETHER_SHA256_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t used;
} Sha256;

void sha256_init(Sha256 *c);
void sha256_update(Sha256 *c, const void *data, size_t n);
void sha256_final(Sha256 *c, unsigned char out[32]);
/* Hex digest (64 chars + NUL) of a buffer. */
void sha256_hex(const void *data, size_t n, char out[65]);

#endif
