/*
 * SHA-256 implementation from Git
 * https://github.com/git/git/blob/master/sha256/block/sha256.c
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Standalone header adapted for pup - no external dependencies
 */

#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE 64

typedef struct {
    uint32_t state[8];
    uint64_t size;
    uint32_t offset;
    uint8_t buf[SHA256_BLOCK_SIZE];
} sha256_ctx;

#ifdef __cplusplus
extern "C" {
#endif

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t *digest);

#ifdef __cplusplus
}
#endif

#endif /* SHA256_H */
