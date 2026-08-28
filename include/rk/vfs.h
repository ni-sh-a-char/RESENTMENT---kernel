/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - virtual filesystem.
 *
 * A conventional VFS with one unconventional rule: opening a path requires a
 * directory capability, and the resulting file handle is itself a capability
 * with rights no wider than the directory it came from. There is no global
 * root that every task can reach, so "confused deputy" attacks through paths
 * do not have a starting point.
 *
 * Filesystems provided in-tree:
 *   ramfs    read/write, the root
 *   devfs    devices, mounted at /dev
 *   graphfs  the runtime graph as a browsable tree, mounted at /graph
 *   initrd   a read-only USTAR image handed over by the bootloader
 */
#pragma once

#include <rk/types.h>
#include <rk/list.h>
#include <rk/sync.h>
#include <rk/cap.h>

#define RK_NAME_MAX 255
#define RK_PATH_MAX 1024

enum rk_ftype {
	RK_FT_NONE = 0,
	RK_FT_REG,
	RK_FT_DIR,
	RK_FT_CHR,
	RK_FT_BLK,
	RK_FT_LINK,
	RK_FT_FIFO,
	RK_FT_SOCK,
	RK_FT_GRAPH,     /* synthesised from the runtime graph */
};

#define RK_O_READ     (1u << 0)
#define RK_O_WRITE    (1u << 1)
#define RK_O_CREAT    (1u << 2)
#define RK_O_EXCL     (1u << 3)
#define RK_O_TRUNC    (1u << 4)
#define RK_O_APPEND   (1u << 5)
#define RK_O_DIR      (1u << 6)
#define RK_O_NONBLOCK (1u << 7)

struct rk_stat {
	u64 size;
	u64 blocks;
	u32 type;
	u32 mode;
	u32 nlink;
	u64 ino;
	s64 atime, mtime, ctime;
	u64 dev;
};

struct rk_dirent {
	char name[RK_NAME_MAX + 1];
	u64  ino;
	u32  type;
};

struct rk_vnode;
struct rk_mount;
struct rk_file;
struct vm_object;

struct rk_vnode_ops {
	int     (*lookup)(struct rk_vnode *dir, const char *name, struct rk_vnode **out);
	int     (*create)(struct rk_vnode *dir, const char *name, u32 type, u32 mode,
	                  struct rk_vnode **out);
	int     (*unlink)(struct rk_vnode *dir, const char *name);
	int     (*rename)(struct rk_vnode *od, const char *on, struct rk_vnode *nd, const char *nn);
	ssize_t (*read)(struct rk_vnode *v, void *buf, size_t n, u64 off);
	ssize_t (*write)(struct rk_vnode *v, const void *buf, size_t n, u64 off);
	int     (*truncate)(struct rk_vnode *v, u64 size);
	int     (*readdir)(struct rk_vnode *v, u64 index, struct rk_dirent *out);
	int     (*stat)(struct rk_vnode *v, struct rk_stat *out);
	int     (*ioctl)(struct rk_vnode *v, u32 cmd, void *arg);
	int     (*mmap)(struct rk_vnode *v, u64 off, size_t len, struct vm_object **out);
	int     (*poll)(struct rk_vnode *v, u32 events, u32 *revents);
	void    (*release)(struct rk_vnode *v);
};

struct rk_vnode {
	u64                        ino;
	u32                        type;
	u32                        mode;
	u64                        size;
	s64                        atime, mtime, ctime;
	const struct rk_vnode_ops *ops;
	struct rk_mount           *mount;
	void                      *priv;
	u32                        refcount;
	struct rk_mount           *mounted_here;  /* covers this node if non-NULL */
	struct mutex               lock;
	rk_id_t                    graph_node;
};

struct rk_fs_type {
	char             name[16];
	int            (*mount)(struct rk_mount *m, const char *source, const char *opts);
	void           (*umount)(struct rk_mount *m);
	struct list_head link;
};

struct rk_mount {
	char                  path[RK_PATH_MAX];
	struct rk_fs_type    *type;
	struct rk_vnode      *root;
	struct rk_vnode      *covered;
	void                 *priv;
	u32                   flags;
	struct list_head      link;
};

#define RK_MNT_RDONLY (1u << 0)
#define RK_MNT_NOEXEC (1u << 1)

struct rk_file {
	struct rk_vnode *vnode;
	u64              pos;
	u32              flags;
	u32              rights;
	u32              refcount;
};

/* --------------------------------------------------------------------- API */

void rk_vfs_init(void);
int  rk_vfs_register(struct rk_fs_type *fs);
int  rk_vfs_mount(const char *source, const char *target, const char *fstype,
                  u32 flags, const char *opts);
int  rk_vfs_umount(const char *target);

struct rk_vnode *rk_vnode_get(struct rk_vnode *v);
void rk_vnode_put(struct rk_vnode *v);

/* Resolve relative to a directory vnode. A NULL base means the global root and
 * is only legal for kernel callers; user tasks always pass a capability. */
int rk_vfs_resolve(struct rk_vnode *base, const char *path, struct rk_vnode **out);
int rk_vfs_open(struct rk_vnode *base, const char *path, u32 flags, u32 mode,
                struct rk_file **out);
void rk_file_get(struct rk_file *f);
void rk_file_put(struct rk_file *f);
ssize_t rk_file_read(struct rk_file *f, void *buf, size_t n);
ssize_t rk_file_write(struct rk_file *f, const void *buf, size_t n);
ssize_t rk_file_pread(struct rk_file *f, void *buf, size_t n, u64 off);
ssize_t rk_file_pwrite(struct rk_file *f, const void *buf, size_t n, u64 off);
s64  rk_file_seek(struct rk_file *f, s64 off, int whence);
int  rk_file_stat(struct rk_file *f, struct rk_stat *st);
int  rk_file_readdir(struct rk_file *f, struct rk_dirent *out);

int  rk_vfs_mkdir(struct rk_vnode *base, const char *path, u32 mode);
int  rk_vfs_unlink(struct rk_vnode *base, const char *path);
int  rk_vfs_stat(struct rk_vnode *base, const char *path, struct rk_stat *out);

#define RK_SEEK_SET 0
#define RK_SEEK_CUR 1
#define RK_SEEK_END 2

/* Convenience for kernel callers and for the shell. */
int    rk_vfs_read_file(const char *path, void **out, size_t *len);
int    rk_vfs_write_file(const char *path, const void *buf, size_t len);

/* In-tree filesystems. */
void rk_ramfs_init(void);
void rk_devfs_init(void);
void rk_graphfs_init(void);
int  rk_initrd_mount(paddr_t start, paddr_t end, const char *target);
int  rk_initrd_mount_mem(const void *addr, size_t len, const char *target);

struct vfs_stats {
	u64 opens, reads, writes, lookups, creates, unlinks;
	u64 vnodes_live, cache_hits, cache_misses;
};
void rk_vfs_stats(struct vfs_stats *out);
