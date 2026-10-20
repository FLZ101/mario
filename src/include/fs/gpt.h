#ifndef _FS_GPT_H
#define _FS_GPT_H

#include <types.h>

#define GPT_SECTOR_SIZE		512
#define GPT_MAX_PARTITION_ENTRIES	256
#define GPT_MAX_ENTRY_SIZE	4096

struct gpt_partition {
	uint64_t first_lba;
	uint64_t last_lba;
	uint8_t type_guid[16];
	uint8_t unique_guid[16];
	uint64_t attrs;
	uint8_t present;
};

typedef int (*gpt_read_sectors_fn)(void *priv, uint64_t lba,
				   unsigned int nr, void *buf);

int gpt_parse(void *priv, uint64_t disk_sectors,
	      gpt_read_sectors_fn read_sectors,
	      struct gpt_partition *parts, unsigned int max_parts,
	      unsigned int *nr_parts);

#endif /* _FS_GPT_H */
