/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - non-cryptographic hashes and encodings.
 *
 * CRC32 and FNV are for integrity and hash tables; SipHash is for anything
 * keyed by attacker-influenced input, because a hash table with a predictable
 * hash is a denial-of-service primitive, and this kernel hands hash tables to
 * a scripting language.
 */
#include <rk/crypto.h>
#include <rk/string.h>
#include <rk/errno.h>
#include <rk/log.h>

/* Bitwise CRC32 with the reflected polynomial. No 1 KiB table: the kernel uses
 * this for occasional integrity checks, not for bulk throughput, and the table
 * would cost more cache than the loop costs cycles. */
u32 rk_crc32(u32 seed, const void *data, size_t len)
{
	const u8 *p = data;
	u32 crc = ~seed;

	for (size_t i = 0; i < len; i++) {
		crc ^= p[i];
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (u32)(-(s32)(crc & 1)));
	}
	return ~crc;
}

u64 rk_fnv1a(const void *data, size_t len)
{
	const u8 *p = data;
	u64 h = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

#define SIP_ROTL(x, b) (((x) << (b)) | ((x) >> (64 - (b))))
#define SIP_ROUND()                                                  \
	do {                                                             \
		v0 += v1; v1 = SIP_ROTL(v1, 13); v1 ^= v0; v0 = SIP_ROTL(v0, 32); \
		v2 += v3; v3 = SIP_ROTL(v3, 16); v3 ^= v2;                   \
		v0 += v3; v3 = SIP_ROTL(v3, 21); v3 ^= v0;                   \
		v2 += v1; v1 = SIP_ROTL(v1, 17); v1 ^= v2; v2 = SIP_ROTL(v2, 32); \
	} while (0)

u64 rk_siphash(const void *data, size_t len, const u8 key[16])
{
	const u8 *p = data;
	u64 k0 = 0, k1 = 0;
	for (int i = 0; i < 8; i++) {
		k0 |= (u64)key[i] << (i * 8);
		k1 |= (u64)key[8 + i] << (i * 8);
	}

	u64 v0 = k0 ^ 0x736f6d6570736575ull;
	u64 v1 = k1 ^ 0x646f72616e646f6dull;
	u64 v2 = k0 ^ 0x6c7967656e657261ull;
	u64 v3 = k1 ^ 0x7465646279746573ull;

	size_t blocks = len / 8;
	for (size_t i = 0; i < blocks; i++) {
		u64 m = 0;
		for (int b = 0; b < 8; b++)
			m |= (u64)p[i * 8 + b] << (b * 8);
		v3 ^= m;
		SIP_ROUND();
		SIP_ROUND();
		v0 ^= m;
	}

	u64 tail = (u64)(len & 0xff) << 56;
	for (size_t i = blocks * 8, s = 0; i < len; i++, s += 8)
		tail |= (u64)p[i] << s;

	v3 ^= tail;
	SIP_ROUND();
	SIP_ROUND();
	v0 ^= tail;

	v2 ^= 0xff;
	SIP_ROUND();
	SIP_ROUND();
	SIP_ROUND();
	SIP_ROUND();
	return v0 ^ v1 ^ v2 ^ v3;
}

/* --------------------------------------------------------------- hex */

static const char hexchars[] = "0123456789abcdef";

size_t rk_hex_encode(char *dst, size_t dstlen, const void *src, size_t srclen)
{
	const u8 *p = src;
	size_t n = 0;
	for (size_t i = 0; i < srclen; i++) {
		if (n + 2 >= dstlen)
			break;
		dst[n++] = hexchars[p[i] >> 4];
		dst[n++] = hexchars[p[i] & 0xf];
	}
	if (dstlen)
		dst[n] = '\0';
	return n;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int rk_hex_decode(void *dst, size_t dstlen, const char *src)
{
	u8 *d = dst;
	size_t n = 0;
	while (src[0] && src[1]) {
		int hi = hexval(src[0]), lo = hexval(src[1]);
		if (hi < 0 || lo < 0)
			return RK_EINVAL;
		if (n >= dstlen)
			return RK_ENOSPC;
		d[n++] = (u8)((hi << 4) | lo);
		src += 2;
	}
	return src[0] ? RK_EINVAL : (int)n;
}

/* ------------------------------------------------------------ base64 */

static const char b64chars[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t rk_base64_encode(char *dst, size_t dstlen, const void *src, size_t srclen)
{
	const u8 *p = src;
	size_t n = 0;

	for (size_t i = 0; i < srclen; i += 3) {
		u32 v = (u32)p[i] << 16;
		if (i + 1 < srclen) v |= (u32)p[i + 1] << 8;
		if (i + 2 < srclen) v |= p[i + 2];

		if (n + 4 >= dstlen)
			break;
		dst[n++] = b64chars[(v >> 18) & 0x3f];
		dst[n++] = b64chars[(v >> 12) & 0x3f];
		dst[n++] = (i + 1 < srclen) ? b64chars[(v >> 6) & 0x3f] : '=';
		dst[n++] = (i + 2 < srclen) ? b64chars[v & 0x3f] : '=';
	}
	if (dstlen)
		dst[n] = '\0';
	return n;
}

int rk_base64_decode(void *dst, size_t dstlen, const char *src, size_t *outlen)
{
	u8 *d = dst;
	size_t n = 0;
	u32 acc = 0;
	int bits = 0;

	for (; *src; src++) {
		const char *q = strchr(b64chars, *src);
		if (*src == '=' )
			break;
		if (!q || !*src)
			return RK_EINVAL;
		acc = (acc << 6) | (u32)(q - b64chars);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n >= dstlen)
				return RK_ENOSPC;
			d[n++] = (u8)(acc >> bits);
		}
	}
	if (outlen)
		*outlen = n;
	return RK_OK;
}

/* ------------------------------------------------------------ selftest */

struct kat {
	const char *input;
	const char *expect_hex;
};

/* Known answer tests from FIPS 180-4 and RFC 4231. A crypto layer that has not
 * been checked against published vectors is decoration. */
static const struct kat sha_kats[] = {
	{ "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
	{ "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
	{ "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
	  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
};

int rk_crypto_selftest(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(sha_kats); i++) {
		u8 d[SHA256_DIGEST_SIZE];
		char hex[65];
		sha256(sha_kats[i].input, strlen(sha_kats[i].input), d);
		rk_hex_encode(hex, sizeof(hex), d, sizeof(d));
		if (strcmp(hex, sha_kats[i].expect_hex) != 0) {
			pr_err("SHA-256 known-answer test %u failed: got %s", (unsigned)i, hex);
			return RK_EIO;
		}
	}

	/* RFC 4231 test case 1. */
	u8 key[20], mac[SHA256_DIGEST_SIZE];
	char hex[65];
	memset(key, 0x0b, sizeof(key));
	hmac_sha256(key, sizeof(key), "Hi There", 8, mac);
	rk_hex_encode(hex, sizeof(hex), mac, sizeof(mac));
	if (strcmp(hex, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7") != 0) {
		pr_err("HMAC-SHA256 known-answer test failed: got %s", hex);
		return RK_EIO;
	}

	/* RFC 8439 section 2.3.2: ChaCha20 block with an all-zero key and nonce
	 * and counter 0 produces this first keystream word. */
	u8 zkey[32] = { 0 }, znonce[12] = { 0 }, ks[64];
	struct chacha20_ctx cc;
	chacha20_init(&cc, zkey, znonce, 0);
	chacha20_keystream(&cc, ks, sizeof(ks));
	rk_hex_encode(hex, sizeof(hex), ks, 16);
	if (strcmp(hex, "76b8e0ada0f13d90405d6ae55386bd28") != 0) {
		pr_err("ChaCha20 known-answer test failed: got %s", hex);
		return RK_EIO;
	}

	/* Base64 round trip, since Kaalka interoperability depends on it. */
	char b64[64];
	u8 back[32];
	size_t backlen = 0;
	rk_base64_encode(b64, sizeof(b64), "RESENTMENT", 10);
	if (strcmp(b64, "UkVTRU5UTUVOVA==") != 0) {
		pr_err("base64 encode failed: got %s", b64);
		return RK_EIO;
	}
	if (rk_base64_decode(back, sizeof(back), b64, &backlen) != RK_OK ||
	    backlen != 10 || memcmp(back, "RESENTMENT", 10) != 0) {
		pr_err("base64 decode round trip failed");
		return RK_EIO;
	}

	pr_info("crypto self-test passed: SHA-256, HMAC, ChaCha20, base64");
	return RK_OK;
}

void rk_crypto_init(void)
{
	rk_random_init();
	if (rk_crypto_selftest() != RK_OK)
		pr_err("crypto self-test FAILED; sealed capabilities are not trustworthy");
}
