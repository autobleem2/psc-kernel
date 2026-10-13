/*
 * sha256.h - a small SHA-256 (FIPS 180-4) for the AutoBleem initramfs init.
 */
#ifndef ABINIT_SHA256_H
#define ABINIT_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32

struct sha256_ctx {
	uint32_t state[8];
	uint64_t length;	/* bytes hashed so far */
	uint8_t block[64];
	size_t used;		/* bytes waiting in block */
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(struct sha256_ctx *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

#endif
