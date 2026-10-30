/*
 * Minimal SHA-256 (FIPS 180-4) for verifying file contents in tests.
 */

#ifndef EHFS_SHA256_H
#define EHFS_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t state[8];
	uint64_t length;
	uint8_t block[64];
	size_t used;
} sha256_ctx;

void sha256_init(sha256_ctx* c);
void sha256_update(sha256_ctx* c, const void* data, size_t n);
void sha256_final(sha256_ctx* c, uint8_t digest[32]);

#endif
