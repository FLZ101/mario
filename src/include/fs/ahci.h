#ifndef _FS_AHCI_H
#define _FS_AHCI_H

#include <types.h>

/* Optional AHCI disk initialization and its shared IRQ handler. */
void hd_init(void);
void irq_ahci(void);

enum ahci_profile_command {
	AHCI_PROFILE_COMMAND_READ,
	AHCI_PROFILE_COMMAND_WRITE,
	AHCI_PROFILE_COMMAND_IDENTIFY,
	AHCI_PROFILE_COMMAND_FLUSH,
	AHCI_PROFILE_COMMAND_OTHER,
	AHCI_PROFILE_COMMAND_COUNT,
};

enum ahci_profile_size {
	AHCI_PROFILE_SIZE_NONE,
	AHCI_PROFILE_SIZE_1_SECTOR,
	AHCI_PROFILE_SIZE_2_TO_8_SECTORS,
	AHCI_PROFILE_SIZE_9_TO_64_SECTORS,
	AHCI_PROFILE_SIZE_OVER_64_SECTORS,
	AHCI_PROFILE_SIZE_COUNT,
};

struct ahci_profile_counter {
	uint64_t commands;
	uint64_t sectors;
	uint64_t wait_cycles;
};

struct ahci_profile {
	struct ahci_profile_counter total;
	struct ahci_profile_counter command[AHCI_PROFILE_COMMAND_COUNT];
	struct ahci_profile_counter size[AHCI_PROFILE_SIZE_COUNT];
};

void ahci_profile_reset(void);
void ahci_profile_get(struct ahci_profile *profile);

#endif /* _FS_AHCI_H */
