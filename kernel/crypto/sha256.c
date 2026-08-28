/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - SHA-256, HMAC-SHA256 and HKDF.
 *
 * Plain portable C. The SHA extensions would be faster on the CPUs that have
 * them, but this runs in contexts where the FPU and vector state are not saved,
 * and a correct scalar implementation that always works beats a fast one with
 * a footgun. Digest throughput is not the bottleneck in a kernel.
 */
#include <rk/crypto.h>
#include <rk/string.h>
#include <rk/errno.h>

static const u32 K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline u32 ror32(u32 x, unsigned n) { return (x >> n) | (x << (32 - n)); }

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define S0(x) (ror32(x, 2) ^ ror32(x, 13) ^ ror32(x, 22))
#define S1(x) (ror32(x, 6) ^ ror32(x, 11) ^ ror32(x, 25))
#define s0(x) (ror32(x, 7) ^ ror32(x, 18) ^ ((x) >> 3))
#define s1(x) (ror32(x, 17) ^ ror32(x, 19) ^ ((x) >> 10))

static void sha256_block(struct sha256_ctx *c, const u8 *p)
{
	u32 w[64];

	for (int i = 0; i < 16; i++)
		w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) |
		       ((u32)p[i * 4 + 2] << 8) | (u32)p[i * 4 + 3];
	for (int i = 16; i < 64; i++)
		w[i] = s1(w[i - 2]) + w[i - 7] + s0(w[i - 15]) + w[i - 16];

	u32 a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
	u32 e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];

	for (int i = 0; i < 64; i++) {
		u32 t1 = h + S1(e) + CH(e, f, g) + K[i] + w[i];
		u32 t2 = S0(a) + MAJ(a, b, cc);
		h = g; g = f; f = e; e = d + t1;
		d = cc; cc = b; b = a; a = t1 + t2;
	}

	c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
	c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void sha256_init(struct sha256_ctx *c)
{
	c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;
	c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
	c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;
	c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
	c->bitlen = 0;
	c->buflen = 0;
}

void sha256_update(struct sha256_ctx *c, const void *data, size_t len)
{
	const u8 *p = data;

	c->bitlen += (u64)len * 8;
	if (c->buflen) {
		size_t need = SHA256_BLOCK_SIZE - c->buflen;
		size_t take = len < need ? len : need;
		memcpy(c->buf + c->buflen, p, take);
		c->buflen += (u32)take;
		p += take;
		len -= take;
		if (c->buflen == SHA256_BLOCK_SIZE) {
			sha256_block(c, c->buf);
			c->buflen = 0;
		}
	}
	while (len >= SHA256_BLOCK_SIZE) {
		sha256_block(c, p);
		p += SHA256_BLOCK_SIZE;
		len -= SHA256_BLOCK_SIZE;
	}
	if (len) {
		memcpy(c->buf, p, len);
		c->buflen = (u32)len;
	}
}

void sha256_final(struct sha256_ctx *c, u8 out[SHA256_DIGEST_SIZE])
{
	u64 bits = c->bitlen;
	u8  pad = 0x80;

	sha256_update(c, &pad, 1);
	c->bitlen = bits;   /* padding must not extend the counted length */
	pad = 0;
	while (c->buflen != 56) {
		sha256_update(c, &pad, 1);
		c->bitlen = bits;
	}
	u8 lenbe[8];
	for (int i = 0; i < 8; i++)
		lenbe[i] = (u8)(bits >> (56 - i * 8));
	sha256_update(c, lenbe, 8);

	for (int i = 0; i < 8; i++) {
		out[i * 4]     = (u8)(c->state[i] >> 24);
		out[i * 4 + 1] = (u8)(c->state[i] >> 16);
		out[i * 4 + 2] = (u8)(c->state[i] >> 8);
		out[i * 4 + 3] = (u8)c->state[i];
	}
	rk_secure_zero(c->buf, sizeof(c->buf));
}

void sha256(const void *data, size_t len, u8 out[SHA256_DIGEST_SIZE])
{
	struct sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, data, len);
	sha256_final(&c, out);
}

/* ------------------------------------------------------------------ HMAC */

void hmac_sha256_init(struct hmac_sha256_ctx *c, const void *key, size_t keylen)
{
	u8 k[SHA256_BLOCK_SIZE];
	u8 pad[SHA256_BLOCK_SIZE];

	memset(k, 0, sizeof(k));
	if (keylen > SHA256_BLOCK_SIZE)
		sha256(key, keylen, k);
	else
		memcpy(k, key, keylen);

	for (int i = 0; i < SHA256_BLOCK_SIZE; i++)
		pad[i] = k[i] ^ 0x36;
	sha256_init(&c->inner);
	sha256_update(&c->inner, pad, sizeof(pad));

	for (int i = 0; i < SHA256_BLOCK_SIZE; i++)
		pad[i] = k[i] ^ 0x5c;
	sha256_init(&c->outer);
	sha256_update(&c->outer, pad, sizeof(pad));

	rk_secure_zero(k, sizeof(k));
	rk_secure_zero(pad, sizeof(pad));
}

void hmac_sha256_update(struct hmac_sha256_ctx *c, const void *data, size_t len)
{
	sha256_update(&c->inner, data, len);
}

void hmac_sha256_final(struct hmac_sha256_ctx *c, u8 out[SHA256_DIGEST_SIZE])
{
	u8 inner[SHA256_DIGEST_SIZE];
	sha256_final(&c->inner, inner);
	sha256_update(&c->outer, inner, sizeof(inner));
	sha256_final(&c->outer, out);
	rk_secure_zero(inner, sizeof(inner));
}

void hmac_sha256(const void *key, size_t keylen, const void *data, size_t len,
                 u8 out[SHA256_DIGEST_SIZE])
{
	struct hmac_sha256_ctx c;
	hmac_sha256_init(&c, key, keylen);
	hmac_sha256_update(&c, data, len);
	hmac_sha256_final(&c, out);
}

/* ------------------------------------------------------------------ HKDF */

void hkdf_sha256(const void *salt, size_t saltlen,
                 const void *ikm, size_t ikmlen,
                 const void *info, size_t infolen,
                 u8 *out, size_t outlen)
{
	u8 prk[SHA256_DIGEST_SIZE];
	u8 zero_salt[SHA256_DIGEST_SIZE];

	if (!salt || !saltlen) {
		memset(zero_salt, 0, sizeof(zero_salt));
		salt = zero_salt;
		saltlen = sizeof(zero_salt);
	}
	hmac_sha256(salt, saltlen, ikm, ikmlen, prk);   /* extract */

	/* expand */
	u8 t[SHA256_DIGEST_SIZE];
	size_t tlen = 0, done = 0;
	for (u8 counter = 1; done < outlen; counter++) {
		struct hmac_sha256_ctx c;
		hmac_sha256_init(&c, prk, sizeof(prk));
		if (tlen)
			hmac_sha256_update(&c, t, tlen);
		if (infolen)
			hmac_sha256_update(&c, info, infolen);
		hmac_sha256_update(&c, &counter, 1);
		hmac_sha256_final(&c, t);
		tlen = sizeof(t);

		size_t n = outlen - done < tlen ? outlen - done : tlen;
		memcpy(out + done, t, n);
		done += n;
	}
	rk_secure_zero(prk, sizeof(prk));
	rk_secure_zero(t, sizeof(t));
}
