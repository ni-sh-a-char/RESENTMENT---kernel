/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - boot-time self-tests.
 *
 * These run on every boot, not in a test harness somebody has to remember to
 * invoke. A kernel whose crypto is wrong, whose allocator loses pages or whose
 * seals do not expire should not be able to look healthy, and the only way to
 * guarantee that is to check on the machine that is about to be trusted.
 *
 * Each test is cheap - the whole set adds a few milliseconds to boot.
 */
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/crypto.h>
#include <rk/kaalka.h>
#include <rk/graph.h>
#include <rk/cap.h>
#include <rk/ai.h>
#include <rk/she.h>
#include <rk/console.h>

#undef RK_SUBSYS
#define RK_SUBSYS "selftest"

static int test_allocator(void)
{
	struct pmm_stats before, after;
	pmm_stats(&before);

	/* Allocate a spread of sizes, write a pattern, verify it, free. Catches
	 * the class of bug where two allocations overlap, which is otherwise
	 * found much later and much further away. */
	void *ptrs[16];
	size_t sizes[16];
	for (int i = 0; i < 16; i++) {
		sizes[i] = (size_t)(16u << (i % 10));
		ptrs[i] = kmalloc(sizes[i]);
		if (!ptrs[i]) {
			pr_err("allocator: kmalloc(%llu) failed",
			       (unsigned long long)sizes[i]);
			return RK_ENOMEM;
		}
		memset(ptrs[i], 0xA0 + i, sizes[i]);
	}
	for (int i = 0; i < 16; i++) {
		u8 *p = ptrs[i];
		for (size_t k = 0; k < sizes[i]; k++) {
			if (p[k] != (u8)(0xA0 + i)) {
				pr_err("allocator: block %d corrupted at offset %llu",
				       i, (unsigned long long)k);
				return RK_EIO;
			}
		}
		if (ksize(ptrs[i]) != sizes[i]) {
			pr_err("allocator: ksize reported %llu for a %llu byte block",
			       (unsigned long long)ksize(ptrs[i]),
			       (unsigned long long)sizes[i]);
			return RK_EIO;
		}
	}
	for (int i = 0; i < 16; i++)
		kfree(ptrs[i]);

	/* A page allocated and freed must come back to the free pool.
	 *
	 * Measured around only this pair, not around the kmalloc loop above: slab
	 * caches deliberately keep their empty slabs, so pages legitimately do not
	 * return there, and comparing across the whole test would report that
	 * design decision as a leak. */
	pmm_stats(&before);
	paddr_t pa = pmm_alloc_pages(3, 0);
	if (!pa) {
		pr_err("allocator: cannot allocate 8 contiguous pages");
		return RK_ENOMEM;
	}
	if (!IS_ALIGNED(pa, RK_PAGE_SIZE * 8)) {
		pr_err("allocator: an 8-page block came back misaligned at %#llx",
		       (unsigned long long)pa);
		return RK_EIO;
	}
	pmm_free_pages(pa, 3);
	pmm_stats(&after);
	if (after.free_pages != before.free_pages) {
		pr_err("allocator: %lld pages leaked across allocate and free",
		       (long long)before.free_pages - (long long)after.free_pages);
		return RK_EIO;
	}

	pr_info("allocator self-test passed");
	return RK_OK;
}

static int test_graph(void)
{
	/* The same label deliberately: identity is content, and two nodes with
	 * identical content must hash identically. Giving them different names
	 * would be testing that different things differ, which proves nothing. */
	struct graph_node *a = rk_graph_node_create(GNODE_LOGSPAN, "selftest", NULL, NULL);
	struct graph_node *b = rk_graph_node_create(GNODE_LOGSPAN, "selftest", NULL, NULL);
	if (!a || !b)
		return RK_ENOMEM;

	rk_graph_set_u64(a, "value", 42);
	rk_graph_set_u64(b, "value", 42);

	/* Two nodes with identical content must hash identically: that identity
	 * is what the whole diff and attestation story rests on. */
	u8 da[RK_GRAPH_DIGEST_SIZE], db[RK_GRAPH_DIGEST_SIZE];
	memcpy(da, rk_graph_digest(a), sizeof(da));
	memcpy(db, rk_graph_digest(b), sizeof(db));
	if (memcmp(da, db, sizeof(da)) != 0) {
		pr_err("graph: identical nodes produced different digests");
		return RK_EIO;
	}

	/* And a change must move the digest. */
	rk_graph_set_u64(b, "value", 43);
	memcpy(db, rk_graph_digest(b), sizeof(db));
	if (memcmp(da, db, sizeof(da)) == 0) {
		pr_err("graph: changing a node did not change its digest");
		return RK_EIO;
	}

	/* The root must have noticed. */
	u8 root_before[RK_GRAPH_DIGEST_SIZE];
	memcpy(root_before, rk_graph_root_digest(), sizeof(root_before));
	rk_graph_set_u64(a, "value", 99);
	if (memcmp(root_before, rk_graph_root_digest(), sizeof(root_before)) == 0) {
		pr_err("graph: a change did not propagate to the root digest");
		return RK_EIO;
	}

	rk_graph_node_destroy(a);
	rk_graph_node_destroy(b);
	pr_info("graph self-test passed: digest stability and propagation");
	return RK_OK;
}

static int test_capabilities(void)
{
	struct capspace *cs = capspace_create();
	if (!cs)
		return RK_ENOMEM;

	int dummy = 0;
	struct cap_object *obj = cap_object_create(CAP_MEMORY, &dummy, "selftest", NULL);
	if (!obj) {
		capspace_destroy(cs);
		return RK_ENOMEM;
	}

	cap_handle_t h = cap_install(cs, obj, CAP_RIGHT_READ | CAP_RIGHT_WRITE |
	                             CAP_RIGHT_DERIVE, 0xBEEF, 60);
	int rc = RK_OK;
	struct cap_object *got = NULL;

	if (h == CAP_INVALID) {
		pr_err("capability: install failed");
		rc = RK_EIO;
		goto out;
	}
	if (cap_lookup(cs, h, CAP_MEMORY, CAP_RIGHT_READ, &got) != RK_OK) {
		pr_err("capability: a valid lookup was refused");
		rc = RK_EIO;
		goto out;
	}
	/* The wrong type must be refused even with the right rights. */
	if (cap_lookup(cs, h, CAP_FILE, CAP_RIGHT_READ, &got) == RK_OK) {
		pr_err("capability: type confusion was allowed");
		rc = RK_EIO;
		goto out;
	}
	/* A right that was never granted must be refused. */
	if (cap_lookup(cs, h, CAP_MEMORY, CAP_RIGHT_EXEC, &got) == RK_OK) {
		pr_err("capability: an ungranted right was allowed");
		rc = RK_EIO;
		goto out;
	}

	/* Derivation may only weaken. */
	cap_handle_t weak = cap_derive(cs, h, CAP_RIGHT_READ | CAP_RIGHT_EXEC, 1, 30);
	if (weak == CAP_INVALID) {
		pr_err("capability: derive failed");
		rc = RK_EIO;
		goto out;
	}
	u32 rights = 0;
	cap_rights(cs, weak, &rights);
	if (rights & CAP_RIGHT_EXEC) {
		pr_err("capability: derive granted a right the parent lacked");
		rc = RK_EIO;
		goto out;
	}

	/* Revocation must invalidate every handle to the object at once. */
	cap_revoke_recursive(obj);
	if (cap_lookup(cs, h, CAP_MEMORY, CAP_RIGHT_READ, &got) == RK_OK) {
		pr_err("capability: a revoked object was still reachable");
		rc = RK_EIO;
		goto out;
	}
	pr_info("capability self-test passed: type, rights, derive, revoke");

out:
	cap_object_put(obj);
	capspace_destroy(cs);
	return rc;
}

static int test_she(void)
{
	static struct she_vm vm;
	extern void she_stdlib_bind(struct she_vm *vm);

	she_vm_init(&vm, NULL, 0);   /* deliberately no permissions */
	she_stdlib_bind(&vm);
	she_vm_set_limits(&vm, 200000, 1u << 20);

	struct she_value r;
	int rc;

	/* Arithmetic and precedence. */
	rc = she_eval(&vm, "let a = 2 + 3 * 4\nreturn a", "selftest", &r);
	if (rc != RK_OK || r.type != SHE_NUM || r.as.n != 14) {
		pr_err("she: 2 + 3 * 4 gave the wrong answer (%s)",
		       vm.error[0] ? vm.error : "no error reported");
		goto fail;
	}

	/* Control flow and loops. */
	rc = she_eval(&vm,
		"let total = 0\n"
		"for each n in 1 to 5\n"
		"  total = total + n\n"
		"end\n"
		"return total", "selftest", &r);
	if (rc != RK_OK || r.as.n != 15) {
		pr_err("she: for-each summed to %lld, expected 15 (%s)",
		       (long long)r.as.n, vm.error);
		goto fail;
	}

	/* Functions and the pipeline operator. */
	rc = she_eval(&vm,
		"fun double(x)\n"
		"  return x * 2\n"
		"end\n"
		"return [1, 2, 3] |> map(double) |> sum()", "selftest", &r);
	if (rc != RK_OK || r.as.n != 12) {
		pr_err("she: pipeline gave %lld, expected 12 (%s)",
		       (long long)r.as.n, vm.error);
		goto fail;
	}

	/* The capability sandbox: this VM was given nothing, so a permission
	 * error is the correct outcome and its absence is the bug. */
	rc = she_eval(&vm, "return now()", "selftest", &r);
	if (rc == RK_OK) {
		pr_err("she: a script with no permissions read the clock");
		goto fail;
	}
	if (!strstr(vm.error, "--allow-time")) {
		pr_err("she: the refusal did not name the flag that grants it: %s",
		       vm.error);
		goto fail;
	}
	vm.failed = false;
	vm.error[0] = '\0';

	/* Gas metering: an infinite loop must stop, not hang the kernel. */
	she_vm_set_limits(&vm, 20000, 1u << 20);
	rc = she_eval(&vm, "while true\n  let x = 1\nend", "selftest", &r);
	if (rc != RK_EAGAIN) {
		pr_err("she: an unbounded loop was not stopped by the gas budget");
		goto fail;
	}

	she_vm_free(&vm);
	pr_info("she self-test passed: arithmetic, loops, pipelines, sandbox, gas");
	return RK_OK;

fail:
	she_vm_free(&vm);
	return RK_EIO;
}


/* ------------------------------------------------------- demand paging */

/* Map a range, write a pattern across it, read it back.
 *
 * This exists because of a specific failure: on aarch64 a freshly mapped page
 * did not reliably hold what was written into it. A single store landed, a
 * sixteen-byte copy landed, and a copy spanning more than a page lost its
 * beginning while keeping its end. The ELF loader was where it was noticed,
 * but nothing about it was specific to loading a program - which is exactly
 * why the test belongs here, on every boot, rather than in the loader.
 *
 * The range deliberately spans several pages, so every page after the first
 * takes its own fault partway through a copy that is already in progress. */
static int test_demand_paging(void)
{
	if (!RK_HAVE_PAGING) {
		pr_info("memory self-test skipped: this port runs without paging");
		return RK_OK;
	}

	const size_t span = 4 * RK_PAGE_SIZE;
	struct address_space *as = as_kernel();
	vaddr_t va = 0;

	int rc = as_map_anon(as, 0, span, RK_PROT_READ | RK_PROT_WRITE,
	                     "selftest", &va);
	if (rc != RK_OK)
		return rc;

	u8 *pattern = kmalloc(span);
	if (!pattern) {
		as_unmap(as, va, span);
		return RK_ENOMEM;
	}
	for (size_t i = 0; i < span; i++)
		pattern[i] = (u8)(i * 31 + 7);

	rc = RK_OK;

	/* One large copy, crossing every page boundary in the range. */
	memcpy((void *)(uintptr_t)va, pattern, span);
	{
		size_t bad = 0, first = span, last = 0;
		for (size_t i = 0; i < span; i++) {
			if (((const volatile u8 *)(uintptr_t)va)[i] != pattern[i]) {
				if (!bad) first = i;
				last = i;
				bad++;
			}
		}
		if (bad) {
			pr_err("demand paging: %llu of %llu bytes wrong, first %llu "
			       "last %llu (page size %llu)",
			       (unsigned long long)bad, (unsigned long long)span,
			       (unsigned long long)first, (unsigned long long)last,
			       (unsigned long long)RK_PAGE_SIZE);
			/* Where in each page do they fall? */
			for (size_t pg = 0; pg < span / RK_PAGE_SIZE; pg++) {
				size_t lo = span, hi = 0, n = 0;
				for (size_t i = 0; i < RK_PAGE_SIZE; i++) {
					size_t k = pg * RK_PAGE_SIZE + i;
					if (((const volatile u8 *)(uintptr_t)va)[k] != pattern[k]) {
						if (!n) lo = i;
						hi = i;
						n++;
					}
				}
				pr_err("  page %llu: %llu bad, offsets %llu..%llu",
				       (unsigned long long)pg, (unsigned long long)n,
				       (unsigned long long)lo, (unsigned long long)hi);
			}
			rc = RK_EIO;
		}
	}

	/* And the same again one byte at a time, which takes the faults in a
	 * different order and would catch a copy that only works when it is the
	 * thing doing the faulting. */
	if (rc == RK_OK) {
		memset((void *)(uintptr_t)va, 0, span);
		for (size_t i = 0; i < span; i++)
			((volatile u8 *)(uintptr_t)va)[i] = pattern[i];
		for (size_t i = 0; i < span; i++) {
			if (((const volatile u8 *)(uintptr_t)va)[i] != pattern[i]) {
				pr_err("demand paging: byte-wise %llu differs",
				       (unsigned long long)i);
				rc = RK_EIO;
				break;
			}
		}
	}

	/* Changing the protection must not lose the contents. The loader depends
	 * on exactly this: it writes a segment through a writable mapping and
	 * then makes it read-only and executable. */
	if (rc == RK_OK) {
		rc = as_protect(as, va, span, RK_PROT_READ);
		if (rc == RK_OK) {
			for (size_t i = 0; i < span; i++) {
				if (((const volatile u8 *)(uintptr_t)va)[i] != pattern[i]) {
					pr_err("demand paging: byte %llu lost across protect",
					       (unsigned long long)i);
					rc = RK_EIO;
					break;
				}
			}
		}
	}

	kfree(pattern);
	as_unmap(as, va, span);

	if (rc == RK_OK)
		pr_info("memory self-test passed: demand paging, copies, protect");
	return rc;
}

int rk_selftest_all(void)
{
	struct {
		const char *name;
		int (*fn)(void);
	} tests[] = {
		{ "allocator",    test_allocator },
		{ "memory",       test_demand_paging },
		{ "crypto",       rk_crypto_selftest },
		{ "kaalka",       kaalka_selftest },
		{ "graph",        test_graph },
		{ "capabilities", test_capabilities },
		{ "ai",           rk_ai_selftest },
		{ "she",          test_she },
	};

	int failures = 0;
	for (size_t i = 0; i < ARRAY_SIZE(tests); i++) {
		int rc = tests[i].fn();
		if (rc != RK_OK) {
			pr_err("%s self-test FAILED: %s", tests[i].name, rk_strerror(rc));
			failures++;
		}
	}

	if (failures) {
		rk_console_set_color(RK_COLOR_LIGHT_RED, RK_COLOR_BLACK);
		pr_err("%d of %u self-tests failed; this kernel is not trustworthy",
		       failures, (unsigned)ARRAY_SIZE(tests));
		rk_console_set_color(RK_COLOR_LIGHT_GRAY, RK_COLOR_BLACK);
	} else {
		pr_info("all %u self-tests passed", (unsigned)ARRAY_SIZE(tests));
	}
	return failures ? RK_EIO : RK_OK;
}
