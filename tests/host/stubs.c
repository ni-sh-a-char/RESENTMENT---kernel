/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - host stubs for subsystems the tests do not exercise.
 *
 * The SHE standard library reaches into the VFS, the AI registry and the
 * scheduler. None of those are under test here, so they answer honestly that
 * they are unavailable rather than pretending to work - a test that passes
 * against a lying stub is worse than no test.
 */
#define RK_HOSTED 1

#include <rk/types.h>
#include <rk/errno.h>
#include <rk/vfs.h>
#include <rk/ai.h>
#include <rk/sched.h>
#include <rk/cap.h>
#include <rk/mm.h>
#include <rk/boot.h>
#include <rk/time.h>
#include <rk/string.h>

/* The replay clock lives in kernel/core/time.c, which the host build replaces
 * wholesale. Replay is exercised on the machine, not here. */
void rk_time_enter_replay(u64 base_mono_ns, s64 base_unix_sec)
{ (void)base_mono_ns; (void)base_unix_sec; }
void rk_time_replay_advance(u64 mono_ns) { (void)mono_ns; }
void rk_time_leave_replay(void) { }
bool rk_time_replaying(void) { return false; }

/* mm_init lives in vmm.c, which needs a real MMU. The heap and the physical
 * allocator underneath it are real and are what the tests cover. */
void mm_init(struct boot_info *bi)
{
	(void)bi;
	kheap_init();
}

int rk_vfs_read_file(const char *path, void **out, size_t *len)
{
	(void)path; (void)out; (void)len;
	return RK_ENODEV;
}

int rk_vfs_write_file(const char *path, const void *buf, size_t len)
{
	(void)path; (void)buf; (void)len;
	return RK_ENODEV;
}

size_t rk_model_list(struct rk_model **out, size_t max)
{
	(void)out; (void)max;
	return 0;
}

void sched_stats(struct sched_stats *out)
{
	memset(out, 0, sizeof(*out));
}

const char *sched_class_name(enum sched_class c) { (void)c; return "n/a"; }

/* Capability lookups always fail on the host, which is the safe answer: a SHE
 * test that appears to acquire authority would be testing the wrong thing. */
int cap_lookup(struct capspace *cs, cap_handle_t h, enum cap_type type,
               u32 need, struct cap_object **out)
{
	(void)cs; (void)h; (void)type; (void)need; (void)out;
	return RK_EACCES;
}
