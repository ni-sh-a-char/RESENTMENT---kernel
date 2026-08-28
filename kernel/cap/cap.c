/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - capabilities.
 *
 * Four checks happen on every lookup, and each one exists because of a
 * specific way capability systems fail:
 *
 *   type       so a file handle cannot be passed where an endpoint is expected
 *   rights     so a read handle cannot be used to write
 *   generation so a revoked object cannot be reached through a stale slot
 *   seal       so a capability cannot outlive the authority that granted it
 *
 * The seal is the Kaalka contribution and the one a conventional design does
 * not have. Revocation in most capability systems is a sweep somebody has to
 * remember to run. Here every capability has a deadline baked into it, so the
 * default behaviour of a forgotten capability is to stop working.
 */
#include <rk/cap.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/time.h>
#include <rk/graph.h>
#include <rk/printf.h>

#undef RK_SUBSYS
#define RK_SUBSYS "cap"

static struct kmem_cache *capspace_cache;
static struct kmem_cache *object_cache;
static rk_id_t next_object_id = 1;
static rk_id_t next_capspace_id = 1;
static struct cap_stats cstats;
static DEFINE_SPINLOCK(stats_lock);

static const char *const type_name[CAP_NTYPES] = {
	"null", "task", "thread", "memory", "addrspace", "endpoint", "channel",
	"notify", "file", "dir", "device", "irq", "ioport", "mmio", "timer",
	"graph", "model", "tensor", "accel", "entropy", "clock", "net",
	"console", "capspace"
};

const char *cap_type_name(enum cap_type t)
{
	return (unsigned)t < CAP_NTYPES ? type_name[t] : "?";
}

static const struct {
	const char *name;
	u32 bit;
} right_names[] = {
	{ "read", CAP_RIGHT_READ }, { "write", CAP_RIGHT_WRITE },
	{ "exec", CAP_RIGHT_EXEC }, { "map", CAP_RIGHT_MAP },
	{ "send", CAP_RIGHT_SEND }, { "recv", CAP_RIGHT_RECV },
	{ "grant", CAP_RIGHT_GRANT }, { "derive", CAP_RIGHT_DERIVE },
	{ "revoke", CAP_RIGHT_REVOKE }, { "create", CAP_RIGHT_CREATE },
	{ "delete", CAP_RIGHT_DELETE }, { "control", CAP_RIGHT_CONTROL },
	{ "inspect", CAP_RIGHT_INSPECT }, { "infer", CAP_RIGHT_INFER },
	{ "train", CAP_RIGHT_TRAIN }, { "snapshot", CAP_RIGHT_SNAPSHOT },
	{ "replay", CAP_RIGHT_REPLAY },
};

size_t cap_rights_string(char *buf, size_t n, u32 rights)
{
	size_t len = 0;
	for (size_t i = 0; i < ARRAY_SIZE(right_names); i++) {
		if (!(rights & right_names[i].bit))
			continue;
		len += (size_t)snprintf(buf + len, n > len ? n - len : 0, "%s%s",
		                        len ? "," : "", right_names[i].name);
	}
	if (!len && n)
		len += (size_t)snprintf(buf, n, "none");
	return len;
}

u32 cap_rights_parse(const char *s)
{
	u32 r = 0;
	while (*s) {
		while (*s == ',' || *s == ' ')
			s++;
		if (!*s)
			break;
		size_t len = 0;
		while (s[len] && s[len] != ',' && s[len] != ' ')
			len++;
		for (size_t i = 0; i < ARRAY_SIZE(right_names); i++)
			if (strlen(right_names[i].name) == len &&
			    strncmp(s, right_names[i].name, len) == 0)
				r |= right_names[i].bit;
		s += len;
	}
	return r;
}

/* ---------------------------------------------------------------- objects */

struct cap_object *cap_object_create(enum cap_type type, void *ptr,
                                     const char *label, void (*release)(void *))
{
	struct cap_object *o = kmem_cache_alloc(object_cache);
	if (!o)
		return NULL;
	memset(o, 0, sizeof(*o));

	o->type = type;
	o->id   = __atomic_add_fetch(&next_object_id, 1, __ATOMIC_SEQ_CST) - 1;
	o->ptr  = ptr;
	o->label = label;
	o->release = release;
	o->refcount = 1;
	o->generation = 1;
	list_init(&o->derived);
	list_init(&o->sibling);
	spin_lock_init(&o->lock, "capobj");

	unsigned long f = spin_lock_irqsave(&stats_lock);
	cstats.objects_live++;
	spin_unlock_irqrestore(&stats_lock, f);
	return o;
}

void cap_object_get(struct cap_object *o)
{
	if (!o)
		return;
	unsigned long f = spin_lock_irqsave(&o->lock);
	o->refcount++;
	spin_unlock_irqrestore(&o->lock, f);
}

void cap_object_put(struct cap_object *o)
{
	if (!o)
		return;
	unsigned long f = spin_lock_irqsave(&o->lock);
	bool last = (--o->refcount == 0);
	spin_unlock_irqrestore(&o->lock, f);
	if (!last)
		return;

	if (o->release)
		o->release(o->ptr);

	unsigned long sf = spin_lock_irqsave(&stats_lock);
	if (cstats.objects_live)
		cstats.objects_live--;
	spin_unlock_irqrestore(&stats_lock, sf);
	kmem_cache_free(object_cache, o);
}

/* ------------------------------------------------------------- capspaces */

struct capspace *capspace_create(void)
{
	struct capspace *cs = kmem_cache_alloc(capspace_cache);
	if (!cs)
		return NULL;
	memset(cs, 0, sizeof(*cs));

	cs->slots = kcalloc(CAPSPACE_INIT_SLOTS, sizeof(struct capability));
	if (!cs->slots) {
		kmem_cache_free(capspace_cache, cs);
		return NULL;
	}
	cs->nslots   = CAPSPACE_INIT_SLOTS;
	cs->refcount = 1;
	cs->id       = __atomic_add_fetch(&next_capspace_id, 1, __ATOMIC_SEQ_CST) - 1;
	spin_lock_init(&cs->lock, "capspace");
	kaalka_ledger_init(&cs->ledger);
	return cs;
}

void capspace_get(struct capspace *cs)
{
	if (!cs)
		return;
	unsigned long f = spin_lock_irqsave(&cs->lock);
	cs->refcount++;
	spin_unlock_irqrestore(&cs->lock, f);
}

void capspace_destroy(struct capspace *cs)
{
	if (!cs)
		return;
	unsigned long f = spin_lock_irqsave(&cs->lock);
	if (--cs->refcount > 0) {
		spin_unlock_irqrestore(&cs->lock, f);
		return;
	}
	for (u32 i = 0; i < cs->nslots; i++)
		if (cs->slots[i].valid && cs->slots[i].obj)
			cap_object_put(cs->slots[i].obj);
	spin_unlock_irqrestore(&cs->lock, f);

	kfree(cs->slots);
	kmem_cache_free(capspace_cache, cs);
}

static bool grow_locked(struct capspace *cs)
{
	if (cs->nslots >= CAPSPACE_MAX_SLOTS)
		return false;
	u32 want = cs->nslots * 2;
	struct capability *fresh = kcalloc(want, sizeof(struct capability));
	if (!fresh)
		return false;
	memcpy(fresh, cs->slots, cs->nslots * sizeof(struct capability));
	kfree(cs->slots);
	cs->slots = fresh;
	cs->nslots = want;
	return true;
}

/* The seal is over the fields that define the capability. Anything that could
 * escalate authority is inside it, so a task that can write its own capspace
 * memory still cannot mint a capability, only corrupt one into being refused. */
static void seal_capability(struct capability *c, u64 lifetime_sec)
{
	struct {
		u64 obj_id;
		u32 type;
		u32 rights;
		u32 generation;
		u64 badge;
	} body = {
		c->obj ? c->obj->id : 0,
		c->obj ? (u32)c->obj->type : 0,
		c->rights, c->generation, c->badge
	};
	kaalka_seal_make(&c->seal, body.obj_id, c->badge, &body, sizeof(body),
	                 lifetime_sec);
}

static int verify_capability(const struct capability *c)
{
	struct {
		u64 obj_id;
		u32 type;
		u32 rights;
		u32 generation;
		u64 badge;
	} body = {
		c->obj ? c->obj->id : 0,
		c->obj ? (u32)c->obj->type : 0,
		c->rights, c->generation, c->badge
	};
	return kaalka_seal_verify(&c->seal, body.obj_id, &body, sizeof(body));
}

cap_handle_t cap_install(struct capspace *cs, struct cap_object *obj,
                         u32 rights, u64 badge, u64 lifetime_sec)
{
	if (!cs || !obj)
		return CAP_INVALID;

	unsigned long f = spin_lock_irqsave(&cs->lock);

	u32 slot = 0;
	while (slot < cs->nslots && cs->slots[slot].valid)
		slot++;
	if (slot == cs->nslots && !grow_locked(cs)) {
		spin_unlock_irqrestore(&cs->lock, f);
		return CAP_INVALID;
	}

	struct capability *c = &cs->slots[slot];
	memset(c, 0, sizeof(*c));
	c->obj        = obj;
	c->rights     = rights;
	c->generation = obj->generation;
	c->badge      = badge;
	c->uses_left  = 0;
	c->valid      = true;
	seal_capability(c, lifetime_sec);
	cs->used++;
	spin_unlock_irqrestore(&cs->lock, f);

	cap_object_get(obj);

	unsigned long sf = spin_lock_irqsave(&stats_lock);
	cstats.caps_live++;
	cstats.installs++;
	spin_unlock_irqrestore(&stats_lock, sf);

	rk_graph_record(GEV_CAP_CHECK, obj->id, (u64)slot, rights, 1);
	return (cap_handle_t)slot;
}

int cap_lookup(struct capspace *cs, cap_handle_t h, enum cap_type type,
               u32 need_rights, struct cap_object **out)
{
	if (!cs || h < 0 || (u32)h >= cs->nslots)
		return RK_EBADF;

	unsigned long f = spin_lock_irqsave(&cs->lock);
	struct capability *c = &cs->slots[h];

	if (!c->valid || !c->obj) {
		spin_unlock_irqrestore(&cs->lock, f);
		return RK_EBADF;
	}
	if (type != CAP_NULL && c->obj->type != type) {
		spin_unlock_irqrestore(&cs->lock, f);
		goto deny;
	}
	if ((c->rights & need_rights) != need_rights) {
		spin_unlock_irqrestore(&cs->lock, f);
		goto deny;
	}
	/* Generation mismatch means the object was revoked out from under this
	 * handle. Fail as a bad handle rather than as a permission error: the
	 * capability is not weaker, it is gone. */
	if (c->generation != c->obj->generation) {
		spin_unlock_irqrestore(&cs->lock, f);
		unsigned long sf = spin_lock_irqsave(&stats_lock);
		cstats.generation_faults++;
		spin_unlock_irqrestore(&stats_lock, sf);
		return RK_EBADF;
	}

	int sr = verify_capability(c);
	if (sr != RK_OK) {
		if (sr == RK_EEXPIRED) {
			/* Expiry retires the slot: leaving a dead capability installed
			 * would let it be re-checked forever. */
			c->valid = false;
			struct cap_object *dead = c->obj;
			c->obj = NULL;
			cs->used--;
			spin_unlock_irqrestore(&cs->lock, f);
			cap_object_put(dead);

			unsigned long sf = spin_lock_irqsave(&stats_lock);
			cstats.expiries++;
			if (cstats.caps_live)
				cstats.caps_live--;
			spin_unlock_irqrestore(&stats_lock, sf);
			return RK_EEXPIRED;
		}
		spin_unlock_irqrestore(&cs->lock, f);
		goto deny;
	}

	if (c->uses_left) {
		c->uses_left--;
		if (!c->uses_left)
			c->valid = false;
	}
	struct cap_object *obj = c->obj;
	spin_unlock_irqrestore(&cs->lock, f);

	if (out)
		*out = obj;
	rk_graph_record(GEV_CAP_CHECK, obj->id, (u64)h, need_rights, 0);
	return RK_OK;

deny: {
	unsigned long sf = spin_lock_irqsave(&stats_lock);
	cstats.denials++;
	spin_unlock_irqrestore(&stats_lock, sf);
	return RK_EACCES;
	}
}

cap_handle_t cap_derive(struct capspace *cs, cap_handle_t parent,
                        u32 new_rights, u64 new_badge, u64 lifetime_sec)
{
	if (!cs || parent < 0 || (u32)parent >= cs->nslots)
		return CAP_INVALID;

	unsigned long f = spin_lock_irqsave(&cs->lock);
	struct capability *p = &cs->slots[parent];
	if (!p->valid || !p->obj || !(p->rights & CAP_RIGHT_DERIVE)) {
		spin_unlock_irqrestore(&cs->lock, f);
		return CAP_INVALID;
	}

	/* Monotonic weakening in both dimensions. A derived capability can never
	 * carry a right the parent lacked, and can never outlive it. */
	u32 rights = new_rights & p->rights;
	s64 parent_left = p->seal.not_after - rk_unix_time();
	if (parent_left < 0)
		parent_left = 0;
	u64 life = lifetime_sec;
	if (!life || (s64)life > parent_left)
		life = (u64)parent_left;

	struct cap_object *obj = p->obj;
	spin_unlock_irqrestore(&cs->lock, f);

	if (!life)
		return CAP_INVALID;

	cap_handle_t h = cap_install(cs, obj, rights, new_badge, life);
	if (h != CAP_INVALID) {
		unsigned long sf = spin_lock_irqsave(&stats_lock);
		cstats.derives++;
		spin_unlock_irqrestore(&stats_lock, sf);
	}
	return h;
}

cap_handle_t cap_grant(struct capspace *from, cap_handle_t h,
                       struct capspace *to, u32 rights_mask)
{
	struct cap_object *obj = NULL;
	if (cap_lookup(from, h, CAP_NULL, CAP_RIGHT_GRANT, &obj) != RK_OK)
		return CAP_INVALID;

	unsigned long f = spin_lock_irqsave(&from->lock);
	struct capability *c = &from->slots[h];
	u32 rights = c->rights & rights_mask;
	s64 left = c->seal.not_after - rk_unix_time();
	u64 badge = c->badge;
	spin_unlock_irqrestore(&from->lock, f);

	if (left <= 0)
		return CAP_INVALID;

	cap_handle_t nh = cap_install(to, obj, rights, badge, (u64)left);
	if (nh != CAP_INVALID) {
		unsigned long sf = spin_lock_irqsave(&stats_lock);
		cstats.grants++;
		spin_unlock_irqrestore(&stats_lock, sf);
	}
	return nh;
}

int cap_close(struct capspace *cs, cap_handle_t h)
{
	if (!cs || h < 0 || (u32)h >= cs->nslots)
		return RK_EBADF;

	unsigned long f = spin_lock_irqsave(&cs->lock);
	struct capability *c = &cs->slots[h];
	if (!c->valid) {
		spin_unlock_irqrestore(&cs->lock, f);
		return RK_EBADF;
	}
	struct cap_object *obj = c->obj;
	memset(c, 0, sizeof(*c));
	if (cs->used)
		cs->used--;
	spin_unlock_irqrestore(&cs->lock, f);

	cap_object_put(obj);
	unsigned long sf = spin_lock_irqsave(&stats_lock);
	if (cstats.caps_live)
		cstats.caps_live--;
	spin_unlock_irqrestore(&stats_lock, sf);
	return RK_OK;
}

/* Revocation by generation bump. Every capability anywhere in the system that
 * points at this object becomes stale at once, with no list to walk and no
 * chance of missing one. */
int cap_revoke_recursive(struct cap_object *obj)
{
	if (!obj)
		return RK_EINVAL;
	unsigned long f = spin_lock_irqsave(&obj->lock);
	obj->generation++;
	spin_unlock_irqrestore(&obj->lock, f);

	unsigned long sf = spin_lock_irqsave(&stats_lock);
	cstats.revokes++;
	spin_unlock_irqrestore(&stats_lock, sf);

	pr_debug("revoked %s object %llu, generation now %u",
	         cap_type_name(obj->type), (unsigned long long)obj->id, obj->generation);
	return RK_OK;
}

int cap_revoke(struct capspace *cs, cap_handle_t h)
{
	if (!cs || h < 0 || (u32)h >= cs->nslots)
		return RK_EBADF;
	unsigned long f = spin_lock_irqsave(&cs->lock);
	struct capability *c = &cs->slots[h];
	if (!c->valid || !c->obj || !(c->rights & CAP_RIGHT_REVOKE)) {
		spin_unlock_irqrestore(&cs->lock, f);
		return RK_EACCES;
	}
	struct cap_object *obj = c->obj;
	spin_unlock_irqrestore(&cs->lock, f);
	return cap_revoke_recursive(obj);
}

int cap_rights(struct capspace *cs, cap_handle_t h, u32 *out)
{
	if (!cs || h < 0 || (u32)h >= cs->nslots)
		return RK_EBADF;
	unsigned long f = spin_lock_irqsave(&cs->lock);
	int rc = cs->slots[h].valid ? RK_OK : RK_EBADF;
	if (rc == RK_OK && out)
		*out = cs->slots[h].rights;
	spin_unlock_irqrestore(&cs->lock, f);
	return rc;
}

int cap_badge(struct capspace *cs, cap_handle_t h, u64 *out)
{
	if (!cs || h < 0 || (u32)h >= cs->nslots)
		return RK_EBADF;
	unsigned long f = spin_lock_irqsave(&cs->lock);
	int rc = cs->slots[h].valid ? RK_OK : RK_EBADF;
	if (rc == RK_OK && out)
		*out = cs->slots[h].badge;
	spin_unlock_irqrestore(&cs->lock, f);
	return rc;
}

void cap_foreach(struct capspace *cs, cap_iter_fn fn, void *ctx)
{
	if (!cs || !fn)
		return;
	for (u32 i = 0; i < cs->nslots; i++) {
		if (!cs->slots[i].valid)
			continue;
		if (!fn((cap_handle_t)i, &cs->slots[i], ctx))
			return;
	}
}

void cap_stats(struct cap_stats *out)
{
	unsigned long f = spin_lock_irqsave(&stats_lock);
	*out = cstats;
	spin_unlock_irqrestore(&stats_lock, f);
}

size_t cap_dump(struct capspace *cs, char *buf, size_t cap)
{
	size_t n = 0;
	n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0,
	                      "%4s %-10s %-28s %8s %s\n",
	                      "slot", "type", "rights", "expires", "label");
	if (!cs)
		return n;

	s64 now = rk_unix_time();
	for (u32 i = 0; i < cs->nslots; i++) {
		struct capability *c = &cs->slots[i];
		if (!c->valid || !c->obj)
			continue;
		char rights[128];
		cap_rights_string(rights, sizeof(rights), c->rights);
		s64 left = c->seal.not_after - now;
		n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0,
		                      "%4u %-10s %-28s %6llds %s\n", i,
		                      cap_type_name(c->obj->type), rights,
		                      (long long)(left > 0 ? left : 0),
		                      c->obj->label ? c->obj->label : "-");
	}
	return n;
}

void cap_init(void)
{
	spin_lock_init(&stats_lock, "cap-stats");
	capspace_cache = kmem_cache_create("capspace", sizeof(struct capspace), 32);
	object_cache   = kmem_cache_create("cap_object", sizeof(struct cap_object), 32);
	pr_info("capability system ready: %u types, sealed with Kaalka", CAP_NTYPES);
}
