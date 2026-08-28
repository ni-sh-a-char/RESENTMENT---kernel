/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - initial ramdisk.
 *
 * USTAR, because tar is the one archive format that needs no decompressor, no
 * index and no allocation to read: every header is 512 bytes, self-describing,
 * and the data follows it. The whole parser is one loop, and the boot path
 * gets a real filesystem before any driver exists.
 *
 * Files are not copied. Each entry becomes a vnode pointing straight into the
 * archive the bootloader already placed in memory, so a 40 MiB initrd costs
 * 40 MiB, not 80.
 */
#include <rk/vfs.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/printf.h>
#include <rk/arch.h>
#include <rk/time.h>

#undef RK_SUBSYS
#define RK_SUBSYS "initrd"

u64 rk_vfs_next_ino(void);

struct ustar_header {
	char name[100];
	char mode[8];
	char uid[8];
	char gid[8];
	char size[12];
	char mtime[12];
	char checksum[8];
	char typeflag;
	char linkname[100];
	char magic[6];
	char version[2];
	char uname[32];
	char gname[32];
	char devmajor[8];
	char devminor[8];
	char prefix[155];
	char pad[12];
} __packed;

RK_STATIC_ASSERT(sizeof(struct ustar_header) == 512, "ustar header must be 512 bytes");

struct initrd_node {
	struct rk_vnode   vnode;
	struct list_head  children;
	struct list_head  sibling;
	struct initrd_node *parent;
	char              name[RK_NAME_MAX + 1];
	const u8         *data;
};

static const struct rk_vnode_ops initrd_ops;

static u64 parse_octal(const char *s, size_t n)
{
	u64 v = 0;
	for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++)
		v = v * 8 + (u64)(s[i] - '0');
	return v;
}

/* The header checksum is the sum of every byte with the checksum field read as
 * spaces. Verifying it catches a truncated or misplaced initrd immediately,
 * which is otherwise a very confusing boot failure. */
static bool checksum_ok(const struct ustar_header *h)
{
	const u8 *p = (const u8 *)h;
	u32 sum = 0;
	for (size_t i = 0; i < 512; i++)
		sum += (i >= 148 && i < 156) ? (u32)' ' : p[i];
	return sum == (u32)parse_octal(h->checksum, 8);
}

static struct initrd_node *node_of(struct rk_vnode *v)
{
	return container_of(v, struct initrd_node, vnode);
}

static struct initrd_node *make_node(const char *name, u32 type,
                                     struct initrd_node *parent)
{
	struct initrd_node *n = kzalloc(sizeof(*n));
	if (!n)
		return NULL;
	strlcpy(n->name, name, sizeof(n->name));
	list_init(&n->children);
	list_init(&n->sibling);
	n->parent = parent;
	n->vnode.ino  = rk_vfs_next_ino();
	n->vnode.type = type;
	n->vnode.mode = type == RK_FT_DIR ? 0555 : 0444;
	n->vnode.ops  = &initrd_ops;
	n->vnode.refcount = 1;
	mutex_init(&n->vnode.lock, "initrd-node");
	if (parent)
		list_add_tail(&n->sibling, &parent->children);
	return n;
}

static struct initrd_node *find_child(struct initrd_node *d, const char *name)
{
	struct initrd_node *c;
	list_for_each_entry(c, &d->children, sibling)
		if (strcmp(c->name, name) == 0)
			return c;
	return NULL;
}

/* Create every intermediate directory on the way to a path. Tar archives are
 * not required to list directories before the files in them. */
static struct initrd_node *mkpath(struct initrd_node *root, const char *path,
                                  char *leaf, size_t leafcap)
{
	struct initrd_node *cur = root;
	const char *p = path;

	for (;;) {
		while (*p == '/')
			p++;
		const char *start = p;
		while (*p && *p != '/')
			p++;
		size_t len = (size_t)(p - start);
		if (!len)
			break;

		char comp[RK_NAME_MAX + 1];
		if (len > RK_NAME_MAX)
			len = RK_NAME_MAX;
		memcpy(comp, start, len);
		comp[len] = '\0';

		/* tar writes "./etc/boot.she" when archiving a directory, and a
		 * literal "." component would become a directory of that name. */
		if (strcmp(comp, ".") == 0)
			continue;

		/* Trailing component: that is the leaf the caller will create. */
		const char *rest = p;
		while (*rest == '/')
			rest++;
		if (!*rest) {
			strlcpy(leaf, comp, leafcap);
			return cur;
		}

		struct initrd_node *next = find_child(cur, comp);
		if (!next)
			next = make_node(comp, RK_FT_DIR, cur);
		if (!next)
			return NULL;
		cur = next;
	}
	leaf[0] = '\0';
	return cur;
}

static int initrd_lookup(struct rk_vnode *dir, const char *name, struct rk_vnode **out)
{
	struct initrd_node *d = node_of(dir);
	if (strcmp(name, ".") == 0) {
		*out = rk_vnode_get(dir);
		return RK_OK;
	}
	if (strcmp(name, "..") == 0) {
		*out = rk_vnode_get(d->parent ? &d->parent->vnode : dir);
		return RK_OK;
	}
	struct initrd_node *c = find_child(d, name);
	if (!c)
		return RK_ENOENT;
	*out = rk_vnode_get(&c->vnode);
	return RK_OK;
}

static ssize_t initrd_read(struct rk_vnode *v, void *buf, size_t n, u64 off)
{
	struct initrd_node *f = node_of(v);
	if (!f->data || off >= v->size)
		return 0;
	size_t avail = (size_t)(v->size - off);
	size_t take = n < avail ? n : avail;
	memcpy(buf, f->data + off, take);
	return (ssize_t)take;
}

static int initrd_readdir(struct rk_vnode *v, u64 index, struct rk_dirent *out)
{
	struct initrd_node *d = node_of(v);
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
	struct initrd_node *c;
	list_for_each_entry(c, &d->children, sibling) {
		if (seen == index) {
			strlcpy(out->name, c->name, sizeof(out->name));
			out->ino  = c->vnode.ino;
			out->type = c->vnode.type;
			return RK_OK;
		}
		seen++;
	}
	return RK_ENOENT;
}

static int initrd_stat(struct rk_vnode *v, struct rk_stat *st)
{
	memset(st, 0, sizeof(*st));
	st->size  = v->size;
	st->type  = v->type;
	st->mode  = v->mode;
	st->ino   = v->ino;
	st->mtime = v->mtime;
	return RK_OK;
}

static const struct rk_vnode_ops initrd_ops = {
	.lookup  = initrd_lookup,
	.read    = initrd_read,
	.readdir = initrd_readdir,
	.stat    = initrd_stat,
};

static struct initrd_node *initrd_root;
static paddr_t initrd_phys_start, initrd_phys_end;

static int initrd_mount_fs(struct rk_mount *m, const char *source, const char *opts)
{
	(void)source; (void)opts;
	if (!initrd_root)
		return RK_ENODEV;
	m->root = &initrd_root->vnode;
	return RK_OK;
}

static struct rk_fs_type initrd_type = {
	.name  = "initrd",
	.mount = initrd_mount_fs,
};

int rk_initrd_mount(paddr_t start, paddr_t end, const char *target)
{
	if (!start || end <= start)
		return RK_ENODEV;

	initrd_phys_start = start;
	initrd_phys_end   = end;

	return rk_initrd_mount_mem((const void *)arch_phys_to_virt(start),
	                           (size_t)(end - start), target);
}

/* The archive is already mapped and its physical address is not interesting:
 * this is the path used by an image that carries its own ramdisk in
 * .rodata, on a machine whose firmware cannot pass a module. */
int rk_initrd_mount_mem(const void *addr, size_t len, const char *target)
{
	if (!addr || len < 512)
		return RK_ENODEV;

	const u8 *base = (const u8 *)addr;
	size_t total = len;

	initrd_root = make_node("/", RK_FT_DIR, NULL);
	if (!initrd_root)
		return RK_ENOMEM;

	size_t off = 0;
	u32 files = 0, dirs = 0;

	while (off + 512 <= total) {
		const struct ustar_header *h = (const struct ustar_header *)(base + off);

		/* Two consecutive zero blocks end the archive. */
		if (h->name[0] == '\0')
			break;
		if (memcmp(h->magic, "ustar", 5) != 0) {
			pr_warn("initrd: bad magic at offset %llu, stopping",
			        (unsigned long long)off);
			break;
		}
		if (!checksum_ok(h)) {
			pr_warn("initrd: header checksum mismatch at offset %llu",
			        (unsigned long long)off);
			break;
		}

		u64 size = parse_octal(h->size, 12);
		char full[RK_PATH_MAX];
		if (h->prefix[0])
			snprintf(full, sizeof(full), "%.155s/%.100s", h->prefix, h->name);
		else
			snprintf(full, sizeof(full), "%.100s", h->name);

		char leaf[RK_NAME_MAX + 1];
		struct initrd_node *dir = mkpath(initrd_root, full, leaf, sizeof(leaf));

		if (dir && leaf[0]) {
			if (h->typeflag == '5') {
				if (!find_child(dir, leaf)) {
					make_node(leaf, RK_FT_DIR, dir);
					dirs++;
				}
			} else if (h->typeflag == '0' || h->typeflag == '\0') {
				struct initrd_node *f = make_node(leaf, RK_FT_REG, dir);
				if (f) {
					f->data = base + off + 512;
					f->vnode.size = size;
					f->vnode.mtime = (s64)parse_octal(h->mtime, 12);
					f->vnode.mode = (u32)parse_octal(h->mode, 8);
					files++;
				}
			}
		}

		off += 512 + (size_t)ALIGN_UP(size, 512ull);
	}

	rk_vfs_register(&initrd_type);
	int r = rk_vfs_mount("initrd", target, "initrd", RK_MNT_RDONLY, NULL);
	if (r == RK_OK)
		pr_info("initrd: %u files, %u directories, %pB at %s",
		        files, dirs, RK_BYTES(total), target);
	return r;
}
