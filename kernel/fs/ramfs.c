/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - in-memory filesystem.
 *
 * The root filesystem. Files grow by doubling, which keeps appends amortised
 * constant rather than quadratic - the shell writes its history one line at a
 * time, and a naive realloc-per-write makes that visibly slow.
 */
#include <rk/vfs.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/time.h>

#undef RK_SUBSYS
#define RK_SUBSYS "ramfs"

struct ramfs_node {
	struct rk_vnode   vnode;
	struct list_head  children;   /* directories */
	struct list_head  sibling;
	struct ramfs_node *parent;
	char              name[RK_NAME_MAX + 1];
	u8               *data;       /* regular files */
	size_t            capacity;
};

u64 rk_vfs_next_ino(void);

static const struct rk_vnode_ops ramfs_ops;

static struct ramfs_node *node_of(struct rk_vnode *v)
{
	return container_of(v, struct ramfs_node, vnode);
}

static struct ramfs_node *ramfs_new(const char *name, u32 type, u32 mode,
                                    struct ramfs_node *parent)
{
	struct ramfs_node *n = kzalloc(sizeof(*n));
	if (!n)
		return NULL;

	strlcpy(n->name, name, sizeof(n->name));
	list_init(&n->children);
	list_init(&n->sibling);
	n->parent = parent;

	n->vnode.ino   = rk_vfs_next_ino();
	n->vnode.type  = type;
	n->vnode.mode  = mode;
	n->vnode.ops   = &ramfs_ops;
	n->vnode.priv  = n;
	n->vnode.refcount = 1;
	n->vnode.ctime = n->vnode.mtime = n->vnode.atime = rk_unix_time();
	mutex_init(&n->vnode.lock, "ramfs-node");

	if (parent)
		list_add_tail(&n->sibling, &parent->children);
	return n;
}

static int ramfs_lookup(struct rk_vnode *dir, const char *name, struct rk_vnode **out)
{
	struct ramfs_node *d = node_of(dir);

	if (strcmp(name, ".") == 0) {
		*out = rk_vnode_get(dir);
		return RK_OK;
	}
	if (strcmp(name, "..") == 0) {
		*out = rk_vnode_get(d->parent ? &d->parent->vnode : dir);
		return RK_OK;
	}

	struct ramfs_node *c;
	list_for_each_entry(c, &d->children, sibling) {
		if (strcmp(c->name, name) == 0) {
			*out = rk_vnode_get(&c->vnode);
			return RK_OK;
		}
	}
	return RK_ENOENT;
}

static int ramfs_create(struct rk_vnode *dir, const char *name, u32 type, u32 mode,
                        struct rk_vnode **out)
{
	struct ramfs_node *d = node_of(dir);
	if (dir->type != RK_FT_DIR)
		return RK_ENOTDIR;

	struct rk_vnode *existing = NULL;
	if (ramfs_lookup(dir, name, &existing) == RK_OK) {
		rk_vnode_put(existing);
		return RK_EEXIST;
	}

	struct ramfs_node *n = ramfs_new(name, type, mode, d);
	if (!n)
		return RK_ENOMEM;
	dir->mtime = rk_unix_time();
	if (out)
		*out = rk_vnode_get(&n->vnode);
	return RK_OK;
}

static int ramfs_unlink(struct rk_vnode *dir, const char *name)
{
	struct ramfs_node *d = node_of(dir);
	struct ramfs_node *c, *tmp;

	list_for_each_entry_safe(c, tmp, &d->children, sibling) {
		if (strcmp(c->name, name) != 0)
			continue;
		if (c->vnode.type == RK_FT_DIR && !list_empty(&c->children))
			return RK_ENOTEMPTY;
		list_del(&c->sibling);
		/* The node may still be open. Dropping the directory reference is
		 * enough; the last close frees it. */
		rk_vnode_put(&c->vnode);
		dir->mtime = rk_unix_time();
		return RK_OK;
	}
	return RK_ENOENT;
}

static int ramfs_grow(struct ramfs_node *n, size_t want)
{
	if (want <= n->capacity)
		return RK_OK;

	size_t cap = n->capacity ? n->capacity : 64;
	while (cap < want)
		cap *= 2;

	u8 *fresh = kmalloc(cap);
	if (!fresh)
		return RK_ENOMEM;
	if (n->data) {
		memcpy(fresh, n->data, n->vnode.size);
		kfree(n->data);
	}
	memset(fresh + n->vnode.size, 0, cap - n->vnode.size);
	n->data = fresh;
	n->capacity = cap;
	return RK_OK;
}

static ssize_t ramfs_read(struct rk_vnode *v, void *buf, size_t n, u64 off)
{
	struct ramfs_node *f = node_of(v);
	if (off >= v->size)
		return 0;
	size_t avail = (size_t)(v->size - off);
	size_t take = n < avail ? n : avail;
	memcpy(buf, f->data + off, take);
	v->atime = rk_unix_time();
	return (ssize_t)take;
}

static ssize_t ramfs_write(struct rk_vnode *v, const void *buf, size_t n, u64 off)
{
	struct ramfs_node *f = node_of(v);
	if (off + n < off)
		return RK_EOVERFLOW;

	int r = ramfs_grow(f, (size_t)(off + n));
	if (r != RK_OK)
		return r;

	/* Writing past the end leaves a hole; zero it so a sparse write does not
	 * expose whatever the allocator handed us. */
	if (off > v->size)
		memset(f->data + v->size, 0, (size_t)(off - v->size));

	memcpy(f->data + off, buf, n);
	if (off + n > v->size)
		v->size = off + n;
	v->mtime = rk_unix_time();
	return (ssize_t)n;
}

static int ramfs_truncate(struct rk_vnode *v, u64 size)
{
	struct ramfs_node *f = node_of(v);
	if (size > v->size) {
		int r = ramfs_grow(f, (size_t)size);
		if (r != RK_OK)
			return r;
		memset(f->data + v->size, 0, (size_t)(size - v->size));
	}
	v->size = size;
	v->mtime = rk_unix_time();
	return RK_OK;
}

static int ramfs_readdir(struct rk_vnode *v, u64 index, struct rk_dirent *out)
{
	struct ramfs_node *d = node_of(v);
	u64 i = 0;

	if (index == 0) {
		strlcpy(out->name, ".", sizeof(out->name));
		out->ino = v->ino;
		out->type = RK_FT_DIR;
		return RK_OK;
	}
	if (index == 1) {
		strlcpy(out->name, "..", sizeof(out->name));
		out->ino = d->parent ? d->parent->vnode.ino : v->ino;
		out->type = RK_FT_DIR;
		return RK_OK;
	}

	struct ramfs_node *c;
	list_for_each_entry(c, &d->children, sibling) {
		if (i + 2 == index) {
			strlcpy(out->name, c->name, sizeof(out->name));
			out->ino = c->vnode.ino;
			out->type = c->vnode.type;
			return RK_OK;
		}
		i++;
	}
	return RK_ENOENT;
}

static int ramfs_stat(struct rk_vnode *v, struct rk_stat *st)
{
	memset(st, 0, sizeof(*st));
	st->size   = v->size;
	st->blocks = (v->size + 511) / 512;
	st->type   = v->type;
	st->mode   = v->mode;
	st->nlink  = 1;
	st->ino    = v->ino;
	st->atime  = v->atime;
	st->mtime  = v->mtime;
	st->ctime  = v->ctime;
	return RK_OK;
}

static void ramfs_release(struct rk_vnode *v)
{
	struct ramfs_node *n = node_of(v);
	kfree(n->data);
	kfree(n);
}

static const struct rk_vnode_ops ramfs_ops = {
	.lookup   = ramfs_lookup,
	.create   = ramfs_create,
	.unlink   = ramfs_unlink,
	.read     = ramfs_read,
	.write    = ramfs_write,
	.truncate = ramfs_truncate,
	.readdir  = ramfs_readdir,
	.stat     = ramfs_stat,
	.release  = ramfs_release,
};

static int ramfs_mount(struct rk_mount *m, const char *source, const char *opts)
{
	(void)source; (void)opts;
	struct ramfs_node *root = ramfs_new("/", RK_FT_DIR, 0755, NULL);
	if (!root)
		return RK_ENOMEM;
	root->vnode.mount = m;
	m->root = &root->vnode;
	m->priv = root;
	return RK_OK;
}

static void ramfs_umount(struct rk_mount *m)
{
	if (m->root)
		rk_vnode_put(m->root);
}

static struct rk_fs_type ramfs_type = {
	.name   = "ramfs",
	.mount  = ramfs_mount,
	.umount = ramfs_umount,
};

void rk_ramfs_init(void)
{
	rk_vfs_register(&ramfs_type);
}
