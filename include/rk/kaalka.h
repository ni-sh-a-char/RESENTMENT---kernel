/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - Kaalka temporal cryptography.
 *
 * Kaalka (https://github.com/PIYUSH-MISHRA-00/Kaalka-Encryption-Algorithm)
 * derives keying material from the angles between the hands of a clock. In
 * userspace that is a cipher. In a kernel it is something more useful: a way
 * to make *time* a dimension of authority.
 *
 * RESENTMENT uses it for three things:
 *
 *   1. Temporal capability seals. Every capability carries a Kaalka seal that
 *      is only valid inside a time window. Authority expires by construction
 *      instead of by a revocation sweep that may never run.
 *   2. Epoch keying. The kernel master key is re-derived every Kaalka epoch,
 *      so a key recovered from a memory dump is stale within seconds.
 *   3. Replay defence for IPC envelopes, via a bounded ledger of seen seals.
 *
 * Two deliberate departures from the reference implementation, both required
 * by the kernel environment and both checked against it by tools/kaalka_ref.py:
 *
 *   a) Fixed point, not floating point. The reference uses doubles. A kernel
 *      cannot use the FPU in interrupt context, and IEEE754 libm results are
 *      not bit-identical across x86_64, aarch64 and riscv64. Determinism is
 *      non-negotiable when the seal is the authorisation check and when the
 *      runtime graph must replay identically, so the trig core here is Q32.32
 *      integer maths with a fixed reduction order.
 *
 *   b) Kaalka supplies the epoch and the schedule; ChaCha20 and HMAC-SHA256
 *      supply the confidentiality and integrity. The clock-angle stream alone
 *      is an additive cipher over a small key space and must not be trusted to
 *      protect kernel objects. Saying so plainly is part of the design.
 */
#pragma once

#include <rk/types.h>
#include <rk/compiler.h>
#include <rk/crypto.h>
#include <rk/spinlock.h>

/* ------------------------------------------------------- fixed point core */

/* Q32.32 signed fixed point. One representation, one rounding rule, on every
 * architecture, forever. */
typedef s64 kq_t;

#define KQ_SHIFT 32
#define KQ_ONE   ((kq_t)1 << KQ_SHIFT)

static inline kq_t kq_from_int(s64 v)     { return (kq_t)v << KQ_SHIFT; }
static inline s64  kq_to_int(kq_t v)      { return v >> KQ_SHIFT; }
static inline kq_t kq_mul(kq_t a, kq_t b) { return (kq_t)(((__int128)a * b) >> KQ_SHIFT); }
static inline kq_t kq_div(kq_t a, kq_t b) { return b ? (kq_t)(((__int128)a << KQ_SHIFT) / b) : 0; }

/* Round half away from zero, matching the reference rounding of offsets. */
s64  kq_round(kq_t v);
kq_t kq_sin(kq_t degrees);
kq_t kq_cos(kq_t degrees);
kq_t kq_tan(kq_t degrees);
kq_t kq_cot(kq_t degrees);

/* ------------------------------------------------------------ clock state */

struct kaalka_time {
	u8 hour;    /* 0-11, as the reference reduces modulo 12 */
	u8 min;     /* 0-59 */
	u8 sec;     /* 0-59 */
};

struct kaalka_angles {
	kq_t hour_min;   /* angle between hour and minute hands, in degrees */
	kq_t min_sec;
	kq_t hour_sec;
};

void kaalka_set_time(struct kaalka_time *k, int h, int m, int s);
void kaalka_time_now(struct kaalka_time *k);
void kaalka_angles(const struct kaalka_time *k, struct kaalka_angles *out);

/* Quadrant-selected trig: sin, cos, tan, cot for quadrants 1 through 4. */
kq_t kaalka_select_trig(kq_t angle_deg);

/* -------------------------------------------------- classic byte transform */

/* Byte-for-byte compatible with the reference encrypt and decrypt, modulo the
 * fixed-point rounding described above. Kept so RESENTMENT can interoperate
 * with Kaalka payloads produced by the Python, JS, Java, Kotlin and Dart
 * implementations. Not used to protect kernel objects. */
void kaalka_classic_encrypt(const struct kaalka_time *k, const u8 *in, u8 *out, size_t len);
void kaalka_classic_decrypt(const struct kaalka_time *k, const u8 *in, u8 *out, size_t len);

/* The integer stream transform from the reference envelope path:
 * offset = (h*3600 + m*60 + s + index) mod 256. */
void kaalka_proc(const struct kaalka_time *k, const u8 *in, u8 *out, size_t len, bool encrypt);

/* --------------------------------------------------------------- epochs */

/* An epoch is a coarse slice of wall time. All keys derived inside one epoch
 * are identical; once the epoch rolls, they are unrecoverable. */
#define KAALKA_EPOCH_SECONDS 60u

typedef u64 kaalka_epoch_t;

kaalka_epoch_t kaalka_epoch_now(void);
kaalka_epoch_t kaalka_epoch_of(s64 unix_sec);
s64            kaalka_epoch_start(kaalka_epoch_t e);

/* Derive a key bound to an epoch and a purpose string. The Kaalka angles for
 * the epoch start are folded into the HKDF info, so the derivation is a pure
 * function of (master, epoch, purpose) and reproducible during replay. */
void kaalka_derive_key(kaalka_epoch_t epoch, const char *purpose,
                       u8 *out, size_t outlen);

void kaalka_init(void);
void kaalka_rekey(void);   /* called when the epoch rolls */

/* ----------------------------------------------------------------- seals */

#define KAALKA_SEAL_SIZE 32

/* A seal binds a payload digest to an identity, a sequence number and a time
 * window. Verification fails outside the window even with a perfect MAC: the
 * clock is part of the authorisation decision, not an advisory field. */
struct kaalka_seal {
	u8             mac[KAALKA_SEAL_SIZE];
	kaalka_epoch_t epoch;
	s64            not_before;   /* unix seconds, inclusive */
	s64            not_after;    /* unix seconds, exclusive */
	u64            seq;
	u64            subject;      /* task id, capability id, or graph node id */
};

void kaalka_seal_make(struct kaalka_seal *s, u64 subject, u64 seq,
                      const void *payload, size_t len, u64 lifetime_sec);
int  kaalka_seal_verify(const struct kaalka_seal *s, u64 subject,
                        const void *payload, size_t len);

/* -------------------------------------------------------- replay ledger */

/* Bounded, self-expiring set of (subject, seq) pairs already accepted. Bounded
 * because an unbounded ledger is a denial-of-service surface, self-expiring
 * because Kaalka already gives every entry a natural death. */
#define KAALKA_LEDGER_SLOTS 1024

struct kaalka_ledger {
	struct {
		u64            subject;
		u64            seq;
		kaalka_epoch_t epoch;
	} slot[KAALKA_LEDGER_SLOTS];
	spinlock_t lock;
	u64        accepted, rejected, evicted;
};

void kaalka_ledger_init(struct kaalka_ledger *l);
/* RK_OK when fresh and recorded, RK_EREPLAY when already seen. */
int  kaalka_ledger_check(struct kaalka_ledger *l, u64 subject, u64 seq);

/* --------------------------------------------------------- AEAD envelope */

/* The production path: ChaCha20 for confidentiality, HMAC-SHA256 for
 * integrity, both keyed by kaalka_derive_key, so the envelope inherits the
 * temporal properties without inheriting the weak cipher. */
struct kaalka_envelope {
	u32                ver;
	u32                ct_len;
	u64                sender;
	u64                receiver;
	u64                seq;
	kaalka_epoch_t     epoch;
	s64                timestamp;
	u8                 nonce[CHACHA20_NONCE_SIZE];
	u8                 _pad[4];
	struct kaalka_seal seal;
	/* ciphertext follows contiguously */
};

#define KAALKA_ENVELOPE_OVERHEAD (sizeof(struct kaalka_envelope))

int kaalka_envelope_seal(void *out, size_t outcap, size_t *outlen,
                         u64 sender, u64 receiver, u64 seq,
                         const void *pt, size_t ptlen, u64 lifetime_sec);
int kaalka_envelope_open(const void *in, size_t inlen,
                         u64 receiver, void *pt, size_t ptcap, size_t *ptlen,
                         struct kaalka_ledger *ledger);

struct kaalka_stats {
	u64 seals_made, seals_verified, seals_rejected, seals_expired;
	u64 envelopes_sealed, envelopes_opened, replays_blocked;
	u64 rekeys;
	kaalka_epoch_t current_epoch;
};
void kaalka_stats(struct kaalka_stats *out);

int kaalka_selftest(void);
