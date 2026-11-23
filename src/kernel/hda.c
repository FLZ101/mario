#include <hda.h>
#include <lib/stddef.h>
#include <lib/string.h>
#include <misc.h>
#include <mm/ioremap.h>
#include <multiboot.h>
#include <pci.h>

#define HDA_CLASS 0x04
#define HDA_SUBCLASS 0x03
#define HDA_GCAP 0x00
#define HDA_VERSION_MAJOR 0x03

static int probe_only;

void hda_boot_options(const struct multiboot_info *boot)
{
	const char *option;

	if (!(boot->flags & MB_FLAG_CMDLINE) || !boot->cmdline)
		return;
	option = (const char *)__vir(boot->cmdline);
	while (*option) {
		if (!strncmp(option, "hda=probe", 9) &&
		    (!option[9] || option[9] == ' ')) {
			probe_only = 1;
			return;
		}
		while (*option && *option != ' ')
			option++;
		while (*option == ' ')
			option++;
	}
}

static int hda_probe(const struct pci_device *dev, void *context)
{
	unsigned long address;
	size_t size;
	uint16_t command, caps;
	uint8_t version;
	unsigned char *bar, *alias;
	int stable = 0;

	if (pci_mem_bar32(dev, 0, &address, &size) || size < 4)
		return 0;
	bar = ioremap(address, size);
	if (!bar)
		return 0;
	command = pci_command(dev);
	pci_write_command(dev, command | PCI_COMMAND_MEMORY);
	caps = readw(bar + HDA_GCAP);
	version = readb(bar + HDA_VERSION_MAJOR);
	alias = ioremap(address + HDA_VERSION_MAJOR, sizeof(version));
	if (alias) {
		iounmap(bar + HDA_VERSION_MAJOR);
		stable = version == 1 && caps != 0xffff && readb(alias) == version;
		iounmap(alias);
	}
	stable = stable && !ioremap(0xfffffffeUL, 4) &&
		 readw(bar + HDA_GCAP) == caps && readb(bar + HDA_VERSION_MAJOR) == version;
	iounmap(bar);
	pci_write_command(dev, command);
	if (stable)
		pr_info("HDA: probe %u:%u.%u GCAP=%x major=%u; unmapped\n",
			dev->bus, dev->slot, dev->func, caps, version);
	else
		pr_warn("HDA: unstable probe at %u:%u.%u\n",
			dev->bus, dev->slot, dev->func);
	return 0;
}

void hda_init(void)
{
	if (probe_only)
		pci_scan_class(HDA_CLASS, HDA_SUBCLASS, PCI_ANY_PROG_IF,
			       hda_probe, NULL);
}
