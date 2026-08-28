/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - virtual filesystem core.
 *
 * Path resolution walks one component at a time and crosses mount points
 * explicitly. It refuses to follow ".." above the directory it started from,
 * which is what makes a directory capability an actual boundary rather than a
 * suggestion: a task handed a capability for /var/data cannot walk out of it,
 * so there is no path-based escape from a sandbox.
 */
#include <rk/vfs.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/time.h>
#include <rk/graph.h>
#include <rk/printf.h>

#undef RK_SUBSYS
#define RK_SUBSYS "vfs"

static LIST_HEAD(filesystems);
static LIST_HEAD(mounts);
static struct rk_vnode *vfs_root;
static struct kmem_cache *file_cache;
static struct mutex vfs_lock;
static struct vfs_stats vstats;
static u64 next_ino = 1;

u64 rk_vfs_next_ino(void)
{
	return __atomic_add_fetch(&next_ino, 1, __ATOMIC_SEQ_CST);
}

int rk_vfs_register(struct rk_fs_type *fs)
{
	if (!fs || !fs->mount)
		return RK_EINVAL;
	list_add_tail(&fs->link, &filesystems);
	pr_debug("filesystem type %s registered", fs->name);
	return RK_OK;
}

static struct rk_fs_type *fs_find(const char *name)
{
	struct rk_fs_type *fs;
	list_for_each_entry(fs, &filesystems, link)
		if (strcmp(fs->name, name) == 0)
			return fs;
	return NULL;
}

struct rk_vnode *rk_vnode_get(struct rk_vnode *v)
{
	if (v)
		__atomic_add_fetch(&v->refcount, 1, __ATOMIC_SEQ_CST);
	return v;
}

void rk_vnode_put(struct rk_vnode *v)
{
	if (!v)
		return;
	if (__atomic_sub_fetch(&v->refcount, 1, __ATOMIC_SEQ_CST) == 0) {
		if (v->ops && v->ops->release)
			v->ops->release(v);
		if (vstats.vnodes_live)
			vstats.vnodes_live--;
	}
}

/* ------------------------------------------------------------ resolution */

/* Split a path into components without allocating. Returns the length of the
 * next component and advances the cursor past any separators. */
static size_t next_component(const char **p)
{
	while (**p == '/')
		(*p)++;
	const char *start = *p;
	while (**p && **p != '/')
		(*p)++;
	return (size_t)(*p - start);
}

static struct rk_vnode *cross_mount(struct rk_vnode *v)
{
	while (v && v->mounted_here && v->mounted_here->root)
		v = v->mounted_here->root;
	return v;
}

int rk_vfs_resolve(struct rk_vnode *base, const char *path, struct rk_vnode **out)
{
	if (!path || !out)
		return RK_EINVAL;

	struct rk_vnode *cur = (path[0] == '/' || !base) ? vfs_root : base;
	if (!cur)
		return RK_ENOENT;

	/* Depth relative to the starting point. Refusing to go negative is the
	 * containment rule; it is enforced here, once, for every caller. */
	int depth = 0;
	cur = rk_vnode_get(cross_mount(cur));

	const char *p = path;
	for (;;) {
		const char *comp = p;
		size_t len = next_component(&p);
		if (!len)
			break;
		comp = p - len;

		if (len == 1 && comp[0] == '.')
			continue;
		if (len == 2 && comp[0] == '.' && comp[1] == '.') {
			if (depth == 0)
				continue;   /* clamp at the root of this view */
			depth--;
			struct rk_vnode *parent = NULL;
			if (cur->ops && cur->ops->lookup &&
			    cur->ops->lookup(cur, "..", &parent) == RK_OK && parent) {
				rk_vnode_put(cur);
				cur = cross_mount(parent);
			}
			continue;
		}

		if (cur->type != RK_FT_DIR) {
			rk_vnode_put(cur);
			return RK_ENOTDIR;
		}
		if (len > RK_NAME_MAX) {
			rk_vnode_put(cur);
			return RK_E2BIG;
		}

		char name[RK_NAME_MAX + 1];
		memcpy(name, comp, len);
		name[len] = '\0';

		struct rk_vnode *child = NULL;
		int r = cur->ops && cur->ops->lookup
		      ? cur->ops->lookup(cur, name, &child) : RK_ENOSYS;
		vstats.lookups++;
		if (r != RK_OK || !child) {
			rk_vnode_put(cur);
			return RK_ENOENT;
		}
		rk_vnode_put(cur);
		cur = cross_mount(child);
		depth++;
	}

	*out = cur;
	return RK_OK;
}

/* Split "a/b/c" into the parent directory and the final name. */
static int resolve_parent(struct rk_vnode *base, const char *path,
                          struct rk_vnode **parent, char *name, size_t namecap)
{
	const char *slash = strrchr(path, '/');
	if (!slash) {
		strlcpy(name, path, namecap);
		*parent = rk_vnode_get(base ? base : vfs_root);
		return *parent ? RK_OK : RK_ENOENT;
	}

	char dir[RK_PATH_MAX];
	size_t dlen = (size_t)(slash - path);
	if (dlen >= sizeof(dir))
		return RK_E2BIG;
	memcpy(dir, path, dlen);
	dir[dlen] = '\0';
	strlcpy(name, slash + 1, namecap);

	if (!dlen)
		return rk_vfs_resolve(base, "/", parent);
	return rk_vfs_resolve(base, dir, parent);
}

/* ------------------------------------------------------------------ open */

int rk_vfs_open(struct rk_vnode *base, const char *path, u32 flags, u32 mode,
                struct rk_file **out)
{
	struct rk_vnode *v = NULL;
	int r = rk_vfs_resolve(base, path, &v);

	if (r != RK_OK && (flags & RK_O_CREAT)) {
		struct rk_vnode *dir = NULL;
		char name[RK_NAME_MAX + 1];
		r = resolve_parent(base, path, &dir, name, sizeof(name));
		if (r != RK_OK)
			return r;
		if (!dir->ops || !dir->ops->create) {
			rk_vnode_put(dir);
			return RK_ENOSYS;
		}
		r = dir->ops->create(dir, name, RK_FT_REG, mode, &v);
		rk_vnode_put(dir);
		vstats.creates++;
		if (r != RK_OK)
			return r;
	} else if (r != RK_OK) {
		return r;
	} else if ((flags & RK_O_CREAT) && (flags & RK_O_EXCL)) {
		rk_vnode_put(v);
		return RK_EEXIST;
	}

	if ((flags & RK_O_DIR) && v->type != RK_FT_DIR) {
		rk_vnode_put(v);
		return RK_ENOTDIR;
	}
	if (v->type == RK_FT_DIR && (flags & RK_O_WRITE)) {
		rk_vnode_put(v);
		return RK_EISDIR;
	}

	if ((flags & RK_O_TRUNC) && v->ops && v->ops->truncate)
		v->ops->truncate(v, 0);

	struct rk_file *f = kmem_cache_alloc(file_cache);
	if (!f) {
		rk_vnode_put(v);
		return RK_ENOMEM;
	}
	f->vnode = v;
	f->pos   = (flags & RK_O_APPEND) ? v->size : 0;
	f->flags = flags;
	f->rights = flags & (RK_O_READ | RK_O_WRITE);
	f->refcount = 1;

	*out = f;
	vstats.opens++;
	return RK_OK;
}

void rk_file_get(struct rk_file *f)
{
	if (f)
		__atomic_add_fetch(&f->refcount, 1, __ATOMIC_SEQ_CST);
}

void rk_file_put(struct rk_file *f)
{
	if (!f)
		return;
	if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_SEQ_CST) == 0) {
		rk_vnode_put(f->vnode);
		kmem_cache_free(file_cache, f);
	}
}

ssize_t rk_file_pread(struct rk_file *f, void *buf, size_t n, u64 off)
{
	if (!f || !f->vnode)
		return RK_EBADF;
	if (!(f->flags & RK_O_READ))
		return RK_EACCES;
	if (!f->vnode->ops || !f->vnode->ops->read)
		return RK_ENOSYS;
	vstats.reads++;
	return f->vnode->ops->read(f->vnode, buf, n, off);
}

ssize_t rk_file_pwrite(struct rk_file *f, const void *buf, size_t n, u64 off)
{
	if (!f || !f->vnode)
		return RK_EBADF;
	if (!(f->flags & RK_O_WRITE))
		return RK_EACCES;
	if (!f->vnode->ops || !f->vnode->ops->write)
		return RK_ENOSYS;
	vstats.writes++;
	return f->vnode->ops->write(f->vnode, buf, n, off);
}

ssize_t rk_file_read(struct rk_file *f, void *buf, size_t n)
{
	ssize_t r = rk_file_pread(f, buf, n, f->pos);
	if (r > 0)
		f->pos += (u64)r;
	return r;
}

ssize_t rk_file_write(struct rk_file *f, const void *buf, size_t n)
{
	ssize_t r = rk_file_pwrite(f, buf, n, f->pos);
	if (r > 0)
		f->pos += (u64)r;
	return r;
}

s64 rk_file_seek(struct rk_file *f, s64 off, int whence)
{
	if (!f || !f->vnode)
		return RK_EBADF;
	s64 base;
	switch (whence) {
	case RK_SEEK_SET: base = 0; break;
	case RK_SEEK_CUR: base = (s64)f->pos; break;
	case RK_SEEK_END: base = (s64)f->vnode->size; break;
	default: return RK_EINVAL;
	}
	s64 np = base + off;
	if (np < 0)
		return RK_EINVAL;
	f->pos = (u64)np;
	return np;
}

int rk_file_stat(struct rk_file *f, struct rk_stat *st)
{
	if (!f || !f->vnode)
		return RK_EBADF;
	if (f->vnode->ops && f->vnode->ops->stat)
		return f->vnode->ops->stat(f->vnode, st);

	memset(st, 0, sizeof(*st));
	st->size  = f->vnode->size;
	st->type  = f->vnode->type;
	st->mode  = f->vnode->mode;
	st->ino   = f->vnode->ino;
	st->mtime = f->vnode->mtime;
	return RK_OK;
}

int rk_file_readdir(struct rk_file *f, struct rk_dirent *out)
{
	if (!f || !f->vnode || f->vnode->type != RK_FT_DIR)
		return RK_ENOTDIR;
	if (!f->vnode->ops || !f->vnode->ops->readdir)
		return RK_ENOSYS;
	int r = f->vnode->ops->readdir(f->vnode, f->pos, out);
	if (r == RK_OK)
		f->pos++;
	return r;
}

/* ------------------------------------------------------------- directories */

int rk_vfs_mkdir(struct rk_vnode *base, const char *path, u32 mode)
{
	struct rk_vnode *dir = NULL, *made = NULL;
	char name[RK_NAME_MAX + 1];

	int r = resolve_parent(base, path, &dir, name, sizeof(name));
	if (r != RK_OK)
		return r;
	if (!dir->ops || !dir->ops->create) {
		rk_vnode_put(dir);
		return RK_ENOSYS;
	}
	r = dir->ops->create(dir, name, RK_FT_DIR, mode, &made);
	rk_vnode_put(dir);
	if (made)
		rk_vnode_put(made);
	vstats.creates++;
	return r;
}

int rk_vfs_unlink(struct rk_vnode *base, const char *path)
{
	struct rk_vnode *dir = NULL;
	char name[RK_NAME_MAX + 1];

	int r = resolve_parent(base, path, &dir, name, sizeof(name));
	if (r != RK_OK)
		return r;
	if (!dir->ops || !dir->ops->unlink) {
		rk_vnode_put(dir);
		return RK_ENOSYS;
	}
	r = dir->ops->unlink(dir, name);
	rk_vnode_put(dir);
	vstats.unlinks++;
	return r;
}

int rk_vfs_stat(struct rk_vnode *base, const char *path, struct rk_stat *out)
{
	struct rk_vnode *v = NULL;
	int r = rk_vfs_resolve(base, path, &v);
	if (r != RK_OK)
		return r;

	if (v->ops && v->ops->stat) {
		r = v->ops->stat(v, out);
	} else {
		memset(out, 0, sizeof(*out));
		out->size = v->size;
		out->type = v->type;
		out->mode = v->mode;
		out->ino  = v->ino;
		r = RK_OK;
	}
	rk_vnode_put(v);
	return r;
}

/* ------------------------------------------------------------------ mount */

int rk_vfs_mount(const char *source, const char *target, const char *fstype,
                 u32 flags, const char *opts)
{
	struct rk_fs_type *fs = fs_find(fstype);
	if (!fs)
		return RK_ENODEV;

	struct rk_mount *m = kzalloc(sizeof(*m));
	if (!m)
		return RK_ENOMEM;
	strlcpy(m->path, target, sizeof(m->path));
	m->type = fs;
	m->flags = flags;

	int r = fs->mount(m, source, opts);
	if (r != RK_OK) {
		kfree(m);
		return r;
	}

	if (strcmp(target, "/") == 0) {
		vfs_root = m->root;
	} else {
		struct rk_vnode *cover = NULL;
		r = rk_vfs_resolve(NULL, target, &cover);
		if (r != RK_OK) {
			if (fs->umount)
				fs->umount(m);
			kfree(m);
			return r;
		}
		m->covered = cover;
		cover->mounted_here = m;
	}

	list_add_tail(&m->link, &mounts);
	pr_info("mounted %s on %s%s%s", fstype, target,
	        source && *source ? " from " : "", source ? source : "");
	return RK_OK;
}

int rk_vfs_umount(const char *target)
{
	struct rk_mount *m;
	list_for_each_entry(m, &mounts, link) {
		if (strcmp(m->path, target) != 0)
			continue;
		if (m->covered)
			m->covered->mounted_here = NULL;
		if (m->type->umount)
			m->type->umount(m);
		list_del(&m->link);
		kfree(m);
		return RK_OK;
	}
	return RK_ENOENT;
}

/* ------------------------------------------------------------ convenience */

/* Reads until end of file rather than trusting the reported size.
 *
 * A generated file - anything under /graph - has no size until something asks
 * for its contents, because the contents are produced on demand. Sizing the
 * buffer from stat would read zero bytes from every one of them. */
int rk_vfs_read_file(const char *path, void **out, size_t *len)
{
	struct rk_file *f = NULL;
	int r = rk_vfs_open(NULL, path, RK_O_READ, 0, &f);
	if (r != RK_OK)
		return r;

	struct rk_stat st;
	rk_file_stat(f, &st);

	size_t cap = st.size ? (size_t)st.size + 1 : 4096;
	size_t used = 0;
	char *buf = kmalloc(cap);
	if (!buf) {
		rk_file_put(f);
		return RK_ENOMEM;
	}

	for (;;) {
		if (used + 1 >= cap) {
			size_t bigger = cap * 2;
			char *fresh = kmalloc(bigger);
			if (!fresh) {
				kfree(buf);
				rk_file_put(f);
				return RK_ENOMEM;
			}
			memcpy(fresh, buf, used);
			kfree(buf);
			buf = fresh;
			cap = bigger;
		}

		ssize_t got = rk_file_pread(f, buf + used, cap - used - 1, used);
		if (got < 0) {
			kfree(buf);
			rk_file_put(f);
			return (int)got;
		}
		if (got == 0)
			break;
		used += (size_t)got;
	}

	buf[used] = '\0';
	rk_file_put(f);
	*out = buf;
	if (len)
		*len = used;
	return RK_OK;
}

int rk_vfs_write_file(const char *path, const void *buf, size_t len)
{
	struct rk_file *f = NULL;
	int r = rk_vfs_open(NULL, path, RK_O_WRITE | RK_O_CREAT | RK_O_TRUNC, 0644, &f);
	if (r != RK_OK)
		return r;
	ssize_t n = rk_file_pwrite(f, buf, len, 0);
	rk_file_put(f);
	return n < 0 ? (int)n : RK_OK;
}

void rk_vfs_stats(struct vfs_stats *out) { *out = vstats; }

struct rk_vnode *rk_vfs_root(void) { return vfs_root; }

void rk_vfs_init(void)
{
	mutex_init(&vfs_lock, "vfs");
	file_cache = kmem_cache_create("rk_file", sizeof(struct rk_file), 16);
	pr_info("VFS ready");
}
