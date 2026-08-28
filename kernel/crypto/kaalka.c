/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - Kaalka temporal cryptography.
 *
 * See rk/kaalka.h for why this exists and what it is used for. This file has
 * three layers:
 *
 *   1. A deterministic fixed-point trig core. Q32.32 throughout, one rounding
 *      rule, identical results on x86_64, aarch64 and riscv64. The reference
 *      implementation uses IEEE doubles and libm, neither of which is
 *      available or bit-reproducible in a kernel.
 *   2. The reference byte transforms, kept for interoperability with Kaalka
 *      payloads produced by the Python, JS, Java, Kotlin and Dart libraries.
 *   3. The kernel protocol: epoch keys, seals, a replay ledger and an
 *      authenticated envelope. This is what capabilities and IPC actually use,
 *      and it takes its confidentiality from ChaCha20 and its integrity from
 *      HMAC-SHA256 rather than from the clock-angle stream.
 */
#include <rk/kaalka.h>
#include <rk/crypto.h>
#include <rk/time.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/printf.h>

#undef RK_SUBSYS
#define RK_SUBSYS "kaalka"

/* ------------------------------------------------------- fixed point trig */

#define KQ_DEG2RAD 74961321LL          /* pi/180 in Q32.32 */
#define KQ_360     (360LL << KQ_SHIFT)
#define KQ_180     (180LL << KQ_SHIFT)
#define KQ_90      (90LL  << KQ_SHIFT)

/* The reference lets tan run to infinity at 90 and 270 degrees. A kernel
 * cannot carry an infinity through an integer pipeline, so trig output is
 * clamped. The clamp only changes a result for angles within about 1e-6
 * degrees of an asymptote, and because encrypt and decrypt clamp identically,
 * a round trip is still exact. */
#define KQ_CLAMP ((kq_t)1 << (KQ_SHIFT + 20))

s64 kq_round(kq_t v)
{
	/* Half away from zero, matching how the reference rounds its offsets. */
	if (v >= 0)
		return (s64)((v + (KQ_ONE / 2)) >> KQ_SHIFT);
	return -(s64)(((-v) + (KQ_ONE / 2)) >> KQ_SHIFT);
}

static kq_t kq_normalize_deg(kq_t d)
{
	d %= KQ_360;
	if (d < 0)
		d += KQ_360;
	return d;
}

/* Taylor series with each term derived from the previous one, so precision
 * does not collapse into a tiny constant. Argument is reduced to [0, 90) with
 * the quadrant handled by the caller, where the series converges quickly. */
static kq_t sin_series(kq_t rad)
{
	kq_t term = rad;
	kq_t sum  = rad;
	kq_t rad2 = kq_mul(rad, rad);

	for (int n = 1; n <= 8; n++) {
		term = kq_mul(term, rad2);
		term = term / (kq_t)((2 * n) * (2 * n + 1));
		sum = (n & 1) ? sum - term : sum + term;
		if (term == 0)
			break;
	}
	return sum;
}

kq_t kq_sin(kq_t degrees)
{
	kq_t d = kq_normalize_deg(degrees);
	int  sign = 1;

	if (d >= KQ_180) {
		d -= KQ_180;
		sign = -1;
	}
	if (d > KQ_90)
		d = KQ_180 - d;

	kq_t rad = kq_mul(d, KQ_DEG2RAD);
	kq_t v = sin_series(rad);
	return sign > 0 ? v : -v;
}

kq_t kq_cos(kq_t degrees) { return kq_sin(degrees + KQ_90); }

kq_t kq_tan(kq_t degrees)
{
	kq_t s = kq_sin(degrees);
	kq_t c = kq_cos(degrees);
	if (c == 0)
		return s >= 0 ? KQ_CLAMP : -KQ_CLAMP;
	kq_t t = kq_div(s, c);
	if (t > KQ_CLAMP)  return KQ_CLAMP;
	if (t < -KQ_CLAMP) return -KQ_CLAMP;
	return t;
}

kq_t kq_cot(kq_t degrees)
{
	kq_t s = kq_sin(degrees);
	kq_t c = kq_cos(degrees);
	if (s == 0)
		return c >= 0 ? KQ_CLAMP : -KQ_CLAMP;
	kq_t t = kq_div(c, s);
	if (t > KQ_CLAMP)  return KQ_CLAMP;
	if (t < -KQ_CLAMP) return -KQ_CLAMP;
	return t;
}

/* ---------------------------------------------------------- clock angles */

void kaalka_set_time(struct kaalka_time *k, int h, int m, int s)
{
	k->hour = (u8)(((h % 12) + 12) % 12);
	k->min  = (u8)(((m % 60) + 60) % 60);
	k->sec  = (u8)(((s % 60) + 60) % 60);
}

void kaalka_time_now(struct kaalka_time *k)
{
	struct rk_tm tm;
	rk_gmtime(rk_unix_time(), &tm);
	kaalka_set_time(k, tm.hour, tm.min, tm.sec);
}

/* The three angles between the hands of the clock, exactly as the reference
 * defines them:
 *   hour hand   30h + 0.5m + (0.5/60)s
 *   minute hand 6m + 0.1s
 *   second hand 6s
 * and each pairwise separation taken the short way round. */
void kaalka_angles(const struct kaalka_time *k, struct kaalka_angles *out)
{
	kq_t h = kq_from_int(30 * k->hour)
	       + kq_div(kq_from_int(k->min), kq_from_int(2))
	       + kq_div(kq_from_int(k->sec), kq_from_int(120));
	kq_t m = kq_from_int(6 * k->min)
	       + kq_div(kq_from_int(k->sec), kq_from_int(10));
	kq_t s = kq_from_int(6 * k->sec);

	kq_t dhm = h > m ? h - m : m - h;
	kq_t dms = m > s ? m - s : s - m;
	kq_t dhs = h > s ? h - s : s - h;

	out->hour_min = MIN(dhm, KQ_360 - dhm);
	out->min_sec  = MIN(dms, KQ_360 - dms);
	out->hour_sec = MIN(dhs, KQ_360 - dhs);
}

/* Quadrant selects the function: sin, cos, tan, cot for quadrants 1 to 4. */
kq_t kaalka_select_trig(kq_t angle_deg)
{
	kq_t a = kq_normalize_deg(angle_deg);
	int  quadrant = (int)(a / KQ_90);

	switch (quadrant) {
	case 0:  return kq_sin(a);
	case 1:  return kq_cos(a);
	case 2:  return kq_tan(a);
	default: return kq_cot(a);
	}
}

/* ------------------------------------------------- reference transforms */

static void classic_apply(const struct kaalka_time *k, const u8 *in, u8 *out,
                          size_t len, bool encrypt)
{
	struct kaalka_angles ang;
	kaalka_angles(k, &ang);

	kq_t t = kaalka_select_trig(ang.hour_min)
	       + kaalka_select_trig(ang.min_sec)
	       + kaalka_select_trig(ang.hour_sec);

	for (size_t i = 0; i < len; i++) {
		s64 factor = (s64)k->hour + k->min + k->sec + (s64)i + 1;

		/* 128-bit product: the trig sum is clamped to 2^20 and the factor
		 * grows with the message, so a 64-bit Q32.32 multiply would overflow
		 * on a long payload. */
		__int128 scaled = (__int128)t * factor;
		s64 whole = (s64)(scaled >> KQ_SHIFT);
		s64 frac  = (s64)(scaled & (KQ_ONE - 1));
		if (frac >= KQ_ONE / 2)
			whole += 1;
		s64 offset = whole + (s64)i + 1;

		/* Floor modulo, so a negative offset wraps the way the algorithm
		 * intends rather than the way C truncation would. */
		s64 v = encrypt ? (s64)in[i] + offset : (s64)in[i] - offset;
		v %= 256;
		if (v < 0)
			v += 256;
		out[i] = (u8)v;
	}
}

void kaalka_classic_encrypt(const struct kaalka_time *k, const u8 *in, u8 *out, size_t len)
{
	classic_apply(k, in, out, len, true);
}

void kaalka_classic_decrypt(const struct kaalka_time *k, const u8 *in, u8 *out, size_t len)
{
	classic_apply(k, in, out, len, false);
}

void kaalka_proc(const struct kaalka_time *k, const u8 *in, u8 *out, size_t len, bool encrypt)
{
	u32 key = (u32)k->hour * 3600u + (u32)k->min * 60u + k->sec;
	if (!key)
		key = 1;

	for (size_t i = 0; i < len; i++) {
		u32 offset = (u32)((key + i) % 256u);
		out[i] = encrypt ? (u8)((in[i] + offset) & 0xff)
		                 : (u8)((in[i] - offset) & 0xff);
	}
}

/* ------------------------------------------------------------- epochs */

static u8   master_key[32];
static u8   epoch_key[32];
static kaalka_epoch_t current_epoch;
static bool kaalka_ready;
static struct kaalka_stats kstats;
static DEFINE_SPINLOCK(kaalka_lock);

kaalka_epoch_t kaalka_epoch_of(s64 unix_sec)
{
	if (unix_sec < 0)
		unix_sec = 0;
	return (kaalka_epoch_t)unix_sec / KAALKA_EPOCH_SECONDS;
}

kaalka_epoch_t kaalka_epoch_now(void) { return kaalka_epoch_of(rk_unix_time()); }

s64 kaalka_epoch_start(kaalka_epoch_t e)
{
	return (s64)(e * KAALKA_EPOCH_SECONDS);
}

/* Derivation is a pure function of (master, epoch, purpose). The clock angles
 * at the epoch boundary go into the HKDF info, which is what makes the key
 * genuinely Kaalka-derived rather than merely time-stamped: the same second on
 * a different day gives a different angle triple and so a different key. */
void kaalka_derive_key(kaalka_epoch_t epoch, const char *purpose,
                       u8 *out, size_t outlen)
{
	struct kaalka_time kt;
	struct kaalka_angles ang;
	struct rk_tm tm;

	rk_gmtime(kaalka_epoch_start(epoch), &tm);
	kaalka_set_time(&kt, tm.hour, tm.min, tm.sec);
	kaalka_angles(&kt, &ang);

	struct {
		u64  epoch;
		kq_t hm, ms, hs;
		char purpose[32];
	} info;
	memset(&info, 0, sizeof(info));
	info.epoch = epoch;
	info.hm = ang.hour_min;
	info.ms = ang.min_sec;
	info.hs = ang.hour_sec;
	strlcpy(info.purpose, purpose ? purpose : "generic", sizeof(info.purpose));

	static const char salt[] = "RESENTMENT/kaalka/v1";
	hkdf_sha256(salt, sizeof(salt) - 1, master_key, sizeof(master_key),
	            &info, sizeof(info), out, outlen);
}

void kaalka_rekey(void)
{
	unsigned long f = spin_lock_irqsave(&kaalka_lock);
	current_epoch = kaalka_epoch_now();
	kaalka_derive_key(current_epoch, "epoch", epoch_key, sizeof(epoch_key));
	kstats.rekeys++;
	kstats.current_epoch = current_epoch;
	spin_unlock_irqrestore(&kaalka_lock, f);
}

static void refresh_epoch(void)
{
	if (kaalka_epoch_now() != current_epoch)
		kaalka_rekey();
}

void kaalka_init(void)
{
	spin_lock_init(&kaalka_lock, "kaalka");
	rk_random_bytes(master_key, sizeof(master_key));
	kaalka_rekey();
	kaalka_ready = true;
	pr_info("temporal keying active, epoch %llu (%u second slices)",
	        (unsigned long long)current_epoch, KAALKA_EPOCH_SECONDS);
}

/* --------------------------------------------------------------- seals */

/* The MAC covers everything that identifies the seal, so no field can be
 * edited without detection - including the validity window, which is the
 * entire point. */
static void seal_mac(const struct kaalka_seal *s, u64 subject,
                     const void *payload, size_t len, u8 out[KAALKA_SEAL_SIZE])
{
	u8 key[32];
	kaalka_derive_key(s->epoch, "seal", key, sizeof(key));

	struct {
		u64 subject, seq;
		u64 epoch;
		s64 not_before, not_after;
		u64 paylen;
	} hdr = { subject, s->seq, s->epoch, s->not_before, s->not_after, len };

	struct hmac_sha256_ctx c;
	hmac_sha256_init(&c, key, sizeof(key));
	hmac_sha256_update(&c, &hdr, sizeof(hdr));
	if (payload && len)
		hmac_sha256_update(&c, payload, len);
	hmac_sha256_final(&c, out);
	rk_secure_zero(key, sizeof(key));
}

void kaalka_seal_make(struct kaalka_seal *s, u64 subject, u64 seq,
                      const void *payload, size_t len, u64 lifetime_sec)
{
	refresh_epoch();

	s64 now = rk_unix_time();
	s->epoch      = kaalka_epoch_now();
	s->subject    = subject;
	s->seq        = seq;
	s->not_before = now;
	s->not_after  = now + (s64)(lifetime_sec ? lifetime_sec : KAALKA_EPOCH_SECONDS);
	seal_mac(s, subject, payload, len, s->mac);

	unsigned long f = spin_lock_irqsave(&kaalka_lock);
	kstats.seals_made++;
	spin_unlock_irqrestore(&kaalka_lock, f);
}

int kaalka_seal_verify(const struct kaalka_seal *s, u64 subject,
                       const void *payload, size_t len)
{
	s64 now = rk_unix_time();

	/* Time first: an expired seal is refused without touching the MAC, so a
	 * caller cannot learn anything by timing the comparison of a stale one. */
	if (now < s->not_before || (s->not_after && now >= s->not_after)) {
		unsigned long f = spin_lock_irqsave(&kaalka_lock);
		kstats.seals_expired++;
		spin_unlock_irqrestore(&kaalka_lock, f);
		return RK_EEXPIRED;
	}
	if (s->subject != subject)
		return RK_ESEAL;

	u8 expect[KAALKA_SEAL_SIZE];
	seal_mac(s, subject, payload, len, expect);

	unsigned long f = spin_lock_irqsave(&kaalka_lock);
	bool ok = rk_ct_eq(expect, s->mac, sizeof(expect));
	if (ok)
		kstats.seals_verified++;
	else
		kstats.seals_rejected++;
	spin_unlock_irqrestore(&kaalka_lock, f);

	rk_secure_zero(expect, sizeof(expect));
	return ok ? RK_OK : RK_ESEAL;
}

/* -------------------------------------------------------- replay ledger */

void kaalka_ledger_init(struct kaalka_ledger *l)
{
	memset(l->slot, 0, sizeof(l->slot));
	spin_lock_init(&l->lock, "kaalka-ledger");
	l->accepted = l->rejected = l->evicted = 0;
}

int kaalka_ledger_check(struct kaalka_ledger *l, u64 subject, u64 seq)
{
	kaalka_epoch_t now = kaalka_epoch_now();
	u64 h = subject * 0x9E3779B97F4A7C15ull + seq;
	size_t idx = (size_t)(h % KAALKA_LEDGER_SLOTS);

	unsigned long f = spin_lock_irqsave(&l->lock);

	/* Linear probe over a short window. Bounded on purpose: an unbounded
	 * ledger is a memory exhaustion attack, and an entry that has aged past
	 * its epoch cannot be replayed anyway because the seal has expired. */
	for (size_t i = 0; i < 8; i++) {
		size_t k = (idx + i) % KAALKA_LEDGER_SLOTS;
		if (l->slot[k].subject == subject && l->slot[k].seq == seq &&
		    l->slot[k].epoch + 2 >= now) {
			l->rejected++;
			spin_unlock_irqrestore(&l->lock, f);
			return RK_EREPLAY;
		}
	}
	for (size_t i = 0; i < 8; i++) {
		size_t k = (idx + i) % KAALKA_LEDGER_SLOTS;
		if (l->slot[k].epoch == 0 || l->slot[k].epoch + 2 < now) {
			l->slot[k].subject = subject;
			l->slot[k].seq = seq;
			l->slot[k].epoch = now;
			l->accepted++;
			spin_unlock_irqrestore(&l->lock, f);
			return RK_OK;
		}
	}

	/* All eight probes are live and current; evict the first. */
	l->slot[idx].subject = subject;
	l->slot[idx].seq = seq;
	l->slot[idx].epoch = now;
	l->evicted++;
	l->accepted++;
	spin_unlock_irqrestore(&l->lock, f);
	return RK_OK;
}

/* ------------------------------------------------------------ envelopes */

/* Digest of everything the seal must bind: the header up to but not including
 * the seal field, then the ciphertext that follows the header. */
static void envelope_digest(const struct kaalka_envelope *e, size_t ctlen,
                            u8 out[SHA256_DIGEST_SIZE])
{
	struct sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, e, offsetof(struct kaalka_envelope, seal));
	sha256_update(&c, (const u8 *)(e + 1), ctlen);
	sha256_final(&c, out);
}

int kaalka_envelope_seal(void *out, size_t outcap, size_t *outlen,
                         u64 sender, u64 receiver, u64 seq,
                         const void *pt, size_t ptlen, u64 lifetime_sec)
{
	size_t need = sizeof(struct kaalka_envelope) + ptlen;
	if (outcap < need)
		return RK_ENOSPC;

	refresh_epoch();

	struct kaalka_envelope *e = out;
	memset(e, 0, sizeof(*e));
	e->ver       = 1;
	e->sender    = sender;
	e->receiver  = receiver;
	e->seq       = seq;
	e->epoch     = kaalka_epoch_now();
	e->timestamp = rk_unix_time();
	e->ct_len    = (u32)ptlen;
	rk_random_bytes(e->nonce, sizeof(e->nonce));

	u8 key[CHACHA20_KEY_SIZE];
	kaalka_derive_key(e->epoch, "envelope", key, sizeof(key));

	struct chacha20_ctx c;
	chacha20_init(&c, key, e->nonce, 1);
	chacha20_xor(&c, pt, (u8 *)(e + 1), ptlen);
	rk_secure_zero(key, sizeof(key));
	rk_secure_zero(&c, sizeof(c));

	/* The seal covers the header and the ciphertext together, so neither the
	 * routing fields nor the payload can be swapped independently. The two
	 * regions are digested separately because the seal itself sits between
	 * them and obviously cannot cover its own bytes. */
	u8 bound[SHA256_DIGEST_SIZE];
	envelope_digest(e, ptlen, bound);
	kaalka_seal_make(&e->seal, receiver, seq, bound, sizeof(bound), lifetime_sec);

	if (outlen)
		*outlen = need;

	unsigned long f = spin_lock_irqsave(&kaalka_lock);
	kstats.envelopes_sealed++;
	spin_unlock_irqrestore(&kaalka_lock, f);
	return RK_OK;
}

int kaalka_envelope_open(const void *in, size_t inlen,
                         u64 receiver, void *pt, size_t ptcap, size_t *ptlen,
                         struct kaalka_ledger *ledger)
{
	if (inlen < sizeof(struct kaalka_envelope))
		return RK_EINVAL;

	const struct kaalka_envelope *e = in;
	if (e->ver != 1)
		return RK_ENOTSUP;
	if (e->receiver != receiver)
		return RK_EACCES;
	if (inlen < sizeof(*e) + e->ct_len)
		return RK_EINVAL;
	if (e->ct_len > ptcap)
		return RK_ENOSPC;

	u8 bound[SHA256_DIGEST_SIZE];
	envelope_digest(e, e->ct_len, bound);
	int r = kaalka_seal_verify(&e->seal, receiver, bound, sizeof(bound));
	if (r != RK_OK)
		return r;

	if (ledger) {
		r = kaalka_ledger_check(ledger, e->sender, e->seq);
		if (r != RK_OK) {
			unsigned long f = spin_lock_irqsave(&kaalka_lock);
			kstats.replays_blocked++;
			spin_unlock_irqrestore(&kaalka_lock, f);
			return r;
		}
	}

	u8 key[CHACHA20_KEY_SIZE];
	kaalka_derive_key(e->epoch, "envelope", key, sizeof(key));

	struct chacha20_ctx c;
	chacha20_init(&c, key, e->nonce, 1);
	chacha20_xor(&c, (const u8 *)(e + 1), pt, e->ct_len);
	rk_secure_zero(key, sizeof(key));
	rk_secure_zero(&c, sizeof(c));

	if (ptlen)
		*ptlen = e->ct_len;

	unsigned long f = spin_lock_irqsave(&kaalka_lock);
	kstats.envelopes_opened++;
	spin_unlock_irqrestore(&kaalka_lock, f);
	return RK_OK;
}

void kaalka_stats(struct kaalka_stats *out)
{
	unsigned long f = spin_lock_irqsave(&kaalka_lock);
	*out = kstats;
	out->current_epoch = current_epoch;
	spin_unlock_irqrestore(&kaalka_lock, f);
}

/* ------------------------------------------------------------ selftest */

int kaalka_selftest(void)
{
	struct kaalka_time kt;
	const char *msg = "Time is the Key";
	u8 ct[32], back[32];
	size_t n = strlen(msg);

	/* Angles at 12:34:56, the example from the Kaalka documentation. Hour is
	 * reduced modulo 12, so this is hour 0. */
	kaalka_set_time(&kt, 12, 34, 56);
	if (kt.hour != 0 || kt.min != 34 || kt.sec != 56) {
		pr_err("clock reduction wrong: %u:%u:%u", kt.hour, kt.min, kt.sec);
		return RK_EIO;
	}

	kaalka_classic_encrypt(&kt, (const u8 *)msg, ct, n);
	kaalka_classic_decrypt(&kt, ct, back, n);
	if (memcmp(back, msg, n) != 0) {
		pr_err("classic transform does not round trip");
		return RK_EIO;
	}
	if (memcmp(ct, msg, n) == 0) {
		pr_err("classic transform produced plaintext");
		return RK_EIO;
	}

	/* A different second must produce a different ciphertext, or time is not
	 * actually part of the key. */
	u8 ct2[32];
	struct kaalka_time kt2;
	kaalka_set_time(&kt2, 12, 34, 57);
	kaalka_classic_encrypt(&kt2, (const u8 *)msg, ct2, n);
	if (memcmp(ct, ct2, n) == 0) {
		pr_err("ciphertext did not change with the clock");
		return RK_EIO;
	}

	kaalka_proc(&kt, (const u8 *)msg, ct, n, true);
	kaalka_proc(&kt, ct, back, n, false);
	if (memcmp(back, msg, n) != 0) {
		pr_err("stream transform does not round trip");
		return RK_EIO;
	}

	/* Trig sanity, in the fixed-point domain. sin 30 is exactly one half. */
	kq_t s30 = kq_sin(kq_from_int(30));
	if (s30 < (KQ_ONE / 2) - 4096 || s30 > (KQ_ONE / 2) + 4096) {
		pr_err("kq_sin(30) = %lld, expected %lld",
		       (long long)s30, (long long)(KQ_ONE / 2));
		return RK_EIO;
	}
	kq_t c0 = kq_cos(0);
	if (c0 < KQ_ONE - 4096 || c0 > KQ_ONE + 4096) {
		pr_err("kq_cos(0) = %lld, expected %lld",
		       (long long)c0, (long long)KQ_ONE);
		return RK_EIO;
	}

	/* Seal: valid now, and refused once its window closes. */
	struct kaalka_seal seal;
	const char *payload = "capability";
	kaalka_seal_make(&seal, 0x1234, 1, payload, strlen(payload), 60);
	if (kaalka_seal_verify(&seal, 0x1234, payload, strlen(payload)) != RK_OK) {
		pr_err("fresh seal failed to verify");
		return RK_EIO;
	}
	if (kaalka_seal_verify(&seal, 0x9999, payload, strlen(payload)) == RK_OK) {
		pr_err("seal verified for the wrong subject");
		return RK_EIO;
	}
	struct kaalka_seal expired = seal;
	expired.not_after = rk_unix_time() - 1;
	if (kaalka_seal_verify(&expired, 0x1234, payload, strlen(payload)) != RK_EEXPIRED) {
		pr_err("expired seal was not refused");
		return RK_EIO;
	}

	/* Envelope round trip plus replay detection. */
	static u8 env[256];
	size_t envlen = 0, outlen = 0;
	u8 plain[64];
	if (kaalka_envelope_seal(env, sizeof(env), &envlen, 1, 2, 7,
	                         msg, n, 60) != RK_OK) {
		pr_err("envelope seal failed");
		return RK_EIO;
	}
	static struct kaalka_ledger ledger;
	kaalka_ledger_init(&ledger);
	if (kaalka_envelope_open(env, envlen, 2, plain, sizeof(plain), &outlen,
	                         &ledger) != RK_OK || outlen != n ||
	    memcmp(plain, msg, n) != 0) {
		pr_err("envelope open failed");
		return RK_EIO;
	}
	if (kaalka_envelope_open(env, envlen, 2, plain, sizeof(plain), &outlen,
	                         &ledger) != RK_EREPLAY) {
		pr_err("replayed envelope was accepted");
		return RK_EIO;
	}

	pr_info("kaalka self-test passed: trig, transforms, seals, envelopes, replay");
	return RK_OK;
}
