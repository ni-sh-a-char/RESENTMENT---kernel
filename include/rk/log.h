/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - structured logging.
 *
 * Every record is also offered to the runtime graph (kernel/graph) so the boot
 * narrative becomes part of the deterministic system IR, not just scrollback.
 */
#pragma once

#include <rk/types.h>
#include <rk/compiler.h>
/* Every user of pr_* also needs the format helpers, RK_BYTES in particular. */
#include <rk/printf.h>

enum rk_loglevel {
	RK_LOG_EMERG = 0,
	RK_LOG_ALERT,
	RK_LOG_CRIT,
	RK_LOG_ERR,
	RK_LOG_WARN,
	RK_LOG_NOTICE,
	RK_LOG_INFO,
	RK_LOG_DEBUG,
	RK_LOG_TRACE,
	RK_LOG_NLEVELS
};

void rk_log_init(void);
void rk_log_set_level(enum rk_loglevel lvl);
enum rk_loglevel rk_log_level(void);
const char *rk_loglevel_name(enum rk_loglevel lvl);

void rk_log(enum rk_loglevel lvl, const char *subsys, const char *fmt, ...) __printf(3, 4);

/* Ring-buffer access, used by the dmesg builtin and by graph snapshots.
 * cursor is an in/out record index; returns bytes written, 0 when drained. */
size_t rk_log_read(char *buf, size_t size, size_t *cursor);
u64    rk_log_records(void);

#ifndef RK_SUBSYS
#define RK_SUBSYS "kernel"
#endif

#define pr_emerg(...)  rk_log(RK_LOG_EMERG,  RK_SUBSYS, __VA_ARGS__)
#define pr_crit(...)   rk_log(RK_LOG_CRIT,   RK_SUBSYS, __VA_ARGS__)
#define pr_err(...)    rk_log(RK_LOG_ERR,    RK_SUBSYS, __VA_ARGS__)
#define pr_warn(...)   rk_log(RK_LOG_WARN,   RK_SUBSYS, __VA_ARGS__)
#define pr_notice(...) rk_log(RK_LOG_NOTICE, RK_SUBSYS, __VA_ARGS__)
#define pr_info(...)   rk_log(RK_LOG_INFO,   RK_SUBSYS, __VA_ARGS__)
#define pr_debug(...)  rk_log(RK_LOG_DEBUG,  RK_SUBSYS, __VA_ARGS__)
#define pr_trace(...)  rk_log(RK_LOG_TRACE,  RK_SUBSYS, __VA_ARGS__)
