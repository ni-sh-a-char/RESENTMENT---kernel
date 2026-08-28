/* SPDX-License-Identifier: Apache-2.0 */
#include <rk/errno.h>
#include <rk/types.h>

static const char *const names[] = {
	"success",
	"operation not permitted",
	"no such object",
	"interrupted",
	"I/O error",
	"no such device or address",
	"argument list too long",
	"bad capability handle",
	"try again",
	"out of memory",
	"permission denied",
	"bad address",
	"resource busy",
	"already exists",
	"no such device",
	"not a directory",
	"is a directory",
	"invalid argument",
	"table overflow",
	"no space left",
	"out of range",
	"not implemented",
	"directory not empty",
	"timed out",
	"value too large",
	"capability seal expired",
	"replay detected",
	"seal verification failed",
	"determinism violation",
	"unsupported operation",
	"broken channel",
	"canceled",
	"deadlock would occur",
};

const char *rk_strerror(int err)
{
	unsigned i = (unsigned)(err <= 0 ? -err : err);
	if (i >= sizeof(names) / sizeof(names[0]))
		return "unknown error";
	return names[i];
}
