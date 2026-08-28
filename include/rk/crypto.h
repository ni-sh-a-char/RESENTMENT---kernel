/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - cryptographic primitives.
 *
 * Deliberately small and boring. The interesting, novel layer is Kaalka
 * (rk/kaalka.h), which supplies the *temporal* dimension: what may be used,
 * and until when. This file supplies the strength underneath it. Mixing the
 * two is the design: Kaalka decides the epoch, SHA-256 and ChaCha20 make the
 * result unforgeable.
 *
 * All routines are constant-time with respect to secret data and use no
 * floating point, so they are safe in interrupt context.
 */
#pragma once

#include <rk/types.h>
#include <rk/compiler.h>

/* ------------------------------------------------------------- SHA-256 */

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

struct sha256_ctx {
	u32  state[8];
	u64  bitlen;
	u8   buf[SHA256_BLOCK_SIZE];
	u32  buflen;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, size_t len);
void sha256_final(struct sha256_ctx *c, u8 out[SHA256_DIGEST_SIZE]);
void sha256(const void *data, size_t len, u8 out[SHA256_DIGEST_SIZE]);

/* ---------------------------------------------------------- HMAC / HKDF */

struct hmac_sha256_ctx {
	struct sha256_ctx inner, outer;
};

void hmac_sha256_init(struct hmac_sha256_ctx *c, const void *key, size_t keylen);
void hmac_sha256_update(struct hmac_sha256_ctx *c, const void *data, size_t len);
void hmac_sha256_final(struct hmac_sha256_ctx *c, u8 out[SHA256_DIGEST_SIZE]);
void hmac_sha256(const void *key, size_t keylen, const void *data, size_t len,
                 u8 out[SHA256_DIGEST_SIZE]);

void hkdf_sha256(const void *salt, size_t saltlen,
                 const void *ikm, size_t ikmlen,
                 const void *info, size_t infolen,
                 u8 *out, size_t outlen);

/* ------------------------------------------------------------ ChaCha20 */

#define CHACHA20_KEY_SIZE   32
#define CHACHA20_NONCE_SIZE 12

struct chacha20_ctx {
	u32 state[16];
};

void chacha20_init(struct chacha20_ctx *c, const u8 key[CHACHA20_KEY_SIZE],
                   const u8 nonce[CHACHA20_NONCE_SIZE], u32 counter);
void chacha20_xor(struct chacha20_ctx *c, const u8 *in, u8 *out, size_t len);
void chacha20_keystream(struct chacha20_ctx *c, u8 *out, size_t len);

/* --------------------------------------------------------------- CSPRNG */

/* ChaCha20-based, reseeded from hardware entropy, interrupt timing jitter and
 * the runtime graph digest, so entropy quality does not depend on the CPU
 * having RDRAND (phones and boards often do not expose one). */
void   rk_random_init(void);
void   rk_random_bytes(void *buf, size_t len);
u64    rk_random_u64(void);
u32    rk_random_u32(void);
void   rk_random_add_entropy(const void *data, size_t len, u32 estimated_bits);
u32    rk_random_entropy_bits(void);

/* --------------------------------------------------------------- CRC / hash */

u32 rk_crc32(u32 seed, const void *data, size_t len);
u64 rk_fnv1a(const void *data, size_t len);
u64 rk_siphash(const void *data, size_t len, const u8 key[16]);

/* ----------------------------------------------------------- hex helpers */

size_t rk_hex_encode(char *dst, size_t dstlen, const void *src, size_t srclen);
int    rk_hex_decode(void *dst, size_t dstlen, const char *src);
size_t rk_base64_encode(char *dst, size_t dstlen, const void *src, size_t srclen);
int    rk_base64_decode(void *dst, size_t dstlen, const char *src, size_t *outlen);

void rk_crypto_init(void);
int  rk_crypto_selftest(void);   /* known-answer tests; run at boot */
