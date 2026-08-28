/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - ChaCha20 and the kernel CSPRNG.
 *
 * ChaCha20 rather than AES because it needs no tables, no hardware support and
 * no data-dependent memory access, which makes it constant time everywhere and
 * portable to the boards this kernel targets without an AES unit.
 *
 * The CSPRNG mixes four sources: hardware entropy where it exists, the timing
 * jitter of real interrupts, the runtime graph digest (which nobody outside
 * the machine can predict), and its own previous state. A machine with no
 * RDRAND still ends up with an unpredictable pool, which matters because most
 * ARM and RISC-V boards do not have one.
 */
#include <rk/crypto.h>
#include <rk/string.h>
#include <rk/arch.h>
#include <rk/spinlock.h>
#include <rk/log.h>
#include <rk/time.h>

#undef RK_SUBSYS
#define RK_SUBSYS "random"

static inline u32 rol32(u32 x, unsigned n) { return (x << n) | (x >> (32 - n)); }

#define QR(a, b, c, d)                       \
	a += b; d ^= a; d = rol32(d, 16);        \
	c += d; b ^= c; b = rol32(b, 12);        \
	a += b; d ^= a; d = rol32(d, 8);         \
	c += d; b ^= c; b = rol32(b, 7)

static void chacha20_core(const u32 in[16], u8 out[64])
{
	u32 x[16];
	memcpy(x, in, sizeof(x));

	for (int i = 0; i < 10; i++) {
		QR(x[0], x[4], x[8],  x[12]);
		QR(x[1], x[5], x[9],  x[13]);
		QR(x[2], x[6], x[10], x[14]);
		QR(x[3], x[7], x[11], x[15]);
		QR(x[0], x[5], x[10], x[15]);
		QR(x[1], x[6], x[11], x[12]);
		QR(x[2], x[7], x[8],  x[13]);
		QR(x[3], x[4], x[9],  x[14]);
	}
	for (int i = 0; i < 16; i++) {
		u32 v = x[i] + in[i];
		out[i * 4]     = (u8)v;
		out[i * 4 + 1] = (u8)(v >> 8);
		out[i * 4 + 2] = (u8)(v >> 16);
		out[i * 4 + 3] = (u8)(v >> 24);
	}
}

static inline u32 le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

void chacha20_init(struct chacha20_ctx *c, const u8 key[CHACHA20_KEY_SIZE],
                   const u8 nonce[CHACHA20_NONCE_SIZE], u32 counter)
{
	c->state[0] = 0x61707865;   /* "expa" */
	c->state[1] = 0x3320646e;   /* "nd 3" */
	c->state[2] = 0x79622d32;   /* "2-by" */
	c->state[3] = 0x6b206574;   /* "te k" */
	for (int i = 0; i < 8; i++)
		c->state[4 + i] = le32(key + i * 4);
	c->state[12] = counter;
	for (int i = 0; i < 3; i++)
		c->state[13 + i] = le32(nonce + i * 4);
}

void chacha20_xor(struct chacha20_ctx *c, const u8 *in, u8 *out, size_t len)
{
	u8 block[64];
	while (len) {
		chacha20_core(c->state, block);
		c->state[12]++;
		size_t n = len < 64 ? len : 64;
		for (size_t i = 0; i < n; i++)
			out[i] = (in ? in[i] : 0) ^ block[i];
		out += n;
		if (in)
			in += n;
		len -= n;
	}
	rk_secure_zero(block, sizeof(block));
}

void chacha20_keystream(struct chacha20_ctx *c, u8 *out, size_t len)
{
	chacha20_xor(c, NULL, out, len);
}

/* --------------------------------------------------------------- CSPRNG */

static struct {
	u8         key[CHACHA20_KEY_SIZE];
	u8         nonce[CHACHA20_NONCE_SIZE];
	u64        counter;
	u32        entropy_bits;
	bool       seeded;
	spinlock_t lock;
	u8         pool[64];
	u32        pool_pos;
} rng;

/* Fold new entropy into the pool by hashing it together with what is already
 * there. Hashing rather than XOR means a caller cannot cancel out entropy by
 * feeding in a chosen value. */
void rk_random_add_entropy(const void *data, size_t len, u32 estimated_bits)
{
	unsigned long f = spin_lock_irqsave(&rng.lock);

	struct sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, rng.pool, sizeof(rng.pool));
	sha256_update(&c, data, len);
	u64 tsc = arch_cycles();
	sha256_update(&c, &tsc, sizeof(tsc));

	u8 d[SHA256_DIGEST_SIZE];
	sha256_final(&c, d);
	memcpy(rng.pool, d, sizeof(d));

	rng.entropy_bits += estimated_bits;
	if (rng.entropy_bits > 4096)
		rng.entropy_bits = 4096;

	/* Rekey on every contribution, not only once the estimate crosses the
	 * threshold.
	 *
	 * The threshold governs whether the kernel is willing to *claim* the
	 * generator is seeded. It must not govern whether the key depends on the
	 * collected entropy at all - gating the rekey on it left the generator
	 * running on its initial all-zero key on any machine whose estimate never
	 * got there, which is every machine without a hardware RNG. The warning
	 * was accurate and the situation was far worse than the warning implied.
	 *
	 * Folding the old key in means a contribution can only ever add
	 * uncertainty; it can never reduce what the key already depended on. */
	struct sha256_ctx k;
	sha256_init(&k);
	sha256_update(&k, rng.key, sizeof(rng.key));
	sha256_update(&k, rng.pool, sizeof(rng.pool));
	sha256_final(&k, rng.key);

	if (rng.entropy_bits >= 128)
		rng.seeded = true;
	rk_secure_zero(d, sizeof(d));
	spin_unlock_irqrestore(&rng.lock, f);
}

void rk_random_init(void)
{
	spin_lock_init(&rng.lock, "random");
	memset(&rng.pool, 0, sizeof(rng.pool));

	u8 hw[32];
	size_t n = arch_hw_random(hw, sizeof(hw));
	if (n) {
		rk_random_add_entropy(hw, n, (u32)(n * 8));
		rk_secure_zero(hw, sizeof(hw));
	}

	/* Timing jitter: sample the cycle counter across operations whose latency
	 * the hardware itself does not fully determine. On a machine with no
	 * hardware RNG this is the only real entropy available at boot, so it is
	 * collected deliberately rather than hoped for. */
	u64 jitter[32];
	for (int i = 0; i < 32; i++) {
		u64 a = arch_cycles();
		for (volatile int k = 0; k < 97; k++)
			arch_cpu_relax();
		jitter[i] = arch_cycles() - a;
	}
	rk_random_add_entropy(jitter, sizeof(jitter), 64);

	/* The wall clock and the cycle counter are mixed in, and credited with
	 * exactly zero bits. Claiming entropy for a value an attacker can simply
	 * read would be a lie in the one place a kernel must not tell one.
	 *
	 * They are mixed anyway because they differ between boots, and without
	 * them a machine with no hardware RNG - which under an emulator also means
	 * no meaningful timing jitter - produces a byte-for-byte identical stream
	 * on every single boot. That is worth fixing even though it improves no
	 * entropy estimate. */
	u64 boot[2] = { (u64)rk_unix_time(), arch_cycles() };
	rk_random_add_entropy(boot, sizeof(boot), 0);

	if (!rng.seeded)
		pr_warn("entropy pool is weak: %u bits estimated", rng.entropy_bits);
	else
		pr_info("CSPRNG seeded, %u bits of estimated entropy", rng.entropy_bits);
}

void rk_random_bytes(void *buf, size_t len)
{
	unsigned long f = spin_lock_irqsave(&rng.lock);

	for (int i = 0; i < 3; i++)
		((u32 *)rng.nonce)[i] = (u32)(rng.counter >> (i * 8));
	rng.counter++;

	struct chacha20_ctx c;
	chacha20_init(&c, rng.key, rng.nonce, (u32)rng.counter);
	chacha20_keystream(&c, buf, len);

	/* Ratchet: derive the next key from this one so that a state compromise
	 * does not expose previously generated values. */
	u8 next[CHACHA20_KEY_SIZE];
	chacha20_keystream(&c, next, sizeof(next));
	memcpy(rng.key, next, sizeof(next));
	rk_secure_zero(next, sizeof(next));
	rk_secure_zero(&c, sizeof(c));

	spin_unlock_irqrestore(&rng.lock, f);
}

u64 rk_random_u64(void)
{
	u64 v;
	rk_random_bytes(&v, sizeof(v));
	return v;
}

u32 rk_random_u32(void)
{
	u32 v;
	rk_random_bytes(&v, sizeof(v));
	return v;
}

u32 rk_random_entropy_bits(void) { return rng.entropy_bits; }
