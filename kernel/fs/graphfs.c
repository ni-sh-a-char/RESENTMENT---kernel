/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - the runtime graph as a filesystem.
 *
 * Mounted at /graph. Reading a file here renders live kernel state, so an
 * agent or a script gets the whole machine through one uniform interface and
 * does not need a syscall per subsystem. Everything is generated on read, so
 * nothing here can go stale.
 *
 * The design rule: every generator produces either deterministic canonical
 * output (digest, canon, json) or human output (text). The deterministic ones
 * are what a diff or an attestation is taken over; the human ones are for
 * people, and are explicitly not stable.
 */
#include <rk/vfs.h>
#include <rk/graph.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/printf.h>
#include <rk/sched.h>
#include <rk/cap.h>
#include <rk/ipc.h>
#include <rk/time.h>
#include <rk/arch.h>
#include <rk/ai.h>
#include <rk/she.h>
#include <rk/crypto.h>

#undef RK_SUBSYS
#define RK_SUBSYS "graphfs"

u64 rk_vfs_next_ino(void);
size_t kheap_dump(char *buf, size_t cap);
size_t sched_dump(char *buf, size_t cap);
size_t rk_irq_dump(char *buf, size_t cap);
size_t cap_dump(struct capspace *cs, char *buf, size_t cap);

typedef size_t (*gen_fn)(char *buf, size_t cap);

struct genfile {
	const char *name;
	gen_fn      fn;
	const char *doc;
};

/* ------------------------------------------------------------ generators */

static size_t gen_digest(char *buf, size_t cap)
{
	const u8 *d = rk_graph_root_digest();
	if (!d)
		return 0;
	char hex[65];
	rk_hex_encode(hex, sizeof(hex), d, RK_GRAPH_DIGEST_SIZE);
	return (size_t)snprintf(buf, cap, "%s\n", hex);
}

static size_t gen_tree(char *buf, size_t cap)
{
	return rk_graph_export(rk_graph_root(), GRAPH_FMT_TEXT, -1, buf, cap);
}

static size_t gen_json(char *buf, size_t cap)
{
	return rk_graph_export(rk_graph_root(), GRAPH_FMT_JSON, -1, buf, cap);
}

static size_t gen_canon(char *buf, size_t cap)
{
	return rk_graph_export(rk_graph_root(), GRAPH_FMT_CANON, -1, buf, cap);
}

static size_t gen_dot(char *buf, size_t cap)
{
	return rk_graph_export(rk_graph_root(), GRAPH_FMT_DOT, -1, buf, cap);
}

static size_t gen_events(char *buf, size_t cap)
{
	struct graph_event ev[128];
	u64 head = rk_graph_event_seq();
	u64 cursor = head > 128 ? head - 128 : 0;
	size_t n = rk_graph_events_read(ev, ARRAY_SIZE(ev), &cursor);
	size_t len = 0;

	len += (size_t)snprintf(buf + len, cap > len ? cap - len : 0,
	                        "%8s %14s %3s %6s %-14s %s\n",
	                        "seq", "mono_ns", "cpu", "tid", "kind", "a b c");
	for (size_t i = 0; i < n; i++)
		len += (size_t)snprintf(buf + len, cap > len ? cap - len : 0,
		                        "%8llu %14llu %3u %6llu %-14s %llu %llu %llu\n",
		                        (unsigned long long)ev[i].seq,
		                        (unsigned long long)ev[i].mono_ns,
		                        (unsigned)ev[i].cpu,
		                        (unsigned long long)ev[i].tid,
		                        graph_event_kind_name((enum graph_event_kind)ev[i].kind),
		                        (unsigned long long)ev[i].a,
		                        (unsigned long long)ev[i].b,
		                        (unsigned long long)ev[i].c);
	return len;
}

static size_t gen_meminfo(char *buf, size_t cap)
{
	struct pmm_stats p;
	struct kheap_stats h;
	pmm_stats(&p);
	kheap_stats(&h);

	size_t n = 0;
	n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0,
		"physical total     %pB\n"
		"physical free      %pB\n"
		"physical used      %pB\n"
		"physical reserved  %pB\n"
		"allocations        %llu\n"
		"frees              %llu\n"
		"failures           %llu\n"
		"largest free block %pB\n"
		"heap live          %pB\n"
		"heap peak          %pB\n"
		"heap allocs        %llu\n"
		"heap frees         %llu\n",
		RK_BYTES(p.total_pages << RK_PAGE_SHIFT),
		RK_BYTES(p.free_pages << RK_PAGE_SHIFT),
		RK_BYTES(p.used_pages << RK_PAGE_SHIFT),
		RK_BYTES(p.reserved_pages << RK_PAGE_SHIFT),
		(unsigned long long)p.alloc_count,
		(unsigned long long)p.free_count,
		(unsigned long long)p.fail_count,
		RK_BYTES((u64)RK_PAGE_SIZE << p.largest_free_order),
		RK_BYTES(h.bytes_live), RK_BYTES(h.bytes_peak),
		(unsigned long long)h.allocs, (unsigned long long)h.frees);
	return n;
}

static size_t gen_slabinfo(char *buf, size_t cap) { return kheap_dump(buf, cap); }
static size_t gen_threads(char *buf, size_t cap)  { return sched_dump(buf, cap); }
static size_t gen_interrupts(char *buf, size_t cap) { return rk_irq_dump(buf, cap); }

static size_t gen_caps(char *buf, size_t cap)
{
	struct task *t = task_current();
	return cap_dump(t ? t->caps : NULL, buf, cap);
}

static size_t gen_sched(char *buf, size_t cap)
{
	struct sched_stats s;
	sched_stats(&s);
	return (size_t)snprintf(buf, cap,
		"switches        %llu\n"
		"preemptions     %llu\n"
		"migrations      %llu\n"
		"deadline misses %llu\n"
		"threads total   %llu\n"
		"threads live    %llu\n"
		"runnable rt     %llu\n"
		"runnable infer  %llu\n"
		"runnable inter  %llu\n"
		"runnable batch  %llu\n"
		"idle ns         %llu\n",
		(unsigned long long)s.switches, (unsigned long long)s.preemptions,
		(unsigned long long)s.migrations, (unsigned long long)s.deadline_misses,
		(unsigned long long)s.threads_total, (unsigned long long)s.threads_live,
		(unsigned long long)s.runnable[SCHED_REALTIME],
		(unsigned long long)s.runnable[SCHED_INFERENCE],
		(unsigned long long)s.runnable[SCHED_INTERACTIVE],
		(unsigned long long)s.runnable[SCHED_BATCH],
		(unsigned long long)s.idle_ns);
}

static size_t gen_kaalka(char *buf, size_t cap)
{
	struct kaalka_stats k;
	struct kaalka_time kt;
	struct kaalka_angles ang;
	kaalka_stats(&k);
	kaalka_time_now(&kt);
	kaalka_angles(&kt, &ang);

	/* Angles are printed in millidegrees: the fixed-point value is exact and
	 * a decimal point would imply a precision the display does not have. */
	return (size_t)snprintf(buf, cap,
		"epoch             %llu\n"
		"epoch seconds     %u\n"
		"clock             %02u:%02u:%02u\n"
		"angle hour-min    %lld millideg\n"
		"angle min-sec     %lld millideg\n"
		"angle hour-sec    %lld millideg\n"
		"seals made        %llu\n"
		"seals verified    %llu\n"
		"seals rejected    %llu\n"
		"seals expired     %llu\n"
		"envelopes sealed  %llu\n"
		"envelopes opened  %llu\n"
		"replays blocked   %llu\n"
		"rekeys            %llu\n",
		(unsigned long long)k.current_epoch, KAALKA_EPOCH_SECONDS,
		kt.hour, kt.min, kt.sec,
		(long long)((ang.hour_min * 1000) >> KQ_SHIFT),
		(long long)((ang.min_sec  * 1000) >> KQ_SHIFT),
		(long long)((ang.hour_sec * 1000) >> KQ_SHIFT),
		(unsigned long long)k.seals_made, (unsigned long long)k.seals_verified,
		(unsigned long long)k.seals_rejected, (unsigned long long)k.seals_expired,
		(unsigned long long)k.envelopes_sealed,
		(unsigned long long)k.envelopes_opened,
		(unsigned long long)k.replays_blocked, (unsigned long long)k.rekeys);
}

static size_t gen_ai(char *buf, size_t cap)
{
	struct rk_infer_stats s;
	u64 pages = 0, shared = 0, bytes = 0, evict = 0;
	rk_infer_stats(&s);
	rk_kv_stats(&pages, &shared, &bytes, &evict);

	size_t n = (size_t)snprintf(buf, cap,
		"inference submitted %llu\n"
		"inference completed %llu\n"
		"inference failed    %llu\n"
		"inference preempted %llu\n"
		"tokens generated    %llu\n"
		"deadline misses     %llu\n"
		"compute ns          %llu\n"
		"wait ns             %llu\n"
		"kv pages            %llu\n"
		"kv shared pages     %llu\n"
		"kv bytes            %pB\n"
		"kv evictions        %llu\n",
		(unsigned long long)s.submitted, (unsigned long long)s.completed,
		(unsigned long long)s.failed, (unsigned long long)s.preempted,
		(unsigned long long)s.tokens_generated,
		(unsigned long long)s.deadline_misses,
		(unsigned long long)s.total_compute_ns,
		(unsigned long long)s.total_wait_ns,
		(unsigned long long)pages, (unsigned long long)shared, RK_BYTES(bytes),
		(unsigned long long)evict);

	struct rk_accel *accels[8];
	size_t na = rk_accel_list(accels, ARRAY_SIZE(accels));
	n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "\n%-12s %-6s %10s %10s\n",
	                      "accelerator", "kind", "submitted", "completed");
	for (size_t i = 0; i < na; i++)
		n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "%-12s %-6u %10llu %10llu\n",
		                      accels[i]->name, (unsigned)accels[i]->kind,
		                      (unsigned long long)accels[i]->submitted,
		                      (unsigned long long)accels[i]->completed);
	return n;
}

static size_t gen_cpuinfo(char *buf, size_t cap)
{
	u64 f = arch_cpu_features();
	return (size_t)snprintf(buf, cap,
		"arch      %s\n"
		"model     %s\n"
		"cpus      %u\n"
		"cycles/s  %llu\n"
		"features  %s%s%s%s%s%s%s\n",
		arch_name(), arch_cpu_model(), arch_cpu_count(),
		(unsigned long long)arch_cycles_per_sec(),
		(f & RK_FEAT_SIMD)    ? "simd " : "",
		(f & RK_FEAT_SIMD256) ? "simd256 " : "",
		(f & RK_FEAT_SIMD512) ? "simd512 " : "",
		(f & RK_FEAT_AES)     ? "aes " : "",
		(f & RK_FEAT_SHA)     ? "sha " : "",
		(f & RK_FEAT_RDRAND)  ? "rdrand " : "",
		(f & RK_FEAT_INVARTSC)? "invtsc " : "");
}

static size_t gen_memfab(char *buf, size_t cap)
{
	u64 entries, bytes, hits, misses;
	rk_memfab_stats(&entries, &bytes, &hits, &misses);
	return (size_t)snprintf(buf, cap,
		"entries %llu\nbytes   %pB\nhits    %llu\nmisses  %llu\n",
		(unsigned long long)entries, RK_BYTES(bytes),
		(unsigned long long)hits, (unsigned long long)misses);
}

static size_t gen_ipc(char *buf, size_t cap)
{
	struct ipc_stats s;
	rk_ipc_stats(&s);
	return (size_t)snprintf(buf, cap,
		"calls %llu\nsends %llu\nrecvs %llu\nreplies %llu\n"
		"channel bytes %llu\nnotifications %llu\ntimeouts %llu\n"
		"seal failures %llu\nfastpath switches %llu\n",
		(unsigned long long)s.calls, (unsigned long long)s.sends,
		(unsigned long long)s.recvs, (unsigned long long)s.replies,
		(unsigned long long)s.channel_bytes,
		(unsigned long long)s.notifications, (unsigned long long)s.timeouts,
		(unsigned long long)s.seal_failures,
		(unsigned long long)s.fastpath_switches);
}

static size_t gen_stats(char *buf, size_t cap)
{
	struct graph_stats g;
	struct cap_stats c;
	rk_graph_stats(&g);
	cap_stats(&c);
	return (size_t)snprintf(buf, cap,
		"graph nodes          %llu\n"
		"graph edges          %llu\n"
		"graph events         %llu\n"
		"digest computes      %llu\n"
		"digest cache hits    %llu\n"
		"snapshots            %llu\n"
		"replays              %llu\n"
		"determinism faults   %llu\n"
		"cap objects          %llu\n"
		"caps live            %llu\n"
		"cap installs         %llu\n"
		"cap derives          %llu\n"
		"cap grants           %llu\n"
		"cap revokes          %llu\n"
		"cap denials          %llu\n"
		"cap expiries         %llu\n",
		(unsigned long long)g.nodes, (unsigned long long)g.edges,
		(unsigned long long)g.events, (unsigned long long)g.digest_computes,
		(unsigned long long)g.digest_cache_hits, (unsigned long long)g.snapshots,
		(unsigned long long)g.replays, (unsigned long long)g.determinism_faults,
		(unsigned long long)c.objects_live, (unsigned long long)c.caps_live,
		(unsigned long long)c.installs, (unsigned long long)c.derives,
		(unsigned long long)c.grants, (unsigned long long)c.revokes,
		(unsigned long long)c.denials, (unsigned long long)c.expiries);
}

static size_t gen_uptime(char *buf, size_t cap)
{
	u64 ns = rk_time_ns();
	char when[40];
	rk_format_time(when, sizeof(when), rk_unix_time());
	return (size_t)snprintf(buf, cap, "%llu.%09llu\n%s\n",
	                        (unsigned long long)(ns / RK_NS_PER_S),
	                        (unsigned long long)(ns % RK_NS_PER_S), when);
}

static size_t gen_readme(char *buf, size_t cap)
{
	return (size_t)snprintf(buf, cap,
		"/graph - the RESENTMENT runtime graph\n"
		"\n"
		"Everything here is generated when you read it, so it is never stale.\n"
		"\n"
		"Deterministic (safe to diff, hash or attest):\n"
		"  digest      SHA-256 Merkle root of the entire system state\n"
		"  canon       canonical per-node digest listing\n"
		"  json        the graph as deterministic JSON\n"
		"\n"
		"Human and agent readable:\n"
		"  tree        the graph as an indented tree\n"
		"  dot         graphviz source\n"
		"  events      the tail of the causal event log\n"
		"  threads     every thread, its class and its CPU time\n"
		"  meminfo     physical and heap memory\n"
		"  slabinfo    per-cache allocator detail\n"
		"  sched       scheduler counters, including deadline misses\n"
		"  interrupts  per-line counts and average service time\n"
		"  caps        the capabilities this task holds\n"
		"  ipc         endpoint, channel and notification counters\n"
		"  kaalka      the temporal keying state and the live clock angles\n"
		"  ai          inference queue, KV cache and accelerators\n"
		"  memfab      the federated memory fabric\n"
		"  cpuinfo     what this machine actually is\n"
		"  stats       graph and capability counters\n"
		"  uptime      monotonic and wall clock\n"
		"\n"
		"Two machines in the same state produce the same digest. That is the\n"
		"point: comparing systems is comparing one hash.\n");
}

static const struct genfile genfiles[] = {
	{ "README",     gen_readme,     "what this directory is" },
	{ "digest",     gen_digest,     "merkle root" },
	{ "tree",       gen_tree,       "indented tree" },
	{ "json",       gen_json,       "deterministic json" },
	{ "canon",      gen_canon,      "canonical digest listing" },
	{ "dot",        gen_dot,        "graphviz" },
	{ "events",     gen_events,     "causal log tail" },
	{ "threads",    gen_threads,    "thread table" },
	{ "meminfo",    gen_meminfo,    "memory" },
	{ "slabinfo",   gen_slabinfo,   "slab caches" },
	{ "sched",      gen_sched,      "scheduler" },
	{ "interrupts", gen_interrupts, "interrupts" },
	{ "caps",       gen_caps,       "capabilities" },
	{ "ipc",        gen_ipc,        "ipc counters" },
	{ "kaalka",     gen_kaalka,     "temporal keying" },
	{ "ai",         gen_ai,         "ai subsystem" },
	{ "memfab",     gen_memfab,     "memory fabric" },
	{ "cpuinfo",    gen_cpuinfo,    "cpu" },
	{ "stats",      gen_stats,      "counters" },
	{ "uptime",     gen_uptime,     "uptime" },
};

/* --------------------------------------------------------------- vnodes */

struct gnode {
	struct rk_vnode vnode;
	const struct genfile *gen;
};

static const struct rk_vnode_ops gfile_ops;
static const struct rk_vnode_ops gdir_ops;
static struct gnode dir_node;
static struct gnode file_nodes[ARRAY_SIZE(genfiles)];

/* Generated files have no fixed size, so a read renders into a scratch buffer
 * and slices out the requested window. Rendering the whole thing per read is
 * wasteful for a sequential scan but keeps the data exactly consistent, which
 * matters more for state a decision is made from. */
#define GEN_BUF 65536

static ssize_t gfile_read(struct rk_vnode *v, void *buf, size_t n, u64 off)
{
	struct gnode *g = container_of(v, struct gnode, vnode);
	if (!g->gen || !g->gen->fn)
		return 0;

	char *scratch = kmalloc(GEN_BUF);
	if (!scratch)
		return RK_ENOMEM;

	size_t total = g->gen->fn(scratch, GEN_BUF);
	if (total > GEN_BUF - 1)
		total = GEN_BUF - 1;
	v->size = total;

	ssize_t got = 0;
	if (off < total) {
		size_t avail = total - (size_t)off;
		got = (ssize_t)(n < avail ? n : avail);
		memcpy(buf, scratch + off, (size_t)got);
	}
	kfree(scratch);
	return got;
}

static int gfile_stat(struct rk_vnode *v, struct rk_stat *st)
{
	memset(st, 0, sizeof(*st));
	st->type = v->type;
	st->mode = 0444;
	st->ino  = v->ino;
	st->size = v->size;
	return RK_OK;
}

static int gdir_lookup(struct rk_vnode *dir, const char *name, struct rk_vnode **out)
{
	(void)dir;
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		*out = rk_vnode_get(&dir_node.vnode);
		return RK_OK;
	}
	for (size_t i = 0; i < ARRAY_SIZE(genfiles); i++) {
		if (strcmp(genfiles[i].name, name) == 0) {
			*out = rk_vnode_get(&file_nodes[i].vnode);
			return RK_OK;
		}
	}
	return RK_ENOENT;
}

static int gdir_readdir(struct rk_vnode *v, u64 index, struct rk_dirent *out)
{
	(void)v;
	if (index == 0) {
		strlcpy(out->name, ".", sizeof(out->name));
		out->type = RK_FT_DIR;
		return RK_OK;
	}
	if (index == 1) {
		strlcpy(out->name, "..", sizeof(out->name));
		out->type = RK_FT_DIR;
		return RK_OK;
	}
	size_t i = (size_t)(index - 2);
	if (i >= ARRAY_SIZE(genfiles))
		return RK_ENOENT;
	strlcpy(out->name, genfiles[i].name, sizeof(out->name));
	out->ino  = file_nodes[i].vnode.ino;
	out->type = RK_FT_GRAPH;
	return RK_OK;
}

static const struct rk_vnode_ops gfile_ops = {
	.read = gfile_read,
	.stat = gfile_stat,
};

static const struct rk_vnode_ops gdir_ops = {
	.lookup  = gdir_lookup,
	.readdir = gdir_readdir,
	.stat    = gfile_stat,
};

static int graphfs_mount(struct rk_mount *m, const char *source, const char *opts)
{
	(void)source; (void)opts;

	memset(&dir_node, 0, sizeof(dir_node));
	dir_node.vnode.ino  = rk_vfs_next_ino();
	dir_node.vnode.type = RK_FT_DIR;
	dir_node.vnode.mode = 0555;
	dir_node.vnode.ops  = &gdir_ops;
	dir_node.vnode.refcount = 1;
	dir_node.vnode.mount = m;
	mutex_init(&dir_node.vnode.lock, "graphfs-dir");

	for (size_t i = 0; i < ARRAY_SIZE(genfiles); i++) {
		memset(&file_nodes[i], 0, sizeof(file_nodes[i]));
		file_nodes[i].vnode.ino  = rk_vfs_next_ino();
		file_nodes[i].vnode.type = RK_FT_GRAPH;
		file_nodes[i].vnode.mode = 0444;
		file_nodes[i].vnode.ops  = &gfile_ops;
		file_nodes[i].vnode.refcount = 1;
		file_nodes[i].vnode.mount = m;
		file_nodes[i].gen = &genfiles[i];
		mutex_init(&file_nodes[i].vnode.lock, "graphfs-file");
	}

	m->root = &dir_node.vnode;
	return RK_OK;
}

static struct rk_fs_type graphfs_type = {
	.name  = "graphfs",
	.mount = graphfs_mount,
};

void rk_graphfs_init(void)
{
	rk_vfs_register(&graphfs_type);
}
