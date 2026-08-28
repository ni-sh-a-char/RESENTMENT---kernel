/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - error codes.
 *
 * Kernel functions return 0 on success and a negative RK_E* on failure.
 */
#pragma once

#define RK_OK            0
#define RK_EPERM        -1   /* operation not permitted */
#define RK_ENOENT       -2   /* no such object */
#define RK_EINTR        -3   /* interrupted */
#define RK_EIO          -4   /* I/O error */
#define RK_ENXIO        -5   /* no such device */
#define RK_E2BIG        -6   /* argument list too long */
#define RK_EBADF        -7   /* bad handle / capability slot */
#define RK_EAGAIN       -8   /* try again */
#define RK_ENOMEM       -9   /* out of memory */
#define RK_EACCES      -10   /* permission denied (capability lacks rights) */
#define RK_EFAULT      -11   /* bad address */
#define RK_EBUSY       -12   /* resource busy */
#define RK_EEXIST      -13   /* already exists */
#define RK_ENODEV      -14   /* no such device */
#define RK_ENOTDIR     -15   /* not a directory */
#define RK_EISDIR      -16   /* is a directory */
#define RK_EINVAL      -17   /* invalid argument */
#define RK_ENFILE      -18   /* table overflow */
#define RK_ENOSPC      -19   /* no space left */
#define RK_ERANGE      -20   /* out of range */
#define RK_ENOSYS      -21   /* not implemented */
#define RK_ENOTEMPTY   -22   /* directory not empty */
#define RK_ETIMEDOUT   -23   /* timed out */
#define RK_EOVERFLOW   -24   /* value too large */
#define RK_EEXPIRED    -25   /* capability seal expired (Kaalka temporal bound) */
#define RK_EREPLAY     -26   /* replay detected (Kaalka ledger) */
#define RK_ESEAL       -27   /* seal verification failed */
#define RK_EDETERM     -28   /* determinism violation during replay */
#define RK_ENOTSUP     -29   /* unsupported operation */
#define RK_EPIPE       -30   /* broken channel */
#define RK_ECANCELED   -31   /* canceled */
#define RK_EDEADLK     -32   /* deadlock would occur */

const char *rk_strerror(int err);
