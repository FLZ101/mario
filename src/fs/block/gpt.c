#include <errno.h>

#include <fs/gpt.h>

#include <lib/crc32.h>
#include <lib/string.h>

#include <mm/kmalloc.h>

struct gpt_header_info {
	uint32_t header_size;
	uint32_t header_crc;
	uint64_t current_lba;
	uint64_t backup_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	uint64_t entries_lba;
	uint32_t num_entries;
	uint32_t entry_size;
	uint32_t entries_crc;
};

static uint32_t read_le32(const uint8_t *p)
{
	return (uint32_t)p[0] |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p)
{
	return (uint64_t)read_le32(p) |
	       ((uint64_t)read_le32(p + 4) << 32);
}

static int gpt_parse_header(const uint8_t *sector, uint64_t header_lba,
			    uint64_t disk_sectors, struct gpt_header_info *hdr)
{
	uint8_t header[GPT_SECTOR_SIZE];
	uint64_t entries_bytes;
	uint64_t entries_sectors;

	if (memcmp(sector, "EFI PART", 8))
		return -EINVAL;

	hdr->header_size = read_le32(sector + 12);
	if (hdr->header_size < 92 || hdr->header_size > GPT_SECTOR_SIZE)
		return -EINVAL;

	hdr->header_crc = read_le32(sector + 16);
	hdr->current_lba = read_le64(sector + 24);
	hdr->backup_lba = read_le64(sector + 32);
	hdr->first_usable_lba = read_le64(sector + 40);
	hdr->last_usable_lba = read_le64(sector + 48);
	hdr->entries_lba = read_le64(sector + 72);
	hdr->num_entries = read_le32(sector + 80);
	hdr->entry_size = read_le32(sector + 84);
	hdr->entries_crc = read_le32(sector + 88);

	if (hdr->current_lba != header_lba)
		return -EINVAL;
	if (hdr->backup_lba <= hdr->current_lba ||
	    hdr->backup_lba >= disk_sectors)
		return -EINVAL;
	if (hdr->first_usable_lba < 1 ||
	    hdr->first_usable_lba > hdr->last_usable_lba)
		return -EINVAL;
	if (hdr->last_usable_lba >= disk_sectors)
		return -EINVAL;
	if (!hdr->num_entries ||
	    hdr->num_entries > GPT_MAX_PARTITION_ENTRIES)
		return -EINVAL;
	if (hdr->entry_size < 128 || hdr->entry_size > GPT_MAX_ENTRY_SIZE)
		return -EINVAL;

	entries_bytes = (uint64_t)hdr->num_entries * hdr->entry_size;
	entries_sectors = (entries_bytes + GPT_SECTOR_SIZE - 1) /
			  GPT_SECTOR_SIZE;
	if (hdr->entries_lba >= disk_sectors ||
	    entries_sectors > disk_sectors - hdr->entries_lba)
		return -EINVAL;

	memcpy(header, sector, hdr->header_size);
	memset(header + 16, 0, 4);
	if (crc32(0, header, hdr->header_size) != hdr->header_crc)
		return -EINVAL;

	return 0;
}

static int gpt_load_header(void *priv, gpt_read_sectors_fn read_sectors,
			   uint64_t lba, uint64_t disk_sectors,
			   struct gpt_header_info *hdr)
{
	uint8_t sector[GPT_SECTOR_SIZE];

	if (read_sectors(priv, lba, 1, sector))
		return -EIO;
	return gpt_parse_header(sector, lba, disk_sectors, hdr);
}

static int gpt_has_protective_mbr(const uint8_t *sector)
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		if (sector[446 + i * 16 + 4] == 0xee)
			return 1;
	return 0;
}

int gpt_parse(void *priv, uint64_t disk_sectors,
	      gpt_read_sectors_fn read_sectors,
	      struct gpt_partition *parts, unsigned int max_parts,
	      unsigned int *nr_parts)
{
	struct gpt_header_info hdr;
	uint8_t sector[GPT_SECTOR_SIZE];
	uint8_t *entries;
	uint64_t entries_bytes;
	uint64_t entries_sectors;
	unsigned int i;
	int error;

	if (!parts || !nr_parts || !read_sectors || disk_sectors < 2)
		return -EINVAL;

	*nr_parts = 0;
	memset(parts, 0, max_parts * sizeof(*parts));

	if (read_sectors(priv, 0, 1, sector))
		return -EIO;
	if (sector[510] != 0x55 || sector[511] != 0xaa)
		return -ENOENT;
	if (!gpt_has_protective_mbr(sector))
		return -ENOENT;

	error = gpt_load_header(priv, read_sectors, 1, disk_sectors, &hdr);
	if (error)
		error = gpt_load_header(priv, read_sectors, disk_sectors - 1,
					disk_sectors, &hdr);
	if (error)
		return -ENOENT;

	entries_bytes = (uint64_t)hdr.num_entries * hdr.entry_size;
	entries_sectors = (entries_bytes + GPT_SECTOR_SIZE - 1) /
			  GPT_SECTOR_SIZE;
	entries = kmalloc((size_t)entries_bytes);
	if (!entries)
		return -ENOMEM;

	if (read_sectors(priv, hdr.entries_lba, entries_sectors, entries)) {
		kfree(entries);
		return -EIO;
	}

	if (crc32(0, entries, (size_t)entries_bytes) != hdr.entries_crc) {
		kfree(entries);
		return -ENOENT;
	}

	for (i = 0; i < hdr.num_entries; i++) {
		const uint8_t *entry = entries + (size_t)i * hdr.entry_size;
		uint64_t first_lba;
		uint64_t last_lba;
		uint64_t nr_sectors;

		first_lba = read_le64(entry + 32);
		last_lba = read_le64(entry + 40);
		if (first_lba == 0 && last_lba == 0)
			continue;
		if (first_lba > last_lba)
			continue;
		if (first_lba < hdr.first_usable_lba ||
		    last_lba > hdr.last_usable_lba)
			continue;
		if (last_lba >= disk_sectors)
			continue;

		nr_sectors = last_lba - first_lba + 1;
		if (last_lba > 0xffffffffULL || nr_sectors > 0x7fffffffULL)
			continue;

		(*nr_parts)++;
		if (i >= max_parts)
			continue;

		memcpy(parts[i].type_guid, entry, 16);
		memcpy(parts[i].unique_guid, entry + 16, 16);
		parts[i].attrs = read_le64(entry + 48);
		parts[i].first_lba = first_lba;
		parts[i].last_lba = last_lba;
		parts[i].present = 1;
	}

	kfree(entries);
	return 0;
}
