/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - host test suite.
 *
 * These run the real kernel sources against a synthetic machine, so they are
 * tests of the shipped code rather than of a reimplementation. They cover the
 * parts where a bug is silent and expensive: the allocator, the fixed-point
 * trig Kaalka depends on, the Merkle digests the graph promises are stable,
 * and the SHE sandbox.
 *
 * No framework. An assertion that prints what it expected and what it got is
 * the whole apparatus a test suite needs.
 */
#define RK_HOSTED 1

#include <rk/types.h>
#include <rk/string.h>
#include <rk/printf.h>
#include <rk/mm.h>
#include <rk/boot.h>
#include <rk/crypto.h>
#include <rk/kaalka.h>
#include <rk/graph.h>
#include <rk/she.h>
#include <rk/time.h>
#include <rk/errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rk_host_arena_init(void);
void rk_host_set_time(s64 t);
void rk_host_console_verbose(bool on);
void she_stdlib_bind(struct she_vm *vm);

typedef unsigned __int128 u128;
u128 rk_udivmod128(u128 n, u128 d, u128 *rem);

static int checks, failures;
static const char *current_suite = "";

/* memmem is not portable, and the assertion that a sealed envelope does not
 * contain its own plaintext is worth having. */
static bool memmem_present(const void *hay, size_t hlen,
                           const void *needle, size_t nlen)
{
	if (nlen > hlen)
		return false;
	const u8 *h = hay;
	for (size_t i = 0; i + nlen <= hlen; i++)
		if (memcmp(h + i, needle, nlen) == 0)
			return true;
	return false;
}

#define CHECK(cond, ...)                                            \
	do {                                                            \
		checks++;                                                   \
		if (!(cond)) {                                              \
			failures++;                                             \
			printf("    FAIL %s:%d  ", __func__, __LINE__);         \
			printf(__VA_ARGS__);                                    \
			printf("\n");                                           \
		}                                                           \
	} while (0)

#define SUITE(name) do { current_suite = name; printf("  %s\n", name); } while (0)

/* ------------------------------------------------------------- strings */

static void test_strings(void)
{
	SUITE("strings");

	char buf[8];
	CHECK(strlcpy(buf, "hello", sizeof(buf)) == 5, "strlcpy length wrong");
	CHECK(strcmp(buf, "hello") == 0, "strlcpy content wrong: %s", buf);

	/* Truncation must still terminate and must report what it wanted. */
	CHECK(strlcpy(buf, "much too long", sizeof(buf)) == 13,
	      "strlcpy did not report the full source length");
	CHECK(strlen(buf) == 7, "strlcpy overflowed: %llu",
	      (unsigned long long)strlen(buf));
	CHECK(buf[7] == '\0', "strlcpy did not terminate");

	strlcpy(buf, "ab", sizeof(buf));
	CHECK(strlcat(buf, "cdefghij", sizeof(buf)) == 10,
	      "strlcat did not report the wanted length");
	CHECK(strcmp(buf, "abcdefg") == 0, "strlcat content wrong: %s", buf);

	CHECK(rk_ct_eq("abcd", "abcd", 4), "constant-time compare said equal is not");
	CHECK(!rk_ct_eq("abcd", "abce", 4), "constant-time compare said unequal is");

	CHECK(rk_strtou64("1234", NULL, 10) == 1234, "decimal parse");
	CHECK(rk_strtou64("0xff", NULL, 0) == 255, "hex parse with prefix");
	CHECK(rk_strtou64("0b1010", NULL, 0) == 10, "binary parse");
	CHECK(rk_strtos64("-42", NULL, 10) == -42, "negative parse");
}

/* -------------------------------------------------------------- printf */

static void test_printf(void)
{
	SUITE("printf");

	char b[64];

	rk_snprintf(b, sizeof(b), "%d %u %x %o", -5, 5u, 255u, 8u);
	CHECK(strcmp(b, "-5 5 ff 10") == 0, "integer conversions: %s", b);

	rk_snprintf(b, sizeof(b), "[%5d][%-5d][%05d]", 42, 42, 42);
	CHECK(strcmp(b, "[   42][42   ][00042]") == 0, "width and padding: %s", b);

	rk_snprintf(b, sizeof(b), "%s|%.3s|%8s", "abc", "abcdef", "xy");
	CHECK(strcmp(b, "abc|abc|      xy") == 0, "string conversions: %s", b);

	rk_snprintf(b, sizeof(b), "%llu", 18446744073709551615ull);
	CHECK(strcmp(b, "18446744073709551615") == 0, "unsigned 64-bit max: %s", b);

	/* The value that breaks a naive negation. */
	rk_snprintf(b, sizeof(b), "%lld", (long long)INT64_MIN);
	CHECK(strcmp(b, "-9223372036854775808") == 0, "INT64_MIN: %s", b);

	rk_snprintf(b, sizeof(b), "%#x %#b", 255u, 5u);
	CHECK(strcmp(b, "0xff 0b101") == 0, "alternate forms: %s", b);

	/* Truncation must terminate and report the length it wanted, per C99. */
	int want = rk_snprintf(b, 5, "abcdefgh");
	CHECK(want == 8, "snprintf returned %d, expected 8", want);
	CHECK(strlen(b) == 4, "snprintf wrote past the buffer");

	rk_snprintf(b, sizeof(b), "%pB", RK_BYTES(1536));
	CHECK(strcmp(b, "1.5 KiB") == 0, "byte size formatting: %s", b);
	rk_snprintf(b, sizeof(b), "%pB", RK_BYTES(4ull << 30));
	CHECK(strcmp(b, "4.0 GiB") == 0, "byte size formatting: %s", b);
}

/* ---------------------------------------------------------- 128-bit maths */

static void test_int128(void)
{
	SUITE("128-bit division");

	u128 rem = 0;
	CHECK(rk_udivmod128(100, 7, &rem) == 14, "small quotient");
	CHECK(rem == 2, "small remainder");

	/* The case the kernel actually hits: a large product divided by a clock
	 * rate. 3 GHz for an hour is far past a 64-bit product. */
	u128 cycles = (u128)3000000000ull * 3600ull;
	u128 ns = rk_udivmod128(cycles * 1000000000ull, 3000000000ull, NULL);
	CHECK(ns == (u128)3600ull * 1000000000ull,
	      "cycles-to-nanoseconds conversion overflowed");

	u128 big = ((u128)1 << 100) + 12345;
	CHECK(rk_udivmod128(big, 1, NULL) == big, "division by one");
	CHECK(rk_udivmod128(big, big, NULL) == 1, "division by self");
	rk_udivmod128(big, 1000, &rem);
	CHECK(rem == big % 1000, "remainder of a 100-bit value");
}

/* ------------------------------------------------------------ allocator */

static void test_allocator(void)
{
	SUITE("physical and heap allocator");

	struct pmm_stats before, after;
	pmm_stats(&before);
	CHECK(before.free_pages > 1000, "the arena produced only %llu free pages",
	      (unsigned long long)before.free_pages);

	/* Every allocation must be distinct and correctly sized. Overlap here is
	 * the bug class that shows up a thousand lines away. */
	enum { N = 200 };
	void *p[N];
	size_t sz[N];
	for (int i = 0; i < N; i++) {
		sz[i] = (size_t)(8 + (i * 37) % 3000);
		p[i] = kmalloc(sz[i]);
		CHECK(p[i] != NULL, "kmalloc(%llu) returned nothing",
		      (unsigned long long)sz[i]);
		if (p[i])
			memset(p[i], i & 0xFF, sz[i]);
	}
	for (int i = 0; i < N; i++) {
		if (!p[i])
			continue;
		u8 *q = p[i];
		int bad = -1;
		for (size_t k = 0; k < sz[i]; k++)
			if (q[k] != (u8)(i & 0xFF)) {
				bad = (int)k;
				break;
			}
		CHECK(bad < 0, "block %d corrupted at byte %d", i, bad);
		CHECK(ksize(p[i]) == sz[i], "ksize said %llu for a %llu byte block",
		      (unsigned long long)ksize(p[i]), (unsigned long long)sz[i]);
	}
	for (int i = 0; i < N; i++)
		kfree(p[i]);

	/* The buddy allocator must hand back what it took. */
	pmm_stats(&after);
	paddr_t big = pmm_alloc_pages(6, 0);
	CHECK(big != 0, "cannot allocate 64 contiguous pages");
	CHECK((big & (RK_PAGE_SIZE * 64 - 1)) == 0,
	      "a 64-page block came back misaligned: %#llx",
	      (unsigned long long)big);
	pmm_free_pages(big, 6);

	struct pmm_stats end;
	pmm_stats(&end);
	CHECK(end.free_pages == after.free_pages,
	      "%lld pages leaked across an allocate and free cycle",
	      (long long)after.free_pages - (long long)end.free_pages);

	/* Alignment requests must be honoured, since the FPU state depends on it. */
	void *a64 = kmalloc_aligned(200, 64);
	CHECK(a64 != NULL && ((uintptr_t)a64 & 63) == 0,
	      "64-byte aligned allocation came back at %p", a64);
	kfree(a64);

	/* Overflow in kcalloc must be refused, not wrapped. */
	CHECK(kcalloc((size_t)-1 / 2, 4) == NULL,
	      "kcalloc did not refuse an overflowing size");
}

/* --------------------------------------------------------------- crypto */

static void test_crypto(void)
{
	SUITE("crypto");
	CHECK(rk_crypto_selftest() == RK_OK, "the published known-answer vectors failed");

	/* Entropy must actually vary. */
	u64 a = rk_random_u64(), b = rk_random_u64();
	CHECK(a != b, "two consecutive random values were identical");

	/* SipHash must be key-dependent, or hash tables are not protected. */
	u8 k1[16], k2[16];
	memset(k1, 1, sizeof(k1));
	memset(k2, 2, sizeof(k2));
	CHECK(rk_siphash("abc", 3, k1) != rk_siphash("abc", 3, k2),
	      "SipHash gave the same result under two different keys");

	CHECK(rk_crc32(0, "123456789", 9) == 0xCBF43926u,
	      "CRC32 check value wrong: %#x", rk_crc32(0, "123456789", 9));
}

/* --------------------------------------------------------------- kaalka */

static void test_kaalka(void)
{
	SUITE("kaalka");

	/* Fixed-point trig against values everyone knows, to within the
	 * resolution the format actually has. */
	struct { int deg; double want; } sines[] = {
		{ 0, 0.0 }, { 30, 0.5 }, { 90, 1.0 }, { 150, 0.5 }, { 180, 0.0 },
		{ 210, -0.5 }, { 270, -1.0 }, { 330, -0.5 }, { 360, 0.0 },
	};
	for (size_t i = 0; i < sizeof(sines) / sizeof(sines[0]); i++) {
		kq_t got = kq_sin(kq_from_int(sines[i].deg));
		double d = (double)got / 4294967296.0;
		CHECK(d > sines[i].want - 0.0005 && d < sines[i].want + 0.0005,
		      "sin(%d) = %.6f, expected %.6f", sines[i].deg, d, sines[i].want);
	}

	kq_t c0 = kq_cos(0);
	CHECK((double)c0 / 4294967296.0 > 0.9995, "cos(0) is not 1");
	kq_t c90 = kq_cos(kq_from_int(90));
	CHECK((double)c90 / 4294967296.0 < 0.0005 &&
	      (double)c90 / 4294967296.0 > -0.0005, "cos(90) is not 0");

	/* Determinism: the same input must give the same output, every time. */
	for (int deg = 0; deg < 360; deg += 7)
		CHECK(kq_sin(kq_from_int(deg)) == kq_sin(kq_from_int(deg)),
		      "kq_sin(%d) is not deterministic", deg);

	/* Clock angles at a known time. At 3:00:00 the hour and minute hands are
	 * exactly 90 degrees apart, which is the one everybody can check. */
	struct kaalka_time kt;
	struct kaalka_angles ang;
	kaalka_set_time(&kt, 3, 0, 0);
	kaalka_angles(&kt, &ang);
	CHECK(ang.hour_min == kq_from_int(90),
	      "the hands at 3:00:00 are %lld/2^32 degrees apart, expected 90",
	      (long long)ang.hour_min);

	/* And the hour is reduced modulo 12, as the reference does. */
	kaalka_set_time(&kt, 15, 30, 45);
	CHECK(kt.hour == 3, "15:30:45 reduced to hour %u, expected 3", kt.hour);

	/* Round trip through both transforms, at several times of day. */
	const char *msg = "RESENTMENT: time is the key";
	size_t n = strlen(msg);
	u8 ct[64], back[64];

	int times[][3] = { {0,0,0}, {12,34,56}, {23,59,59}, {6,15,30}, {9,45,0} };
	for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
		kaalka_set_time(&kt, times[i][0], times[i][1], times[i][2]);

		kaalka_classic_encrypt(&kt, (const u8 *)msg, ct, n);
		kaalka_classic_decrypt(&kt, ct, back, n);
		CHECK(memcmp(back, msg, n) == 0,
		      "classic transform failed to round trip at %02d:%02d:%02d",
		      times[i][0], times[i][1], times[i][2]);

		kaalka_proc(&kt, (const u8 *)msg, ct, n, true);
		kaalka_proc(&kt, ct, back, n, false);
		CHECK(memcmp(back, msg, n) == 0,
		      "stream transform failed to round trip at %02d:%02d:%02d",
		      times[i][0], times[i][1], times[i][2]);
	}

	/* Different second, different ciphertext: otherwise time is decorative. */
	struct kaalka_time k1, k2;
	u8 c1[64], c2[64];
	kaalka_set_time(&k1, 10, 20, 30);
	kaalka_set_time(&k2, 10, 20, 31);
	kaalka_classic_encrypt(&k1, (const u8 *)msg, c1, n);
	kaalka_classic_encrypt(&k2, (const u8 *)msg, c2, n);
	CHECK(memcmp(c1, c2, n) != 0, "one second later produced identical output");

	/* Seals. */
	kaalka_init();
	struct kaalka_seal seal;
	const char *payload = "a capability";
	kaalka_seal_make(&seal, 0x1234, 1, payload, strlen(payload), 60);

	CHECK(kaalka_seal_verify(&seal, 0x1234, payload, strlen(payload)) == RK_OK,
	      "a fresh seal did not verify");
	CHECK(kaalka_seal_verify(&seal, 0x9999, payload, strlen(payload)) != RK_OK,
	      "a seal verified for the wrong subject");
	CHECK(kaalka_seal_verify(&seal, 0x1234, "different", 9) != RK_OK,
	      "a seal verified over the wrong payload");

	/* Tampering with any covered field must be caught. */
	struct kaalka_seal widened = seal;
	widened.not_after += 100000;
	CHECK(kaalka_seal_verify(&widened, 0x1234, payload, strlen(payload)) != RK_OK,
	      "extending the validity window was not detected");

	struct kaalka_seal expired = seal;
	expired.not_after = rk_unix_time() - 1;
	CHECK(kaalka_seal_verify(&expired, 0x1234, payload, strlen(payload)) == RK_EEXPIRED,
	      "an expired seal was not refused");

	/* Envelopes and replay. */
	static u8 env[512];
	size_t envlen = 0, ptlen = 0;
	u8 plain[128];
	CHECK(kaalka_envelope_seal(env, sizeof(env), &envlen, 1, 2, 7,
	                           msg, n, 60) == RK_OK, "envelope seal failed");
	CHECK(memmem_present(env, envlen, msg, n) == false,
	      "the envelope contains its plaintext");

	static struct kaalka_ledger ledger;
	kaalka_ledger_init(&ledger);
	CHECK(kaalka_envelope_open(env, envlen, 2, plain, sizeof(plain),
	                           &ptlen, &ledger) == RK_OK, "envelope open failed");
	CHECK(ptlen == n && memcmp(plain, msg, n) == 0,
	      "envelope did not round trip");
	CHECK(kaalka_envelope_open(env, envlen, 2, plain, sizeof(plain),
	                           &ptlen, &ledger) == RK_EREPLAY,
	      "a replayed envelope was accepted");
	CHECK(kaalka_envelope_open(env, envlen, 3, plain, sizeof(plain),
	                           &ptlen, NULL) == RK_EACCES,
	      "an envelope opened for the wrong receiver");

	/* Corrupting one ciphertext byte must be detected. */
	env[sizeof(struct kaalka_envelope) + 2] ^= 0x01;
	CHECK(kaalka_envelope_open(env, envlen, 2, plain, sizeof(plain),
	                           &ptlen, NULL) != RK_OK,
	      "a corrupted envelope was accepted");
}

/* --------------------------------------------------------------- calendar */

static void test_calendar(void)
{
	SUITE("calendar");

	struct rk_tm tm;
	rk_gmtime(0, &tm);
	CHECK(tm.year == 1970 && tm.month == 1 && tm.day == 1 && tm.wday == 4,
	      "the epoch decoded as %04d-%02d-%02d wday %d",
	      tm.year, tm.month, tm.day, tm.wday);

	rk_gmtime(1735689600, &tm);
	CHECK(tm.year == 2025 && tm.month == 1 && tm.day == 1,
	      "1735689600 decoded as %04d-%02d-%02d", tm.year, tm.month, tm.day);

	/* The leap day that catches every naive implementation. */
	rk_gmtime(1709164800, &tm);
	CHECK(tm.year == 2024 && tm.month == 2 && tm.day == 29,
	      "2024-02-29 decoded as %04d-%02d-%02d", tm.year, tm.month, tm.day);

	/* 1900 is not a leap year, 2000 is. */
	rk_gmtime(951782400, &tm);
	CHECK(tm.year == 2000 && tm.month == 2 && tm.day == 29,
	      "2000-02-29 decoded as %04d-%02d-%02d", tm.year, tm.month, tm.day);

	/* Round trip over a wide range of instants. */
	for (s64 t = 0; t < 2000000000; t += 86400 * 37) {
		rk_gmtime(t, &tm);
		s64 back = rk_mktime(&tm);
		CHECK(back == t, "gmtime/mktime round trip failed at %lld: got %lld",
		      (long long)t, (long long)back);
		if (back != t)
			break;
	}

	char buf[40];
	rk_format_time(buf, sizeof(buf), 1735689600);
	CHECK(strcmp(buf, "2025-01-01T00:00:00Z") == 0, "ISO formatting: %s", buf);
}

/* ----------------------------------------------------------------- graph */

static void test_graph(void)
{
	SUITE("runtime graph");

	rk_graph_init();

	struct graph_node *a = rk_graph_node_create(GNODE_TASK, "alpha", NULL, NULL);
	struct graph_node *b = rk_graph_node_create(GNODE_TASK, "alpha", NULL, NULL);
	CHECK(a && b, "cannot create graph nodes");

	rk_graph_set_u64(a, "pid", 7);
	rk_graph_set_u64(b, "pid", 7);

	u8 da[32], db[32];
	memcpy(da, rk_graph_digest(a), 32);
	memcpy(db, rk_graph_digest(b), 32);
	CHECK(memcmp(da, db, 32) == 0,
	      "two nodes with identical content hashed differently");

	/* Attribute order must not matter: the canonical encoding sorts keys. */
	struct graph_node *c = rk_graph_node_create(GNODE_TASK, "beta", NULL, NULL);
	struct graph_node *d = rk_graph_node_create(GNODE_TASK, "beta", NULL, NULL);
	rk_graph_set_u64(c, "aaa", 1);
	rk_graph_set_u64(c, "zzz", 2);
	rk_graph_set_u64(d, "zzz", 2);
	rk_graph_set_u64(d, "aaa", 1);
	CHECK(memcmp(rk_graph_digest(c), rk_graph_digest(d), 32) == 0,
	      "attribute insertion order changed the digest");

	/* Any change must move the digest, and must reach the root. */
	u8 root_before[32];
	memcpy(root_before, rk_graph_root_digest(), 32);
	rk_graph_set_u64(a, "pid", 8);
	CHECK(memcmp(da, rk_graph_digest(a), 32) != 0,
	      "changing an attribute did not change the node digest");
	CHECK(memcmp(root_before, rk_graph_root_digest(), 32) != 0,
	      "a change deep in the graph did not reach the root digest");

	/* The root must be stable when nothing changes. */
	u8 stable[32];
	memcpy(stable, rk_graph_root_digest(), 32);
	CHECK(memcmp(stable, rk_graph_root_digest(), 32) == 0,
	      "the root digest changed without any mutation");

	/* Export must be deterministic for the same state. */
	char e1[8192], e2[8192];
	size_t n1 = rk_graph_export(NULL, GRAPH_FMT_CANON, -1, e1, sizeof(e1));
	size_t n2 = rk_graph_export(NULL, GRAPH_FMT_CANON, -1, e2, sizeof(e2));
	CHECK(n1 == n2 && memcmp(e1, e2, n1) == 0,
	      "two exports of the same state differed");

	/* And a diff of identical exports must be empty. */
	char diff[4096];
	size_t dn = rk_graph_diff(e1, n1, e2, n2, diff, sizeof(diff));
	CHECK(dn == 0, "diffing identical exports produced %llu bytes",
	      (unsigned long long)dn);

	/* Events. */
	u64 seq_before = rk_graph_event_seq();
	rk_graph_record(GEV_NODE_UPDATE, a->id, 1, 2, 3);
	CHECK(rk_graph_event_seq() == seq_before + 1, "an event was not recorded");

	struct graph_event ev[4];
	u64 cursor = seq_before;
	size_t got = rk_graph_events_read(ev, 4, &cursor);
	CHECK(got >= 1 && ev[0].a == 1 && ev[0].b == 2 && ev[0].c == 3,
	      "the recorded event did not read back");
}

/* ---------------------------------------------------------- memory fabric */

static void test_memfab(void)
{
	SUITE("federated memory fabric");

	rk_memfab_init(1u << 20);

	CHECK(rk_memfab_put_str("greeting", "hello", 5) == RK_OK, "put failed");

	char out[32];
	size_t n = rk_memfab_get_str("greeting", out, sizeof(out));
	CHECK(n == 5 && memcmp(out, "hello", 5) == 0,
	      "get returned %llu bytes", (unsigned long long)n);

	CHECK(rk_memfab_get_str("absent", out, sizeof(out)) == 0,
	      "a key that was never stored returned data");

	/* Serialise, merge into the same fabric, and confirm it is idempotent -
	 * that property is what makes merging two machines safe. */
	size_t need = rk_memfab_serialize(NULL, 0);
	void *blob = malloc(need);
	size_t wrote = rk_memfab_serialize(blob, need);
	CHECK(wrote > 0 && wrote <= need, "serialise wrote %llu of %llu",
	      (unsigned long long)wrote, (unsigned long long)need);

	u64 entries_before, bytes_before, h, m;
	rk_memfab_stats(&entries_before, &bytes_before, &h, &m);
	CHECK(rk_memfab_merge(blob, wrote) == RK_OK, "merge failed");

	u64 entries_after, bytes_after;
	rk_memfab_stats(&entries_after, &bytes_after, &h, &m);
	CHECK(entries_after == entries_before,
	      "merging a fabric into itself changed the entry count from %llu to %llu",
	      (unsigned long long)entries_before, (unsigned long long)entries_after);
	free(blob);
}

/* ------------------------------------------------------------------- she */

struct capture {
	char buf[4096];
	size_t len;
};

static void capture_out(void *ctx, const char *s, size_t n)
{
	struct capture *c = ctx;
	for (size_t i = 0; i < n && c->len + 1 < sizeof(c->buf); i++)
		c->buf[c->len++] = s[i];
	c->buf[c->len] = '\0';
}

static struct she_vm *fresh_vm(struct capture *cap, u32 allow)
{
	static struct she_vm vm;
	she_vm_init(&vm, NULL, allow);
	she_stdlib_bind(&vm);
	she_vm_set_limits(&vm, 2000000, 4u << 20);
	if (cap) {
		cap->len = 0;
		cap->buf[0] = '\0';
		she_vm_set_io(&vm, cap, capture_out, NULL);
	}
	return &vm;
}

static void expect_num(const char *src, s64 want)
{
	struct she_vm *vm = fresh_vm(NULL, SHE_ALLOW_ALL);
	struct she_value r = SHE_NOTHING_V;
	int rc = she_eval(vm, src, "test", &r);

	CHECK(rc == RK_OK, "\"%s\" failed: %s", src, vm->error[0] ? vm->error : "?");
	if (rc == RK_OK)
		CHECK(r.type == SHE_NUM && r.as.n == want,
		      "\"%s\" gave %lld, expected %lld",
		      src, (long long)(r.type == SHE_NUM ? r.as.n : -999999),
		      (long long)want);
	she_vm_free(vm);
}

static void expect_say(const char *src, const char *want)
{
	struct capture cap;
	struct she_vm *vm = fresh_vm(&cap, SHE_ALLOW_ALL);
	struct she_value r;
	int rc = she_eval(vm, src, "test", &r);

	CHECK(rc == RK_OK, "\"%s\" failed: %s", src, vm->error[0] ? vm->error : "?");
	CHECK(strcmp(cap.buf, want) == 0,
	      "\"%s\" printed \"%s\", expected \"%s\"", src, cap.buf, want);
	she_vm_free(vm);
}

static void test_she(void)
{
	SUITE("she language");

	expect_num("return 2 + 3 * 4", 14);
	expect_num("return (2 + 3) * 4", 20);
	expect_num("return 17 % 5", 2);
	expect_num("return 2 ^ 10", 1024);
	expect_num("return -5 + 10", 5);

	expect_num("let x = 10\nx = x + 5\nreturn x", 15);

	expect_num("if 1 is 1 then\n return 1\nelse\n return 2\nend", 1);
	expect_num("if 1 is not 1 then\n return 1\nelse\n return 2\nend", 2);

	expect_num("let t = 0\nfor each n in 1 to 10\n t = t + n\nend\nreturn t", 55);
	expect_num("let i = 0\nwhile i < 5\n i = i + 1\nend\nreturn i", 5);
	expect_num("let i = 0\nrepeat\n i = i + 1\nuntil i is 3\nreturn i", 3);

	expect_num("let t = 0\nfor each n in 1 to 10\n if n > 5 then\n break\n end\n"
	           " t = t + n\nend\nreturn t", 15);

	expect_num("fun add(a, b)\n return a + b\nend\nreturn add(3, 4)", 7);
	expect_num("fun fact(n)\n if n <= 1 then\n  return 1\n end\n"
	           " return n * fact(n - 1)\nend\nreturn fact(6)", 720);

	expect_num("return length([1, 2, 3])", 3);
	expect_num("return sum([1, 2, 3, 4])", 10);
	expect_num("return [1, 2, 3][1]", 2);
	expect_num("return [1, 2, 3][-1]", 3);

	/* The pipeline, which is the shape the language is built around. */
	expect_num("return [1, 2, 3, 4, 5, 6] |> filter(fun(n) -> n % 2 is 0) "
	           "|> map(fun(n) -> n * 10) |> sum()", 120);

	expect_num("let m = {name: \"x\", size: 5}\nreturn m[\"size\"]", 5);
	expect_num("let m = {a: 1}\nreturn m.a", 1);

	expect_say("say \"hello\"", "hello\n");
	expect_say("let who = \"world\"\nsay \"hello, {who}!\"", "hello, world!\n");
	expect_say("say 1 + 1", "2\n");
	expect_say("say [1, 2]", "[1, 2]\n");
	expect_say("say true", "yes\n");

	/* Text and numbers add the way people expect them to. */
	expect_say("say \"n = \" + 42", "n = 42\n");

	SUITE("she sandbox");

	/* A VM with no grants must refuse anything that reaches outside. */
	{
		struct she_vm *vm = fresh_vm(NULL, 0);
		struct she_value r;
		int rc = she_eval(vm, "return now()", "test", &r);
		CHECK(rc != RK_OK, "a script with no permissions read the clock");
		CHECK(strstr(vm->error, "--allow-time") != NULL,
		      "the refusal did not name the flag: %s", vm->error);
		she_vm_free(vm);
	}
	{
		struct she_vm *vm = fresh_vm(NULL, SHE_ALLOW_TIME);
		struct she_value r;
		int rc = she_eval(vm, "return now()", "test", &r);
		CHECK(rc == RK_OK, "granting time did not allow reading the clock: %s",
		      vm->error);
		she_vm_free(vm);
	}
	{
		/* A pure computation must still work with zero permissions. */
		struct she_vm *vm = fresh_vm(NULL, 0);
		struct she_value r;
		int rc = she_eval(vm, "return sum([1,2,3])", "test", &r);
		CHECK(rc == RK_OK && r.as.n == 6,
		      "a pure computation needed a permission");
		she_vm_free(vm);
	}

	SUITE("she limits");

	{
		/* An unbounded loop must stop rather than hang the machine. */
		struct she_vm *vm = fresh_vm(NULL, 0);
		she_vm_set_limits(vm, 5000, 1u << 20);
		struct she_value r;
		int rc = she_eval(vm, "while true\n let x = 1\nend", "test", &r);
		CHECK(rc == RK_EAGAIN, "an infinite loop returned %d, expected the "
		      "gas budget to stop it", rc);
		she_vm_free(vm);
	}
	{
		/* Runaway recursion must produce a diagnostic, not a stack overflow. */
		struct she_vm *vm = fresh_vm(NULL, 0);
		struct she_value r;
		int rc = she_eval(vm, "fun f(n)\n return f(n + 1)\nend\nreturn f(0)",
		                  "test", &r);
		CHECK(rc != RK_OK, "unbounded recursion was not caught");
		CHECK(strstr(vm->error, "nested calls") != NULL ||
		      strstr(vm->error, "budget") != NULL,
		      "the recursion diagnostic was unhelpful: %s", vm->error);
		she_vm_free(vm);
	}
	{
		/* Division by zero must be a message, not a fault. */
		struct she_vm *vm = fresh_vm(NULL, 0);
		struct she_value r;
		int rc = she_eval(vm, "return 1 / 0", "test", &r);
		CHECK(rc != RK_OK && strstr(vm->error, "zero") != NULL,
		      "dividing by zero gave: %s", vm->error);
		she_vm_free(vm);
	}

	SUITE("she diagnostics");

	{
		struct she_vm *vm = fresh_vm(NULL, 0);
		struct she_value r;
		she_eval(vm, "let x = \nsay x", "test", &r);
		CHECK(vm->error[0] != '\0', "a syntax error produced no message");
		CHECK(strstr(vm->error, "line") != NULL,
		      "the syntax error did not name a line: %s", vm->error);
		she_vm_free(vm);
	}
	{
		struct she_vm *vm = fresh_vm(NULL, 0);
		struct she_value r;
		she_eval(vm, "return undefined_name", "test", &r);
		CHECK(strstr(vm->error, "undefined_name") != NULL,
		      "an unknown name error did not quote the name: %s", vm->error);
		CHECK(strstr(vm->error, "let") != NULL,
		      "the unknown name error did not suggest a fix: %s", vm->error);
		she_vm_free(vm);
	}
}

/* ------------------------------------------------------------------ main */

/* Emit Kaalka test vectors for tools/kaalka_ref.py to compare against the
 * reference implementation. Keeping the producer inside the test binary means
 * the vectors always come from the code that actually ships. */
static void emit_vectors(void)
{
	static const char *messages[] = {
		"Hello", "Time is the Key", "RESENTMENT",
		"abcdefghijklmnopqrstuvwxyz0123456789",
	};
	static const int times[][3] = {
		{ 0, 0, 0 }, { 12, 34, 56 }, { 3, 0, 0 }, { 23, 59, 59 },
		{ 6, 15, 30 }, { 9, 45, 0 }, { 1, 2, 3 }, { 11, 11, 11 },
	};

	printf("# RESENTMENT Kaalka vectors: h m s message_hex classic_hex proc_hex\n");
	for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++) {
		for (size_t m = 0; m < sizeof(messages) / sizeof(messages[0]); m++) {
			struct kaalka_time kt;
			kaalka_set_time(&kt, times[t][0], times[t][1], times[t][2]);
			size_t n = strlen(messages[m]);
			u8 classic[128], proc[128];
			kaalka_classic_encrypt(&kt, (const u8 *)messages[m], classic, n);
			kaalka_proc(&kt, (const u8 *)messages[m], proc, n, true);

			printf("%d %d %d ", times[t][0], times[t][1], times[t][2]);
			for (size_t i = 0; i < n; i++) printf("%02x", (u8)messages[m][i]);
			printf(" ");
			for (size_t i = 0; i < n; i++) printf("%02x", classic[i]);
			printf(" ");
			for (size_t i = 0; i < n; i++) printf("%02x", proc[i]);
			printf("\n");
		}
	}

	/* The trig core, sampled every degree, so a port to another architecture
	 * can be checked against exactly these integers. */
	printf("# trig: degrees sin cos tan cot selected\n");
	for (int d = 0; d < 360; d++) {
		printf("%d %lld %lld %lld %lld %lld\n", d,
		       (long long)kq_sin(kq_from_int(d)),
		       (long long)kq_cos(kq_from_int(d)),
		       (long long)kq_tan(kq_from_int(d)),
		       (long long)kq_cot(kq_from_int(d)),
		       (long long)kaalka_select_trig(kq_from_int(d)));
	}
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--vectors") == 0) {
		rk_host_console_verbose(false);
		emit_vectors();
		return 0;
	}

	bool verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
	rk_host_console_verbose(verbose);

	printf("RESENTMENT host test suite\n\n");

	rk_host_arena_init();
	pmm_init(&rk_boot_info);
	mm_init(&rk_boot_info);
	rk_crypto_init();
	she_stdlib_init();

	test_strings();
	test_printf();
	test_int128();
	test_allocator();
	test_crypto();
	test_calendar();
	test_kaalka();
	test_graph();
	test_memfab();
	test_she();

	printf("\n%d checks, %d failure%s\n",
	       checks, failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
