/*
 * md5.h — Minimal MD5 (public domain, Solar Designer)
 * Not for cryptographic use — only content hashing/distribution.
 */

#ifndef FBMQ_MD5_H
#define FBMQ_MD5_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t lo, hi;
    uint32_t a, b, c, d;
    unsigned char buffer[64];
    uint32_t block[16];
} fbmq_md5_ctx;

void fbmq_md5_init(fbmq_md5_ctx *ctx);
void fbmq_md5_update(fbmq_md5_ctx *ctx, const void *data, size_t size);
void fbmq_md5_final(unsigned char result[16], fbmq_md5_ctx *ctx);

#endif /* FBMQ_MD5_H */
