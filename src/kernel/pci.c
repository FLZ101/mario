#include <pci.h>
#include <io.h>
#include <errno.h>

/*
 * Minimal PCI configuration-space enumeration.  Devices are discovered with
 * the legacy 0xcf8/0xcfc I/O-port mechanism, which is sufficient for the
 * flat buses used by QEMU's i440fx and q35 machines.
 */

#define PCI_CONFIG_ADDRESS	0xcf8
#define PCI_CONFIG_DATA		0xcfc

#define PCI_VENDOR_ID		0x00
#define PCI_DEVICE_ID		0x02
#define PCI_CLASS_REVISION	0x08
#define PCI_HEADER_TYPE		0x0c
#define PCI_BAR0		0x10
#define PCI_SECONDARY_BUS	0x18
#define PCI_INTERRUPT_LINE	0x3c

#define PCI_BAR_IO		0x1U
#define PCI_BAR_MEM32		0x2U
#define PCI_BAR_MEM64		0x4U

uint32_t pci_config_read32(unsigned int bus, unsigned int slot,
			   unsigned int func, unsigned int offset)
{
	uint32_t address;

	address = 0x80000000U |
		  ((bus & 0xffU) << 16) |
		  ((slot & 0x1fU) << 11) |
		  ((func & 0x07U) << 8) |
		  (offset & 0xfcU);
	outl(PCI_CONFIG_ADDRESS, address);
	return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(unsigned int bus, unsigned int slot,
			unsigned int func, unsigned int offset, uint32_t value)
{
	uint32_t address;

	address = 0x80000000U |
		  ((bus & 0xffU) << 16) |
		  ((slot & 0x1fU) << 11) |
		  ((func & 0x07U) << 8) |
		  (offset & 0xfcU);
	outl(PCI_CONFIG_ADDRESS, address);
	outl(PCI_CONFIG_DATA, value);
}

static uint8_t pci_class_code(uint32_t class_revision)
{
	return (class_revision >> 24) & 0xff;
}

static uint8_t pci_subclass(uint32_t class_revision)
{
	return (class_revision >> 16) & 0xff;
}

static uint8_t pci_prog_if(uint32_t class_revision)
{
	return (class_revision >> 8) & 0xff;
}

static void pci_read_device(unsigned int bus, unsigned int slot,
			    unsigned int func, struct pci_device *dev)
{
	uint32_t class_revision;
	uint32_t interrupt;
	int bar;

	dev->bus = bus;
	dev->slot = slot;
	dev->func = func;
	dev->vendor_id = pci_config_read32(bus, slot, func, PCI_VENDOR_ID) & 0xffff;
	dev->device_id = pci_config_read32(bus, slot, func, PCI_DEVICE_ID) >> 16;

	class_revision = pci_config_read32(bus, slot, func, PCI_CLASS_REVISION);
	dev->class_code = pci_class_code(class_revision);
	dev->subclass = pci_subclass(class_revision);
	dev->prog_if = pci_prog_if(class_revision);

	interrupt = pci_config_read32(bus, slot, func, PCI_INTERRUPT_LINE);
	dev->irq = interrupt & 0xff;

	for (bar = 0; bar < 6; bar++)
		dev->bars[bar] =
			pci_config_read32(bus, slot, func, PCI_BAR0 + bar * 4);
}

/*
 * Recursively scan a bus for a class/subclass/prog-if match, descending into
 * PCI-to-PCI bridges when present.  The seen[] map prevents bridge loops.
 */
static int pci_find_on_bus(unsigned int bus, uint8_t class_code,
			   uint8_t subclass, uint8_t prog_if,
			   struct pci_device *dev, unsigned char *seen)
{
	unsigned int slot;
	unsigned int func;

	if (bus > 255 || seen[bus])
		return 0;
	seen[bus] = 1;

	for (slot = 0; slot < 32; slot++) {
		uint32_t vendor = pci_config_read32(bus, slot, 0, PCI_VENDOR_ID);

		if ((vendor & 0xffff) == 0xffff)
			continue;

		uint32_t header_type =
			pci_config_read32(bus, slot, 0, PCI_HEADER_TYPE);
		unsigned int max_func = (header_type & 0x80) ? 8 : 1;

		for (func = 0; func < max_func; func++) {
			struct pci_device candidate;

			vendor = pci_config_read32(bus, slot, func, PCI_VENDOR_ID);
			if ((vendor & 0xffff) == 0xffff)
				continue;

			pci_read_device(bus, slot, func, &candidate);
			if (candidate.class_code == class_code &&
			    candidate.subclass == subclass &&
			    candidate.prog_if == prog_if) {
				*dev = candidate;
				return 1;
			}

			header_type = pci_config_read32(bus, slot, func,
							PCI_HEADER_TYPE);
			if ((header_type & 0x7f) == 0x01) {
				uint32_t secondary =
					pci_config_read32(bus, slot, func,
							  PCI_SECONDARY_BUS);
				if (pci_find_on_bus((secondary >> 8) & 0xff,
						    class_code, subclass,
						    prog_if, dev, seen))
					return 1;
			}
		}
	}

	return 0;
}

int pci_find_device_by_class(uint8_t class_code, uint8_t subclass,
			     uint8_t prog_if, struct pci_device *dev)
{
	unsigned char seen[256] = {0};

	return pci_find_on_bus(0, class_code, subclass, prog_if, dev, seen);
}

int pci_bar_info(const struct pci_device *dev, int bar, uint64_t *address,
		 uint32_t *flags, int *bar_count)
{
	uint32_t low;
	uint32_t type;

	if (!dev || !address || !flags || !bar_count || bar < 0 || bar > 5)
		return -EINVAL;

	low = dev->bars[bar];
	if (low & PCI_BAR_IO) {
		/* I/O BARs are ignored by the AHCI driver. */
		*address = low & ~3U;
		*flags = PCI_BAR_IO;
		*bar_count = 1;
		return 0;
	}

	type = (low >> 1) & 0x03;
	if (type == 0x00) {
		*address = low & ~15U;
		*flags = PCI_BAR_MEM32;
		*bar_count = 1;
		return 0;
	}

	if (type == 0x02 && bar < 5) {
		uint64_t high = dev->bars[bar + 1];

		/* A 64-bit memory BAR consumes two adjacent config words. */
		*address = (high << 32) | (low & ~15U);
		*flags = PCI_BAR_MEM64;
		*bar_count = 2;
		return 0;
	}

	return -EINVAL;
}
