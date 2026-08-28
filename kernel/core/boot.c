/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - boot description helpers.
 */
#include <rk/boot.h>
#include <rk/string.h>
#include <rk/log.h>

/* The one description of the machine, filled in by whichever architecture
 * boot path ran, and read by everything above it. */
struct boot_info rk_boot_info;

static const char *const memtype[] = {
	"unknown", "usable", "reserved", "acpi-reclaim", "acpi-nvs", "bad",
	"bootloader", "kernel", "module", "framebuffer", "mmio"
};

const char *rk_mem_type_name(u32 type)
{
	return type < ARRAY_SIZE(memtype) ? memtype[type] : "unknown";
}

/* Command line parsing. Deliberately allocation free: this runs before the
 * heap exists, and a boot parameter that needs an allocator to be read is a
 * parameter that cannot configure the allocator. */
const char *rk_cmdline_get(const char *key)
{
	const char *p = rk_boot_info.cmdline;
	size_t klen = strlen(key);

	while (*p) {
		while (*p == ' ')
			p++;
		if (strncmp(p, key, klen) == 0 && p[klen] == '=')
			return p + klen + 1;
		while (*p && *p != ' ')
			p++;
	}
	return NULL;
}

bool rk_cmdline_flag(const char *key)
{
	const char *p = rk_boot_info.cmdline;
	size_t klen = strlen(key);

	while (*p) {
		while (*p == ' ')
			p++;
		if (strncmp(p, key, klen) == 0 && (p[klen] == ' ' || p[klen] == '\0'))
			return true;
		while (*p && *p != ' ')
			p++;
	}
	return false;
}

u64 rk_cmdline_u64(const char *key, u64 fallback)
{
	const char *v = rk_cmdline_get(key);
	if (!v)
		return fallback;

	const char *end = NULL;
	u64 n = rk_strtou64(v, &end, 0);
	/* Accept the usual size suffixes, because writing memfab=4M is what
	 * people actually type. */
	if (end) {
		if (*end == 'K' || *end == 'k') n <<= 10;
		else if (*end == 'M' || *end == 'm') n <<= 20;
		else if (*end == 'G' || *end == 'g') n <<= 30;
	}
	return n;
}
