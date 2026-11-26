#include <errno.h>
#include <io.h>
#include <lib/stddef.h>
#include <misc.h>
#include <net/ethernet.h>
#include <net/worker.h>

/*
 * Linux reboot(2) command values.  musl's reboot() wrapper passes these
 * through unchanged together with the two magic numbers required by the
 * syscall.
 */
#define LINUX_REBOOT_MAGIC1 0xfee1deadU
#define LINUX_REBOOT_MAGIC2 672274793U
#define LINUX_REBOOT_MAGIC2A 85072278U
#define LINUX_REBOOT_MAGIC2B 369367448U
#define LINUX_REBOOT_MAGIC2C 537993216U

#define LINUX_REBOOT_CMD_RESTART 0x01234567U
#define LINUX_REBOOT_CMD_HALT 0xCDEF0123U
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDCU

static void machine_halt(void) __attribute__((noreturn));

static void machine_halt(void)
{
	cli();
	for (;;)
		__asm__ __volatile__("hlt" : : : "memory");
}

static void machine_restart(void) __attribute__((noreturn));

static void machine_restart(void)
{
	unsigned int index;

	/* Pulse the keyboard controller's CPU reset line. */
	for (index = 0; index < 100000; index++) {
		if ((inb(0x64) & 0x02) == 0)
			break;
	}
	outb(0x64, 0xFE);
	machine_halt();
}

static void machine_power_off(void) __attribute__((noreturn));

static void machine_power_off(void)
{
	/*
	 * ACPI PM1a_CNT is 0x604 on QEMU's PIIX4, and Bochs uses the
	 * legacy 0xB004 power-off port.  Write SLP_EN (bit 13) with S5
	 * (SLP_TYP 0) and fall back to a halted CPU if neither takes.
	 */
	outw(0x604, 0x2000);
	outw(0xB004, 0x2000);
	machine_halt();
}

int sys_reboot(int magic1, int magic2, unsigned int cmd, void *arg)
{
	int error;

	(void)arg;

	if (magic1 != LINUX_REBOOT_MAGIC1)
		return -EINVAL;
	if (magic2 != LINUX_REBOOT_MAGIC2 &&
	    magic2 != LINUX_REBOOT_MAGIC2A &&
	    magic2 != LINUX_REBOOT_MAGIC2B &&
	    magic2 != LINUX_REBOOT_MAGIC2C)
		return -EINVAL;
	if (cmd != LINUX_REBOOT_CMD_RESTART && cmd != LINUX_REBOOT_CMD_HALT &&
	    cmd != LINUX_REBOOT_CMD_POWER_OFF)
		return -EINVAL;
	error = net_call(ethernet_stop, NULL);
	if (error && error != -ENETDOWN)
		return error;

	switch (cmd) {
	case LINUX_REBOOT_CMD_RESTART:
		machine_restart();
	case LINUX_REBOOT_CMD_HALT:
		machine_halt();
	case LINUX_REBOOT_CMD_POWER_OFF:
		machine_power_off();
	default:
		return -EINVAL;
	}
}
