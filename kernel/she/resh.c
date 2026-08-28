/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - resh, the system shell.
 *
 * There is no separate shell grammar. A command line is a SHE expression, so
 * `2 + 2` prints 4, `system()` returns a map you can index, and a pipeline
 * written at the prompt is the same pipeline you would write in a script.
 * The only additions are a handful of dot-commands for things that are about
 * the session rather than about the program: permissions, history, help.
 *
 * The shell holds one VM for the whole session, so `let x = 1` on one line is
 * visible on the next, which is what people expect from a REPL and what a
 * fresh-VM-per-line design cannot do.
 */
#include "she_internal.h"
#include <rk/console.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/vfs.h>
#include <rk/sched.h>
#include <rk/graph.h>
#include <rk/time.h>
#include <rk/arch.h>
#include <rk/printf.h>
#include <rk/boot.h>

#undef RK_SUBSYS
#define RK_SUBSYS "resh"

#ifndef RK_VERSION
#define RK_VERSION "0.0.0-dev"
#endif
#ifndef RK_CODENAME
#define RK_CODENAME "unnamed"
#endif

#define HISTORY_MAX 32
#define LINE_MAX    512

static char history[HISTORY_MAX][LINE_MAX];
static u32  history_count;

static void banner(void)
{
	rk_console_set_color(RK_COLOR_LIGHT_CYAN, RK_COLOR_BLACK);
	rk_printf("\n");
	rk_printf("  RESENTMENT %s (%s)  %s  %u cpu\n",
	          RK_VERSION, RK_CODENAME, arch_name(), arch_cpu_count());
	rk_console_set_color(RK_COLOR_DARK_GRAY, RK_COLOR_BLACK);
	rk_printf("  a capability-secure, AI-native kernel\n");
	rk_printf("  type .help for commands, or write SHE directly\n\n");
	rk_console_set_color(RK_COLOR_LIGHT_GRAY, RK_COLOR_BLACK);
}

static void show_help(void)
{
	rk_printf(
	"resh commands (everything else is SHE)\n"
	"\n"
	"  .help                 this text\n"
	"  .allow <what>         grant a permission: read write net run env time\n"
	"                        random infer graph device cap all\n"
	"  .deny <what>          take one back\n"
	"  .allowed              what this session may do\n"
	"  .graph [tree|json|dot|canon]   print the runtime graph\n"
	"  .digest               the merkle root of the whole system\n"
	"  .snapshot             take a Kaalka-sealed snapshot\n"
	"  .ps                   threads and their scheduling class\n"
	"  .mem                  memory\n"
	"  .caps                 capabilities this task holds\n"
	"  .irq                  interrupt counts\n"
	"  .kaalka               temporal keying state and live clock angles\n"
	"  .ai                   inference queue, KV cache, accelerators\n"
	"  .cat <path>           print a file\n"
	"  .ls [path]            list a directory\n"
	"  .run <path>           run a SHE script from a file\n"
	"  .dmesg                the kernel log\n"
	"  .history              recent lines\n"
	"  .selftest             run every subsystem self-test\n"
	"  .reboot / .poweroff\n"
	"\n"
	"SHE, briefly\n"
	"\n"
	"  let name = ask \"what is your name?\"\n"
	"  say \"hello, {name}\"\n"
	"  let n = [4, 8, 15, 16, 23, 42]\n"
	"  say n |> filter(fun(x) -> x %% 2 is 0) |> map(fun(x) -> x / 2) |> sum()\n"
	"  for each x in 1 to 5\n"
	"    say x * x\n"
	"  end\n"
	"\n"
	"A script starts with no permissions. When one is missing the error names\n"
	"the exact grant that would allow it.\n");
}

static void cat_file(const char *path)
{
	void *buf = NULL;
	size_t len = 0;
	int rc = rk_vfs_read_file(path, &buf, &len);
	if (rc != RK_OK) {
		rk_printf("cannot read %s: %s\n", path, rk_strerror(rc));
		return;
	}
	rk_console_write(buf, len);
	if (len && ((char *)buf)[len - 1] != '\n')
		rk_console_putc('\n');
	kfree(buf);
}

static void list_dir(const char *path)
{
	struct rk_file *f = NULL;
	int rc = rk_vfs_open(NULL, path, RK_O_READ | RK_O_DIR, 0, &f);
	if (rc != RK_OK) {
		rk_printf("cannot list %s: %s\n", path, rk_strerror(rc));
		return;
	}
	struct rk_dirent e;
	while (rk_file_readdir(f, &e) == RK_OK) {
		const char *kind = e.type == RK_FT_DIR ? "dir " :
		                   e.type == RK_FT_CHR ? "dev " :
		                   e.type == RK_FT_GRAPH ? "graph" : "file";
		rk_printf("  %-6s %s\n", kind, e.name);
	}
	rk_file_put(f);
}

static void dump_log(void)
{
	char buf[1024];
	size_t cursor = 0;
	size_t n;
	while ((n = rk_log_read(buf, sizeof(buf), &cursor)) > 0)
		rk_console_write(buf, n);
}

static void print_graph_file(const char *name)
{
	char path[64];
	snprintf(path, sizeof(path), "/graph/%s", name);
	cat_file(path);
}

/* Returns true when the line was a shell command and needs no evaluation. */
static bool dot_command(struct she_vm *vm, const char *line)
{
	if (line[0] != '.')
		return false;

	const char *cmd = line + 1;
	const char *arg = strchr(cmd, ' ');
	char verb[32];
	size_t vlen = arg ? (size_t)(arg - cmd) : strlen(cmd);
	if (vlen >= sizeof(verb))
		vlen = sizeof(verb) - 1;
	memcpy(verb, cmd, vlen);
	verb[vlen] = '\0';
	while (arg && *arg == ' ')
		arg++;

	if (strcmp(verb, "help") == 0)          show_help();
	else if (strcmp(verb, "graph") == 0)    print_graph_file(arg && *arg ? arg : "tree");
	else if (strcmp(verb, "digest") == 0)   print_graph_file("digest");
	else if (strcmp(verb, "ps") == 0)       print_graph_file("threads");
	else if (strcmp(verb, "mem") == 0)      print_graph_file("meminfo");
	else if (strcmp(verb, "slab") == 0)     print_graph_file("slabinfo");
	else if (strcmp(verb, "caps") == 0)     print_graph_file("caps");
	else if (strcmp(verb, "irq") == 0)      print_graph_file("interrupts");
	else if (strcmp(verb, "kaalka") == 0)   print_graph_file("kaalka");
	else if (strcmp(verb, "ai") == 0)       print_graph_file("ai");
	else if (strcmp(verb, "ipc") == 0)      print_graph_file("ipc");
	else if (strcmp(verb, "sched") == 0)    print_graph_file("sched");
	else if (strcmp(verb, "cpu") == 0)      print_graph_file("cpuinfo");
	else if (strcmp(verb, "events") == 0)   print_graph_file("events");
	else if (strcmp(verb, "stats") == 0)    print_graph_file("stats");
	else if (strcmp(verb, "dmesg") == 0)    dump_log();
	else if (strcmp(verb, "cat") == 0) {
		if (arg && *arg) cat_file(arg);
		else rk_printf("usage: .cat <path>\n");
	} else if (strcmp(verb, "ls") == 0) {
		list_dir(arg && *arg ? arg : "/");
	} else if (strcmp(verb, "snapshot") == 0) {
		char *buf = kmalloc(65536);
		if (buf) {
			struct graph_snapshot s;
			if (rk_graph_snapshot(&s, buf, 65536) == RK_OK) {
				char hex[65];
				rk_hex_encode(hex, sizeof(hex), s.root_digest, 32);
				rk_printf("snapshot %llu\n  root   %s\n  nodes  %u\n"
				          "  bytes  %u\n  sealed until %lld\n",
				          (unsigned long long)s.id, hex, s.node_count,
				          s.byte_len, (long long)s.seal.not_after);
			}
			kfree(buf);
		}
	} else if (strcmp(verb, "allow") == 0) {
		if (!arg || !*arg) {
			rk_printf("usage: .allow read|write|net|run|env|time|random|"
			          "infer|graph|device|cap|all\n");
		} else {
			u32 bit = she_allow_parse(arg);
			if (!bit) {
				rk_printf("no such permission: %s\n", arg);
			} else {
				vm->allow |= bit;
				rk_printf("granted %s\n", she_allow_name(bit));
			}
		}
	} else if (strcmp(verb, "deny") == 0) {
		u32 bit = arg ? she_allow_parse(arg) : 0;
		if (bit) {
			vm->allow &= ~bit;
			rk_printf("revoked %s\n", she_allow_name(bit));
		} else {
			rk_printf("no such permission\n");
		}
	} else if (strcmp(verb, "allowed") == 0) {
		rk_printf("this session may:\n");
		bool any = false;
		for (u32 b = 0; b < 11; b++) {
			u32 bit = 1u << b;
			if (vm->allow & bit) {
				rk_printf("  %s\n", she_allow_name(bit));
				any = true;
			}
		}
		if (!any)
			rk_printf("  nothing yet. use .allow <what>\n");
	} else if (strcmp(verb, "run") == 0) {
		if (!arg || !*arg) {
			rk_printf("usage: .run <path>\n");
		} else {
			void *src = NULL;
			size_t len = 0;
			if (rk_vfs_read_file(arg, &src, &len) != RK_OK) {
				rk_printf("cannot read %s\n", arg);
			} else {
				struct she_value r;
				int rc = she_eval(vm, src, arg, &r);
				if (rc != RK_OK && vm->error[0])
					rk_printf("%s\n", vm->error);
				kfree(src);
			}
		}
	} else if (strcmp(verb, "history") == 0) {
		for (u32 i = 0; i < history_count; i++)
			rk_printf("%3u  %s\n", i + 1, history[i]);
	} else if (strcmp(verb, "selftest") == 0) {
		extern int rk_selftest_all(void);
		rk_selftest_all();
	} else if (strcmp(verb, "reboot") == 0) {
		rk_printf("rebooting\n");
		arch_reboot();
	} else if (strcmp(verb, "poweroff") == 0) {
		rk_printf("powering off\n");
		arch_poweroff();
	} else if (strcmp(verb, "clear") == 0) {
		rk_console_clear();
	} else {
		rk_printf("unknown command .%s, try .help\n", verb);
	}
	return true;
}

int resh_exec_line(struct she_vm *vm, const char *line)
{
	while (*line == ' ' || *line == '\t')
		line++;
	if (!*line)
		return RK_OK;

	if (history_count < HISTORY_MAX)
		strlcpy(history[history_count++], line, LINE_MAX);
	else {
		for (u32 i = 1; i < HISTORY_MAX; i++)
			memcpy(history[i - 1], history[i], LINE_MAX);
		strlcpy(history[HISTORY_MAX - 1], line, LINE_MAX);
	}

	if (dot_command(vm, line))
		return RK_OK;

	/* A bare expression at the prompt prints its value; a statement does not.
	 * The test is whether `say (<line>)` compiles - compilation has no side
	 * effects, so trying it costs nothing and cannot run anything twice. That
	 * is what makes the prompt feel like a calculator without a special case
	 * for every expression form. */
	struct she_value result = SHE_NOTHING_V;
	struct she_obj *fn = NULL;
	char wrapped[LINE_MAX + 8];
	int rc;

	snprintf(wrapped, sizeof(wrapped), "say (%s)", line);
	if (she_compile(vm, wrapped, "prompt", &fn) == RK_OK) {
		vm->failed = false;
		vm->error[0] = '\0';
		rc = she_run(vm, fn, &result);
	} else {
		vm->failed = false;
		vm->error[0] = '\0';
		rc = she_eval(vm, line, "prompt", &result);
	}

	if (rc != RK_OK && vm->error[0]) {
		rk_console_set_color(RK_COLOR_LIGHT_RED, RK_COLOR_BLACK);
		rk_printf("%s\n", vm->error);
		rk_console_set_color(RK_COLOR_LIGHT_GRAY, RK_COLOR_BLACK);
		vm->failed = false;
		vm->error[0] = '\0';
		return rc;
	}
	if (rc == RK_EAGAIN) {
		rk_printf("(stopped: this used its whole instruction budget)\n");
		vm->gas = vm->gas_limit;
	}
	return rc;
}

/* Everything the shell needs that the plain SHE VM does not carry: the shell
 * starts with time and graph access, because a prompt you cannot ask the time
 * from is a poor prompt, and both are read-only. */
#define RESH_DEFAULT_ALLOW (SHE_ALLOW_TIME | SHE_ALLOW_GRAPH | SHE_ALLOW_RANDOM)

void resh_run(void)
{
	static struct she_vm vm;
	char line[LINE_MAX];

	she_vm_init(&vm, task_current() ? task_current()->caps : NULL,
	            RESH_DEFAULT_ALLOW);
	she_stdlib_bind(&vm);

	/* A boot script runs before the first prompt, which is how a machine is
	 * configured without recompiling the kernel. It is looked for in the
	 * initrd first and then on the root filesystem, so a machine can be
	 * reconfigured either by rebuilding the ramdisk or by editing a file. */
	static const char *const boot_scripts[] = {
		"/boot/etc/boot.she", "/etc/boot.she",
	};
	for (size_t i = 0; i < ARRAY_SIZE(boot_scripts); i++) {
		void *src = NULL;
		size_t len = 0;
		if (rk_vfs_read_file(boot_scripts[i], &src, &len) != RK_OK)
			continue;

		rk_printf("running %s\n", boot_scripts[i]);
		struct she_value r;
		u32 saved = vm.allow;
		/* Trusted by position: it is the file the kernel was told to run. */
		vm.allow = SHE_ALLOW_ALL;
		if (she_eval(&vm, src, boot_scripts[i], &r) != RK_OK && vm.error[0])
			rk_printf("%s\n", vm.error);
		vm.allow = saved;
		vm.failed = false;
		kfree(src);
		break;
	}

	/* `allow=read,write,graph` on the kernel command line grants the session
	 * those permissions. It is on the command line rather than in a config
	 * file on purpose: the grant is made by whoever booted the machine, which
	 * is the only party entitled to make it. */
	const char *allow = rk_cmdline_get("allow");
	while (allow && *allow && *allow != ' ') {
		char name[24];
		size_t n = 0;
		while (allow[n] && allow[n] != ',' && allow[n] != ' ' &&
		       n + 1 < sizeof(name)) {
			name[n] = allow[n];
			n++;
		}
		name[n] = '\0';

		u32 bit = she_allow_parse(name);
		if (bit) {
			vm.allow |= bit;
			rk_printf("granted %s\n", she_allow_name(bit));
		} else if (n) {
			rk_printf("no such permission on the command line: %s\n", name);
		}
		allow += n;
		if (*allow == ',')
			allow++;
		else
			break;
	}

	/* Headless operation: `run=<path>` executes a script before the prompt,
	 * and `once` powers the machine off afterwards. Together they make the
	 * kernel usable from a script or a CI job with no console at all. */
	const char *run = rk_cmdline_get("run");
	if (run) {
		char path[128];
		size_t i = 0;
		while (run[i] && run[i] != ' ' && i + 1 < sizeof(path)) {
			path[i] = run[i];
			i++;
		}
		path[i] = '\0';

		void *script = NULL;
		size_t slen = 0;
		if (rk_vfs_read_file(path, &script, &slen) == RK_OK) {
			rk_printf("running %s\n", path);
			struct she_value r;
			if (she_eval(&vm, script, path, &r) != RK_OK && vm.error[0])
				rk_printf("%s\n", vm.error);
			vm.failed = false;
			vm.gas = vm.gas_limit;
			kfree(script);
		} else {
			rk_printf("cannot read %s\n", path);
		}
	}
	if (rk_cmdline_flag("once")) {
		rk_printf("done; powering off\n");
		arch_poweroff();
	}

	banner();

	static char pending[LINE_MAX * 8];
	bool continuation = false;
	pending[0] = '\0';

	for (;;) {
		rk_console_set_color(RK_COLOR_LIGHT_GREEN, RK_COLOR_BLACK);
		rk_printf(continuation ? "        ... " : "resentment> ");
		rk_console_set_color(RK_COLOR_WHITE, RK_COLOR_BLACK);

		int n = rk_console_readline(line, sizeof(line));
		rk_console_set_color(RK_COLOR_LIGHT_GRAY, RK_COLOR_BLACK);
		if (n <= 0 && !pending[0])
			continue;

		/* A block typed a line at a time. Rather than counting keywords -
		 * which needs a second parser that can disagree with the first - the
		 * shell asks the compiler: if it failed because it ran out of input,
		 * the block is unfinished and the next line belongs to it. */
		if (pending[0]) {
			if (strlcat(pending, "\n", sizeof(pending)) >= sizeof(pending) ||
			    strlcat(pending, line, sizeof(pending)) >= sizeof(pending)) {
				rk_printf("that block is longer than the shell can hold\n");
				pending[0] = '\0';
				continue;
			}
		} else {
			strlcpy(pending, line, sizeof(pending));
		}

		vm.gas = vm.gas_limit;

		struct she_obj *probe = NULL;
		if (she_compile(&vm, pending, "prompt", &probe) != RK_OK && vm.incomplete) {
			vm.failed = false;
			vm.incomplete = false;
			vm.error[0] = '\0';
			continuation = true;
			continue;
		}
		vm.failed = false;
		vm.error[0] = '\0';

		resh_exec_line(&vm, pending);
		pending[0] = '\0';
		continuation = false;
	}
}
