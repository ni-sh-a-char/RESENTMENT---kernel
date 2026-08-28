/* SPDX-License-Identifier: Apache-2.0
 * RESENTMENT kernel - PS/2 keyboard.
 *
 * The scancode set 1 map, an interrupt handler and nothing else. A USB HID
 * stack is the right long-term answer, but every emulator and every PC
 * firmware still presents a PS/2 controller, so this is what makes the shell
 * usable on the machine most people will boot first.
 */
#include <arch/x86.h>
#include <rk/console.h>
#include <rk/irq.h>
#include <rk/device.h>
#include <rk/log.h>
#include <rk/errno.h>

#undef RK_SUBSYS
#define RK_SUBSYS "ps2"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64

static bool shift, ctrl, caps;

static const char map_lower[128] = {
	0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
	'\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
	0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
	0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
	'*', 0, ' ',
};

static const char map_upper[128] = {
	0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
	'\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
	0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
	0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
	'*', 0, ' ',
};

static enum rk_irq_result ps2_irq(u32 irq, void *dev)
{
	(void)irq; (void)dev;

	if (!(arch_inb(PS2_STATUS) & 0x01))
		return RK_IRQ_NONE;

	u8 sc = arch_inb(PS2_DATA);
	bool release = (sc & 0x80) != 0;
	u8 code = sc & 0x7F;

	switch (code) {
	case 0x2A: case 0x36:
		shift = !release;
		return RK_IRQ_HANDLED;
	case 0x1D:
		ctrl = !release;
		return RK_IRQ_HANDLED;
	case 0x3A:
		if (!release)
			caps = !caps;
		return RK_IRQ_HANDLED;
	}
	if (release || code >= sizeof(map_lower))
		return RK_IRQ_HANDLED;

	bool upper = shift ^ caps;
	char c = upper ? map_upper[code] : map_lower[code];
	if (!c)
		return RK_IRQ_HANDLED;

	/* Ctrl-C and friends arrive as control codes, which is what the line
	 * editor expects. */
	if (ctrl && c >= 'a' && c <= 'z')
		c = (char)(c - 'a' + 1);
	else if (ctrl && c >= 'A' && c <= 'Z')
		c = (char)(c - 'A' + 1);

	rk_console_push_input(c);
	return RK_IRQ_HANDLED;
}

void x86_ps2_init(void)
{
	/* Drain anything the firmware left in the buffer, or the first keypress
	 * will look like whatever the BIOS was doing. */
	for (int i = 0; i < 16 && (arch_inb(PS2_STATUS) & 0x01); i++)
		(void)arch_inb(PS2_DATA);

	struct rk_device *d = rk_device_create("kbd0", RK_BUS_PLATFORM, RK_CLASS_INPUT, NULL);
	if (d) {
		d->ident.name = "i8042";
		rk_device_add_resource(d, RK_RES_IO, PS2_DATA, 8);
		rk_device_add_resource(d, RK_RES_IRQ, 1, 1);
		rk_device_register(d);
	}

	rk_irq_request(1, ps2_irq, NULL, d, "ps2-keyboard");
	rk_irq_unmask(1);
	pr_info("PS/2 keyboard ready on IRQ 1");
}
