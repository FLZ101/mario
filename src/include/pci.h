#ifndef _PCI_H
#define _PCI_H

#include <types.h>

#define PCI_ANY_PROG_IF (-1)
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_MASTER 0x0004

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

typedef int (*pci_match_fn)(const struct pci_device *dev, void *context);

/* Boot-only enumeration; a nonzero callback result stops the scan. */
int pci_scan_class(uint8_t class_code, uint8_t subclass, int prog_if,
		   pci_match_fn match, void *context);
int pci_find_device_by_class(uint8_t class_code, uint8_t subclass,
			     uint8_t prog_if, struct pci_device *dev);

int pci_bar_info(const struct pci_device *dev, int bar, uint64_t *address,
		 uint32_t *flags, int *bar_count);
int pci_mem_bar32(const struct pci_device *dev, int bar,
		  unsigned long *address, size_t *size);
uint16_t pci_command(const struct pci_device *dev);
void pci_write_command(const struct pci_device *dev, uint16_t command);

#endif /* _PCI_H */
