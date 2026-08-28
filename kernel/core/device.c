/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - device and driver core.
 *
 * Binding is a match between what a driver declares and what a bus enumerated.
 * The match function is the whole design: a driver lists PCI ids, device-tree
 * compatible strings, ACPI HIDs or platform names, the core tries each against
 * every unbound device, and neither side knows about the other. Adding a bus
 * changes no driver; adding a driver changes no bus.
 */
#include <rk/device.h>
#include <rk/mm.h>
#include <rk/string.h>
#include <rk/log.h>
#include <rk/errno.h>
#include <rk/graph.h>

#undef RK_SUBSYS
#define RK_SUBSYS "device"

static LIST_HEAD(buses);
static LIST_HEAD(drivers);
static LIST_HEAD(devices);
static LIST_HEAD(blockdevs);
static LIST_HEAD(netdevs);
/* Statically initialised: architecture bring-up registers devices, and getting
 * that to happen strictly after rk_device_init is an ordering constraint worth
 * removing rather than documenting. */
static DEFINE_MUTEX(device_lock);
static rk_id_t next_device_id = 1;

static const char *const class_names[RK_CLASS_COUNT] = {
	"other", "console", "input", "block", "net", "gpu", "accel", "serial",
	"timer", "rtc", "storage", "usb", "audio", "sensor", "power", "random"
};

static const char *const bus_names[RK_BUS_COUNT] = {
	"platform", "pci", "usb", "virtio", "i2c", "spi", "mmio"
};

const char *rk_dev_class_name(enum rk_dev_class c)
{
	return (unsigned)c < RK_CLASS_COUNT ? class_names[c] : "?";
}

const char *rk_bus_kind_name(enum rk_bus_kind b)
{
	return (unsigned)b < RK_BUS_COUNT ? bus_names[b] : "?";
}

int rk_bus_register(struct rk_bus *b)
{
	if (!b)
		return RK_EINVAL;
	list_add_tail(&b->link, &buses);
	pr_info("bus %s registered", b->name);
	return RK_OK;
}

int rk_driver_register(struct rk_driver *d)
{
	if (!d || !d->probe)
		return RK_EINVAL;
	list_add_tail(&d->link, &drivers);
	pr_debug("driver %s registered with %llu match entries",
	         d->name, (unsigned long long)d->nmatch);
	/* A driver registered after enumeration should still find its devices. */
	rk_device_probe_all();
	return RK_OK;
}

struct rk_device *rk_device_create(const char *name, enum rk_bus_kind bus,
                                   enum rk_dev_class cls, struct rk_device *parent)
{
	struct rk_device *d = kzalloc(sizeof(*d));
	if (!d)
		return NULL;

	strlcpy(d->name, name ? name : "device", sizeof(d->name));
	d->id  = __atomic_add_fetch(&next_device_id, 1, __ATOMIC_SEQ_CST) - 1;
	d->bus = (u8)bus;
	d->cls = (u8)cls;
	d->parent = parent;
	d->present = true;
	list_init(&d->children);
	list_init(&d->sibling);
	list_init(&d->link);
	mutex_init(&d->lock, "device");
	return d;
}

int rk_device_add_resource(struct rk_device *d, u8 kind, u64 start, u64 len)
{
	if (!d || d->nres >= RK_DEV_MAX_RES)
		return RK_ENOSPC;
	d->res[d->nres].kind  = kind;
	d->res[d->nres].start = start;
	d->res[d->nres].len   = len;
	d->nres++;
	return RK_OK;
}

u64 rk_device_resource(struct rk_device *d, u8 kind, u32 index, u64 *len)
{
	if (!d)
		return 0;
	u32 seen = 0;
	for (u8 i = 0; i < d->nres; i++) {
		if (d->res[i].kind != kind)
			continue;
		if (seen++ == index) {
			if (len)
				*len = d->res[i].len;
			return d->res[i].start;
		}
	}
	return 0;
}

int rk_device_register(struct rk_device *d)
{
	if (!d)
		return RK_EINVAL;

	mutex_lock(&device_lock);
	list_add_tail(&d->link, &devices);
	if (d->parent)
		list_add_tail(&d->sibling, &d->parent->children);
	mutex_unlock(&device_lock);

	struct graph_node *parent = d->parent
	                          ? rk_graph_node_find(d->parent->graph_node) : NULL;
	struct graph_node *n = rk_graph_node_create(GNODE_DEVICE, d->name, parent, d);
	if (n) {
		rk_graph_set_str(n, "bus", rk_bus_kind_name((enum rk_bus_kind)d->bus));
		rk_graph_set_str(n, "class", rk_dev_class_name((enum rk_dev_class)d->cls));
		if (d->ident.vendor)
			rk_graph_set_u64(n, "vendor", d->ident.vendor);
		if (d->ident.device)
			rk_graph_set_u64(n, "device", d->ident.device);
		d->graph_node = n->id;
	}
	return RK_OK;
}

void rk_device_unregister(struct rk_device *d)
{
	if (!d)
		return;
	if (d->driver && d->driver->remove)
		d->driver->remove(d);

	mutex_lock(&device_lock);
	list_del(&d->link);
	list_del(&d->sibling);
	mutex_unlock(&device_lock);

	struct graph_node *n = rk_graph_node_find(d->graph_node);
	if (n)
		rk_graph_node_destroy(n);
	kfree(d);
}

struct rk_device *rk_device_find(const char *name)
{
	struct rk_device *d;
	list_for_each_entry(d, &devices, link)
		if (strcmp(d->name, name) == 0)
			return d;
	return NULL;
}

size_t rk_device_list(struct rk_device **out, size_t max)
{
	size_t n = 0;
	struct rk_device *d;
	list_for_each_entry(d, &devices, link) {
		if (n >= max)
			break;
		out[n++] = d;
	}
	return n;
}

/* Any one field matching is enough. A driver that lists both a PCI id and a
 * device-tree string works on a PC and on a board without knowing which it is
 * running on, which is the entire point of having one model. */
static bool id_matches(const struct rk_dev_id *want, const struct rk_dev_id *have,
                       const struct rk_device *d)
{
	if (want->bus && want->bus != d->bus)
		return false;

	if (want->vendor && want->device)
		if (want->vendor == have->vendor && want->device == have->device)
			return true;
	if (want->compatible && have->compatible)
		if (strcmp(want->compatible, have->compatible) == 0)
			return true;
	if (want->hid && have->hid)
		if (strcmp(want->hid, have->hid) == 0)
			return true;
	if (want->name)
		if (strcmp(want->name, d->name) == 0)
			return true;
	return false;
}

void rk_device_probe_all(void)
{
	struct rk_device *d;
	struct rk_driver *drv;

	list_for_each_entry(d, &devices, link) {
		if (d->bound || !d->present)
			continue;
		list_for_each_entry(drv, &drivers, link) {
			bool hit = false;
			for (size_t i = 0; i < drv->nmatch && !hit; i++)
				hit = id_matches(&drv->match[i], &d->ident, d);
			if (!hit)
				continue;

			int rc = drv->probe(d);
			if (rc == RK_OK) {
				d->driver = drv;
				d->bound = true;
				pr_info("%s bound to %s", d->name, drv->name);
				struct graph_node *n = rk_graph_node_find(d->graph_node);
				if (n)
					rk_graph_set_str(n, "driver", drv->name);
				break;
			}
			pr_warn("%s refused %s: %s", drv->name, d->name, rk_strerror(rc));
		}
	}
}

void rk_device_shutdown_all(void)
{
	struct rk_device *d;
	list_for_each_entry(d, &devices, link)
		if (d->bound && d->driver && d->driver->shutdown)
			d->driver->shutdown(d);
}

/* ------------------------------------------------------------ block / net */

int rk_blockdev_register(struct rk_blockdev *b)
{
	if (!b || !b->read)
		return RK_EINVAL;
	list_add_tail(&b->link, &blockdevs);
	pr_info("block device %s: %llu sectors of %u bytes%s",
	        b->dev ? b->dev->name : "?", (unsigned long long)b->nsectors,
	        b->sector_size, b->readonly ? ", read only" : "");
	return RK_OK;
}

struct rk_blockdev *rk_blockdev_find(const char *name)
{
	struct rk_blockdev *b;
	list_for_each_entry(b, &blockdevs, link)
		if (b->dev && strcmp(b->dev->name, name) == 0)
			return b;
	return NULL;
}

int rk_netdev_register(struct rk_netdev *n)
{
	if (!n || !n->transmit)
		return RK_EINVAL;
	list_add_tail(&n->link, &netdevs);
	pr_info("network device %s: %02x:%02x:%02x:%02x:%02x:%02x, mtu %u",
	        n->dev ? n->dev->name : "?",
	        n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5],
	        n->mtu);
	return RK_OK;
}

void rk_netdev_receive(struct rk_netdev *n, const void *frame, size_t len)
{
	if (!n)
		return;
	n->rx_packets++;
	n->rx_bytes += len;
	/* Without a protocol stack bound, a received frame is counted and
	 * dropped. Saying so in the counters is better than pretending. */
	n->rx_drops++;
	(void)frame;
}

struct rk_netdev *rk_netdev_find(const char *name)
{
	struct rk_netdev *n;
	list_for_each_entry(n, &netdevs, link)
		if (n->dev && strcmp(n->dev->name, name) == 0)
			return n;
	return NULL;
}

void rk_device_init(void)
{
	mutex_init(&device_lock, "device-core");
	pr_info("device model ready");
}
