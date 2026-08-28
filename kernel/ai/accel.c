/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - accelerator abstraction.
 *
 * One interface over CPU SIMD, a GPU queue, a phone NPU and a remote worker.
 * A driver fills in rk_accel_ops and registers; callers ask for the best
 * accelerator for a given operator and dtype and get either real hardware or
 * the CPU fallback, without knowing which.
 *
 * The CPU backend is always present and always last in preference order, so
 * the AI subsystem works on a machine with no accelerator at all - which is
 * the machine most people will boot this on.
 */
#include <rk/ai.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/arch.h>
#include <rk/time.h>
#include <rk/graph.h>

#undef RK_SUBSYS
#define RK_SUBSYS "accel"

static LIST_HEAD(accels);
static struct mutex accel_lock;
static rk_id_t next_accel_id = 1;

/* ------------------------------------------------------------ CPU backend */

static int cpu_submit(struct rk_accel *a, enum rk_op op, struct rk_tensor *dst,
                      struct rk_tensor *x, struct rk_tensor *y, struct rk_tensor *z,
                      const struct rk_op_args *args, u64 *fence)
{
	u64 t0 = rk_time_ns();
	int rc = rk_op_exec(op, dst, x, y, z, args);
	a->busy_ns += rk_time_ns() - t0;
	a->submitted++;
	if (rc == RK_OK)
		a->completed++;
	else
		a->errors++;
	/* The CPU backend is synchronous, so the fence is already signalled. */
	if (fence)
		*fence = a->completed;
	return rc;
}

static int cpu_wait(struct rk_accel *a, u64 fence, u64 timeout_ns)
{
	(void)a; (void)fence; (void)timeout_ns;
	return RK_OK;
}

static int cpu_alloc(struct rk_accel *a, size_t bytes, void **out, u64 *dev_addr)
{
	(void)a;
	void *p = kmalloc(bytes);
	if (!p)
		return RK_ENOMEM;
	*out = p;
	if (dev_addr)
		*dev_addr = (u64)(uintptr_t)p;
	return RK_OK;
}

static void cpu_free(struct rk_accel *a, void *p) { (void)a; kfree(p); }

static int cpu_upload(struct rk_accel *a, void *dev, const void *host, size_t n)
{
	(void)a;
	memcpy(dev, host, n);
	return RK_OK;
}

static int cpu_download(struct rk_accel *a, void *host, const void *dev, size_t n)
{
	(void)a;
	memcpy(host, dev, n);
	return RK_OK;
}

static bool cpu_supports(struct rk_accel *a, enum rk_op op, enum rk_dtype dt)
{
	(void)a; (void)dt;
	/* Everything rk_op_exec implements. Convolution and pooling are declared
	 * in the op list but not yet written, so say so rather than accepting the
	 * work and failing at submit time. */
	switch (op) {
	case RK_OP_CONV2D:
	case RK_OP_MAXPOOL2D:
	case RK_OP_CONCAT:
	case RK_OP_SLICE:
	case RK_OP_TOPK:
		return false;
	default:
		return op < RK_OP_COUNT;
	}
}

static const struct rk_accel_ops cpu_ops = {
	.submit   = cpu_submit,
	.wait     = cpu_wait,
	.alloc    = cpu_alloc,
	.free     = cpu_free,
	.upload   = cpu_upload,
	.download = cpu_download,
	.supports = cpu_supports,
};

static struct rk_accel cpu_accel;

/* ------------------------------------------------------------- registry */

int rk_accel_register(struct rk_accel *a)
{
	if (!a || !a->ops || !a->ops->submit)
		return RK_EINVAL;

	a->id = __atomic_add_fetch(&next_accel_id, 1, __ATOMIC_SEQ_CST) - 1;
	mutex_init(&a->lock, "accel");
	list_init(&a->link);

	mutex_lock(&accel_lock);
	list_add_tail(&a->link, &accels);
	mutex_unlock(&accel_lock);

	struct graph_node *n = rk_graph_node_create(GNODE_ACCEL, a->name, NULL, a);
	if (n) {
		rk_graph_set_u64(n, "kind", a->kind);
		rk_graph_set_u64(n, "mem", a->mem_total);
		rk_graph_set_u64(n, "peakops", a->peak_ops_per_sec);
		a->graph_node = n->id;
	}

	pr_info("accelerator %s registered: kind %u, %pB memory, %llu ops/s declared",
	        a->name, a->kind, RK_BYTES(a->mem_total),
	        (unsigned long long)a->peak_ops_per_sec);
	return RK_OK;
}

void rk_accel_unregister(struct rk_accel *a)
{
	if (!a)
		return;
	mutex_lock(&accel_lock);
	list_del(&a->link);
	mutex_unlock(&accel_lock);
}

struct rk_accel *rk_accel_find(const char *name)
{
	struct rk_accel *a;
	list_for_each_entry(a, &accels, link)
		if (strcmp(a->name, name) == 0)
			return a;
	return NULL;
}

/* Highest declared throughput that actually supports the operator. Declared
 * rather than measured because a device that has never run this op has no
 * measurement, and refusing to use it until it has is a chicken and egg. */
struct rk_accel *rk_accel_best(enum rk_op op, enum rk_dtype dt)
{
	struct rk_accel *best = NULL, *a;

	list_for_each_entry(a, &accels, link) {
		if (a->ops->supports && !a->ops->supports(a, op, dt))
			continue;
		if (a->queue_depth && a->submitted - a->completed >= a->queue_depth)
			continue;   /* saturated: fall through to something less busy */
		if (!best || a->peak_ops_per_sec > best->peak_ops_per_sec)
			best = a;
	}
	return best ? best : &cpu_accel;
}

size_t rk_accel_list(struct rk_accel **out, size_t max)
{
	size_t n = 0;
	struct rk_accel *a;
	list_for_each_entry(a, &accels, link) {
		if (n >= max)
			break;
		out[n++] = a;
	}
	return n;
}

void rk_accel_init(void)
{
	mutex_init(&accel_lock, "accel-registry");

	memset(&cpu_accel, 0, sizeof(cpu_accel));
	strlcpy(cpu_accel.name, "cpu", sizeof(cpu_accel.name));
	cpu_accel.kind = RK_ACCEL_CPU;
	cpu_accel.ops  = &cpu_ops;

	/* A rough but honest estimate: one fused multiply-add per lane per cycle.
	 * It exists to order the CPU below any real accelerator, not to be a
	 * benchmark. */
	u64 lanes = 1;
	u64 feats = arch_cpu_features();
	if (feats & RK_FEAT_SIMD512)      lanes = 16;
	else if (feats & RK_FEAT_SIMD256) lanes = 8;
	else if (feats & RK_FEAT_SIMD)    lanes = 4;
	cpu_accel.peak_ops_per_sec = arch_cycles_per_sec() * lanes * 2 * arch_cpu_count();

	struct pmm_stats ps;
	pmm_stats(&ps);
	cpu_accel.mem_total = ps.total_pages << RK_PAGE_SHIFT;
	cpu_accel.mem_free  = ps.free_pages << RK_PAGE_SHIFT;

	rk_accel_register(&cpu_accel);
}
