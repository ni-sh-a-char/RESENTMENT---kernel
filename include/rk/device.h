/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - device and driver model.
 *
 * One model for every bus and every class, because "support any device" is a
 * claim about the driver model, not about how many drivers ship in the tree.
 * A driver declares what it matches on (a PCI id, a device-tree compatible
 * string, an ACPI HID, a USB class, or a platform name), the bus enumerates,
 * and the core binds them. Adding a bus does not change any driver; adding a
 * driver does not change any bus.
 *
 * Every device is also a node in the runtime graph, so the hardware topology
 * of a machine is queryable and diffable like everything else.
 */
#pragma once

#include <rk/types.h>
#include <rk/list.h>
#include <rk/sync.h>

#define RK_DEV_NAME_MAX 32

enum rk_dev_class {
	RK_CLASS_OTHER = 0,
	RK_CLASS_CONSOLE,
	RK_CLASS_INPUT,
	RK_CLASS_BLOCK,
	RK_CLASS_NET,
	RK_CLASS_GPU,
	RK_CLASS_ACCEL,
	RK_CLASS_SERIAL,
	RK_CLASS_TIMER,
	RK_CLASS_RTC,
	RK_CLASS_STORAGE_CTRL,
	RK_CLASS_USB,
	RK_CLASS_AUDIO,
	RK_CLASS_SENSOR,
	RK_CLASS_POWER,
	RK_CLASS_RANDOM,
	RK_CLASS_COUNT
};

enum rk_bus_kind {
	RK_BUS_PLATFORM = 0,   /* discovered from the device tree or hardcoded */
	RK_BUS_PCI,
	RK_BUS_USB,
	RK_BUS_VIRTIO,
	RK_BUS_I2C,
	RK_BUS_SPI,
	RK_BUS_MMIO,
	RK_BUS_COUNT
};

/* What a driver matches on. A driver may fill in several; any hit binds. */
struct rk_dev_id {
	u16         bus;
	u16         vendor, device;      /* PCI/USB */
	u16         subclass;
	const char *compatible;          /* device-tree */
	const char *hid;                 /* ACPI */
	const char *name;                /* platform */
};

struct rk_resource {
	u8      kind;        /* RK_RES_* */
	u64     start, len;
};

#define RK_RES_MEM  1
#define RK_RES_IO   2
#define RK_RES_IRQ  3
#define RK_RES_DMA  4

#define RK_DEV_MAX_RES 8

struct rk_driver;

struct rk_device {
	char                name[RK_DEV_NAME_MAX];
	rk_id_t             id;
	u8                  bus;
	u8                  cls;
	struct rk_dev_id    ident;
	struct rk_resource  res[RK_DEV_MAX_RES];
	u8                  nres;

	struct rk_device   *parent;
	struct list_head    children;
	struct list_head    sibling;
	struct list_head    link;

	struct rk_driver   *driver;
	void               *drvdata;
	void               *busdata;

	bool                present, bound, suspended;
	rk_id_t             graph_node;
	struct mutex        lock;
};

struct rk_driver {
	char                    name[RK_DEV_NAME_MAX];
	const struct rk_dev_id *match;
	size_t                  nmatch;
	int   (*probe)(struct rk_device *d);
	void  (*remove)(struct rk_device *d);
	int   (*suspend)(struct rk_device *d);
	int   (*resume)(struct rk_device *d);
	void  (*shutdown)(struct rk_device *d);
	struct list_head link;
};

struct rk_bus {
	char   name[RK_DEV_NAME_MAX];
	u8     kind;
	int  (*enumerate)(struct rk_bus *b);
	int  (*read_config)(struct rk_device *d, u32 off, u32 len, u64 *out);
	int  (*write_config)(struct rk_device *d, u32 off, u32 len, u64 val);
	struct list_head link;
};

void rk_device_init(void);
int  rk_bus_register(struct rk_bus *b);
int  rk_driver_register(struct rk_driver *d);
struct rk_device *rk_device_create(const char *name, enum rk_bus_kind bus,
                                   enum rk_dev_class cls, struct rk_device *parent);
int  rk_device_register(struct rk_device *d);
void rk_device_unregister(struct rk_device *d);
struct rk_device *rk_device_find(const char *name);
size_t rk_device_list(struct rk_device **out, size_t max);
int  rk_device_add_resource(struct rk_device *d, u8 kind, u64 start, u64 len);
u64  rk_device_resource(struct rk_device *d, u8 kind, u32 index, u64 *len);

/* Probe everything currently unbound. Safe to call again after a hotplug. */
void rk_device_probe_all(void);
void rk_device_shutdown_all(void);

/* ------------------------------------------------------------- char devices */

struct rk_chardev_ops {
	ssize_t (*read)(struct rk_device *d, void *buf, size_t n, u64 off);
	ssize_t (*write)(struct rk_device *d, const void *buf, size_t n, u64 off);
	int     (*ioctl)(struct rk_device *d, u32 cmd, void *arg);
	int     (*poll)(struct rk_device *d, u32 events, u32 *revents);
};

int rk_chardev_register(struct rk_device *d, const struct rk_chardev_ops *ops);

/* ------------------------------------------------------------ block devices */

struct rk_blockdev {
	struct rk_device *dev;
	u64               nsectors;
	u32               sector_size;
	bool              readonly;
	int  (*read)(struct rk_blockdev *b, u64 lba, u32 count, void *buf);
	int  (*write)(struct rk_blockdev *b, u64 lba, u32 count, const void *buf);
	int  (*flush)(struct rk_blockdev *b);
	void             *priv;
	struct list_head  link;
	u64               reads, writes, errors;
};

int rk_blockdev_register(struct rk_blockdev *b);
struct rk_blockdev *rk_blockdev_find(const char *name);

/* ------------------------------------------------------------- net devices */

struct rk_netdev {
	struct rk_device *dev;
	u8                mac[6];
	u32               mtu;
	bool              up, link_up;
	int  (*transmit)(struct rk_netdev *n, const void *frame, size_t len);
	void             *priv;
	struct list_head  link;
	u64               rx_packets, tx_packets, rx_bytes, tx_bytes, rx_drops, tx_drops;
};

int  rk_netdev_register(struct rk_netdev *n);
void rk_netdev_receive(struct rk_netdev *n, const void *frame, size_t len);
struct rk_netdev *rk_netdev_find(const char *name);

const char *rk_dev_class_name(enum rk_dev_class c);
const char *rk_bus_kind_name(enum rk_bus_kind b);
