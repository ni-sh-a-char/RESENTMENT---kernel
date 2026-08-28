/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - the portable boot sequence.
 *
 * The architecture layer has already given us a console, traps and a boot
 * description by the time rk_main runs. Everything from here is portable, and
 * the order is a dependency order: each step needs the ones above it and
 * nothing below.
 */
#include <rk/boot.h>
#include <rk/arch.h>
#include <rk/console.h>
#include <rk/log.h>
#include <rk/mm.h>
#include <rk/sched.h>
#include <rk/time.h>
#include <rk/irq.h>
#include <rk/graph.h>
#include <rk/cap.h>
#include <rk/ipc.h>
#include <rk/vfs.h>
#include <rk/ai.h>
#include <rk/she.h>
#include <rk/device.h>
#include <rk/syscall.h>
#include <rk/crypto.h>
#include <rk/string.h>
#include <rk/errno.h>
#include <rk/panic.h>

#undef RK_SUBSYS
#define RK_SUBSYS "boot"

#ifndef RK_VERSION
#define RK_VERSION "0.2.0"
#endif
#ifndef RK_CODENAME
#define RK_CODENAME "kaalachakra"
#endif
#ifndef RK_GITREV
#define RK_GITREV "unknown"
#endif

static void print_banner(struct boot_info *bi)
{
	rk_console_set_color(RK_COLOR_LIGHT_CYAN, RK_COLOR_BLACK);
	rk_printf("\n");
	rk_printf("  ####  RESENTMENT %s \"%s\"  rev %s\n",
	          RK_VERSION, RK_CODENAME, RK_GITREV);
	rk_console_set_color(RK_COLOR_DARK_GRAY, RK_COLOR_BLACK);
	rk_printf("        a capability-secure, AI-native kernel\n");
	rk_printf("        %s on %s, firmware %s\n",
	          arch_name(), bi->platform, bi->firmware);
	if (bi->cmdline[0])
		rk_printf("        cmdline: %s\n", bi->cmdline);
	rk_printf("\n");
	rk_console_set_color(RK_COLOR_LIGHT_GRAY, RK_COLOR_BLACK);
}

/* Publish the machine into the runtime graph. From this point an agent can
 * read what the kernel decided about the hardware instead of inferring it. */
static void seed_graph(struct boot_info *bi)
{
	struct graph_node *machine =
		rk_graph_node_create(GNODE_MACHINE, bi->platform, rk_graph_root(), NULL);
	if (!machine)
		return;
	rk_graph_set_str(machine, "arch", arch_name());
	rk_graph_set_str(machine, "cpu", arch_cpu_model());
	rk_graph_set_str(machine, "firmware", bi->firmware);
	rk_graph_set_u64(machine, "cpus", arch_cpu_count());
	rk_graph_set_u64(machine, "features", arch_cpu_features());

	struct pmm_stats p;
	pmm_stats(&p);
	struct graph_node *mem =
		rk_graph_node_create(GNODE_MEMORY, "ram", machine, NULL);
	if (mem) {
		rk_graph_set_u64(mem, "total", p.total_pages << RK_PAGE_SHIFT);
		rk_graph_set_u64(mem, "usable", p.free_pages << RK_PAGE_SHIFT);
		rk_graph_set_u64(mem, "pagesize", RK_PAGE_SIZE);
	}

	for (u32 i = 0; i < arch_cpu_count() && i < 64; i++) {
		char name[16];
		snprintf(name, sizeof(name), "cpu%u", i);
		struct graph_node *c = rk_graph_node_create(GNODE_CPU, name, machine, NULL);
		if (c)
			rk_graph_set_u64(c, "hz", arch_cycles_per_sec());
	}
}

static void mount_filesystems(struct boot_info *bi)
{
	rk_ramfs_init();
	rk_devfs_init();
	rk_graphfs_init();

	int r = rk_vfs_mount("none", "/", "ramfs", 0, NULL);
	if (r != RK_OK)
		panic("cannot mount the root filesystem: %s", rk_strerror(r));

	rk_vfs_mkdir(NULL, "/dev", 0755);
	rk_vfs_mkdir(NULL, "/graph", 0555);
	rk_vfs_mkdir(NULL, "/tmp", 01777);
	rk_vfs_mkdir(NULL, "/etc", 0755);
	rk_vfs_mkdir(NULL, "/models", 0755);

	rk_vfs_mount("none", "/dev", "devfs", 0, NULL);
	rk_vfs_mount("none", "/graph", "graphfs", RK_MNT_RDONLY, NULL);

	if (bi->initrd_start && bi->initrd_end > bi->initrd_start) {
		rk_vfs_mkdir(NULL, "/boot", 0755);
		rk_initrd_mount(bi->initrd_start, bi->initrd_end, "/boot");
	}
}

/* The first real thread. Everything that needs to sleep, allocate freely or
 * wait on a device happens here rather than in the boot path. */
static void init_thread(void *arg)
{
	struct boot_info *bi = arg;

	pr_info("init thread running");

	mount_filesystems(bi);
	rk_ai_init();
	she_stdlib_init();

	rk_device_probe_all();

	/* Self-tests run at boot, not in a test harness somebody has to remember
	 * to invoke. A kernel whose crypto is wrong should refuse to look healthy. */
	extern int rk_selftest_all(void);
	rk_selftest_all();

	struct pmm_stats p;
	struct kheap_stats h;
	pmm_stats(&p);
	kheap_stats(&h);
	pr_info("boot complete in %llu ms, %pB free, %pB kernel heap live",
	        (unsigned long long)(rk_time_ns() / RK_NS_PER_MS),
	        RK_BYTES(p.free_pages << RK_PAGE_SHIFT),
	        RK_BYTES(h.bytes_live));

	const u8 *d = rk_graph_root_digest();
	if (d) {
		char hex[65];
		rk_hex_encode(hex, sizeof(hex), d, 32);
		pr_info("system state digest %s", hex);
	}

	resh_run();
}

void rk_main(struct boot_info *bi) __noreturn;

void rk_main(struct boot_info *bi)
{
	rk_log_init();
	print_banner(bi);

	/* 1. Physical memory. The architecture layer needs an allocator to finish
	 *    building page tables, so this comes before arch_init. */
	pmm_init(bi);

	/* 2. Traps, MMU hardening, interrupt controller, timers. */
	arch_init(bi);

	/* 3. The heap and address spaces. */
	mm_init(bi);

	/* 4. Time, which everything below timestamps against. */
	rk_time_init();

	/* 5. Entropy and the primitives that the seals depend on. */
	rk_crypto_init();

	/* 6. Kaalka, which needs entropy and the clock. */
	kaalka_init();

	/* 7. The runtime graph, which needs the heap and Kaalka for sealing. */
	rk_graph_init();
	rk_memfab_init(rk_cmdline_u64("memfab", 0));

	/* 8. Capabilities, which are Kaalka-sealed and graph-visible. */
	cap_init();

	/* 9. Scheduling, which needs capabilities to build the kernel task. */
	sched_init();

	/* 10. IPC and the VFS, which need the scheduler to block on. */
	rk_ipc_init();
	rk_vfs_init();
	rk_syscall_init();

	/* 11. The device model, before any architecture code registers a device.
	 *     arch_late_init brings up the keyboard and the serial line, and both
	 *     register devices, so the core they register into has to exist. */
	rk_device_init();

	/* 12. Architecture bring-up that needs threads: SMP, syscall entry. */
	arch_late_init();

	seed_graph(bi);

	struct thread *init = thread_create("init", init_thread, bi,
	                                    SCHED_INTERACTIVE, 8);
	if (!init)
		panic("cannot create the init thread");
	thread_start(init);

	sched_start();   /* becomes the idle thread and never returns */
}
