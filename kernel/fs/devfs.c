/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - device filesystem.
 *
 * Mounted at /dev. Registered character devices appear here automatically, so
 * a driver does not have to know anything about the VFS to become reachable.
 */
#include <rk/vfs.h>
#include <rk/device.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/crypto.h>
#include <rk/console.h>

#undef RK_SUBSYS
#define RK_SUBSYS "devfs"

u64 rk_vfs_next_ino(void);

#define DEVFS_MAX 64

struct devnode {
	struct rk_vnode vnode;
	char  name[32];
	const struct rk_chardev_ops *ops;
	struct rk_device *dev;
	bool  used;
};

static struct devnode devs[DEVFS_MAX];
static struct devnode dev_root;
static u32 ndevs;

/* ---------------------------------------------------------- builtin nodes */

static ssize_t null_read(struct rk_device *d, void *b, size_t n, u64 o)
{
	(void)d; (void)b; (void)n; (void)o;
	return 0;
}

static ssize_t null_write(struct rk_device *d, const void *b, size_t n, u64 o)
{
	(void)d; (void)b; (void)o;
	return (ssize_t)n;
}

static ssize_t zero_read(struct rk_device *d, void *b, size_t n, u64 o)
{
	(void)d; (void)o;
	memset(b, 0, n);
	return (ssize_t)n;
}

static ssize_t random_read(struct rk_device *d, void *b, size_t n, u64 o)
{
	(void)d; (void)o;
	rk_random_bytes(b, n);
	return (ssize_t)n;
}

static ssize_t console_write(struct rk_device *d, const void *b, size_t n, u64 o)
{
	(void)d; (void)o;
	rk_console_write(b, n);
	return (ssize_t)n;
}

static ssize_t console_read(struct rk_device *d, void *b, size_t n, u64 o)
{
	(void)d; (void)o;
	return (ssize_t)rk_console_readline(b, n);
}

static const struct rk_chardev_ops null_ops    = { .read = null_read, .write = null_write };
static const struct rk_chardev_ops zero_ops    = { .read = zero_read, .write = null_write };
static const struct rk_chardev_ops random_ops  = { .read = random_read, .write = null_write };
static const struct rk_chardev_ops console_ops = { .read = console_read, .write = console_write };

/* ---------------------------------------------------------------- vnodes */

static const struct rk_vnode_ops devfile_ops;
static const struct rk_vnode_ops devdir_ops;

static ssize_t devfile_read(struct rk_vnode *v, void *buf, size_t n, u64 off)
{
	struct devnode *d = container_of(v, struct devnode, vnode);
	if (!d->ops || !d->ops->read)
		return RK_ENOSYS;
	return d->ops->read(d->dev, buf, n, off);
}

static ssize_t devfile_write(struct rk_vnode *v, const void *buf, size_t n, u64 off)
{
	struct devnode *d = container_of(v, struct devnode, vnode);
	if (!d->ops || !d->ops->write)
		return RK_ENOSYS;
	return d->ops->write(d->dev, buf, n, off);
}

static int devfile_ioctl(struct rk_vnode *v, u32 cmd, void *arg)
{
	struct devnode *d = container_of(v, struct devnode, vnode);
	if (!d->ops || !d->ops->ioctl)
		return RK_ENOSYS;
	return d->ops->ioctl(d->dev, cmd, arg);
}

static int devfile_stat(struct rk_vnode *v, struct rk_stat *st)
{
	memset(st, 0, sizeof(*st));
	st->type = v->type;
	st->mode = v->mode;
	st->ino  = v->ino;
	return RK_OK;
}

static int devdir_lookup(struct rk_vnode *dir, const char *name, struct rk_vnode **out)
{
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		*out = rk_vnode_get(&dev_root.vnode);
		return RK_OK;
	}
	for (u32 i = 0; i < DEVFS_MAX; i++) {
		if (devs[i].used && strcmp(devs[i].name, name) == 0) {
			*out = rk_vnode_get(&devs[i].vnode);
			return RK_OK;
		}
	}
	(void)dir;
	return RK_ENOENT;
}

static int devdir_readdir(struct rk_vnode *v, u64 index, struct rk_dirent *out)
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
	u64 seen = 2;
	for (u32 i = 0; i < DEVFS_MAX; i++) {
		if (!devs[i].used)
			continue;
		if (seen == index) {
			strlcpy(out->name, devs[i].name, sizeof(out->name));
			out->ino  = devs[i].vnode.ino;
			out->type = devs[i].vnode.type;
			return RK_OK;
		}
		seen++;
	}
	return RK_ENOENT;
}

static const struct rk_vnode_ops devfile_ops = {
	.read  = devfile_read,
	.write = devfile_write,
	.ioctl = devfile_ioctl,
	.stat  = devfile_stat,
};

static const struct rk_vnode_ops devdir_ops = {
	.lookup  = devdir_lookup,
	.readdir = devdir_readdir,
	.stat    = devfile_stat,
};

static int devfs_add(const char *name, const struct rk_chardev_ops *ops,
                     struct rk_device *dev)
{
	for (u32 i = 0; i < DEVFS_MAX; i++) {
		if (devs[i].used)
			continue;
		memset(&devs[i], 0, sizeof(devs[i]));
		strlcpy(devs[i].name, name, sizeof(devs[i].name));
		devs[i].ops = ops;
		devs[i].dev = dev;
		devs[i].used = true;
		devs[i].vnode.ino  = rk_vfs_next_ino();
		devs[i].vnode.type = RK_FT_CHR;
		devs[i].vnode.mode = 0666;
		devs[i].vnode.ops  = &devfile_ops;
		devs[i].vnode.refcount = 1;
		mutex_init(&devs[i].vnode.lock, "devfs-node");
		ndevs++;
		return RK_OK;
	}
	return RK_ENOSPC;
}

int rk_chardev_register(struct rk_device *d, const struct rk_chardev_ops *ops)
{
	if (!d || !ops)
		return RK_EINVAL;
	return devfs_add(d->name, ops, d);
}

static int devfs_mount(struct rk_mount *m, const char *source, const char *opts)
{
	(void)source; (void)opts;

	memset(&dev_root, 0, sizeof(dev_root));
	dev_root.vnode.ino  = rk_vfs_next_ino();
	dev_root.vnode.type = RK_FT_DIR;
	dev_root.vnode.mode = 0555;
	dev_root.vnode.ops  = &devdir_ops;
	dev_root.vnode.refcount = 1;
	dev_root.vnode.mount = m;
	mutex_init(&dev_root.vnode.lock, "devfs-root");

	devfs_add("null",    &null_ops, NULL);
	devfs_add("zero",    &zero_ops, NULL);
	devfs_add("random",  &random_ops, NULL);
	devfs_add("urandom", &random_ops, NULL);
	devfs_add("console", &console_ops, NULL);

	m->root = &dev_root.vnode;
	return RK_OK;
}

static struct rk_fs_type devfs_type = {
	.name  = "devfs",
	.mount = devfs_mount,
};

void rk_devfs_init(void)
{
	rk_vfs_register(&devfs_type);
}
