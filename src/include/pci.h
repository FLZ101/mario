#ifndef _PCI_H
#define _PCI_H

#include <types.h>

/* Minimal PCI device descriptor used for AHCI controller discovery. */
struct pci_device {
	uint8_t bus;
	uint8_t slot;
	uint8_t func;
	uint16_t vendor_id;
	uint16_t device_id;
	uint8_t class_code;
	uint8_t subclass;
	uint8_t prog_if;
	uint8_t irq;
	uint32_t bars[6];
};

uint32_t pci_config_read32(unsigned int bus, unsigned int slot,
			   unsigned int func, unsigned int offset);
void pci_config_write32(unsigned int bus, unsigned int slot,
			unsigned int func, unsigned int offset, uint32_t value);

int pci_find_device_by_class(uint8_t class_code, uint8_t subclass,
			     uint8_t prog_if, struct pci_device *dev);

int pci_bar_info(const struct pci_device *dev, int bar, uint64_t *address,
		 uint32_t *flags, int *bar_count);

#endif /* _PCI_H */
