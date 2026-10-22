#include <fs/fs.h>
#include <fs/fat/fat.h>

#include <lib/string.h>

static int fat_tolower(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A' + 'a';
	return c;
}

/*
 * Convert an 11-byte FAT short-name field into the normal byte-oriented VFS
 * name with a dot before the extension.  Spaces are trimmed from both fields.
 */
void fat_short_name_from_entry(const uint8_t *entry, char *name)
{
	int i;
	int len = 0;

	for (i = 0; i < 8 && entry[i] != ' '; i++)
		name[len++] = entry[i];
	if (entry[8] != ' ') {
		name[len++] = '.';
		for (i = 8; i < 11 && entry[i] != ' '; i++)
			name[len++] = entry[i];
	}
	name[len] = '\0';
}

/*
 * FAT short names are compared case-insensitively and without trailing dot
 * distinctions.
 */
int fat_short_name_match(const char *name, int len, const uint8_t *entry)
{
	char short_name[13];
	int short_len;
	int i;

	fat_short_name_from_entry(entry, short_name);
	short_len = strlen(short_name);
	if (short_len != len)
		return 0;
	for (i = 0; i < len; i++) {
		if (fat_tolower(name[i]) != fat_tolower(short_name[i]))
			return 0;
	}
	return 1;
}

/*
 * Convert a VFS name to a DOS 8.3 short name.  The writable implementation
 * intentionally does not generate LFN entries; names that do not fit the
 * classic 8.3 format are rejected.
 */
int fat_name_to_short(const char *name, int len, uint8_t short_name[11])
{
	int base_len = 0;
	int ext_len = 0;
	int i;
	int last_dot = -1;

	if (len < 1 || len > 12)
		return -ENAMETOOLONG;
	if (name[0] == '.' || name[len - 1] == '.')
		return -EINVAL;

	for (i = 0; i < len; i++) {
		char c = name[i];

		if (c == '.') {
			if (last_dot >= 0 || i == 0 || i == len - 1)
				return -EINVAL;
			last_dot = i;
		}
	}

	if (last_dot >= 0) {
		base_len = last_dot;
		ext_len = len - last_dot - 1;
		if (!base_len || !ext_len || base_len > 8 || ext_len > 3)
			return -ENAMETOOLONG;
	} else {
		base_len = len;
		if (base_len > 8)
			return -ENAMETOOLONG;
	}

	memset(short_name, ' ', 11);
	for (i = 0; i < base_len; i++) {
		char c = name[i];

		if (c >= 'a' && c <= 'z')
			c = c - 'a' + 'A';
		if (c == ' ' || c == '.' || c == '/' || c == '\\' ||
		    c == '?' || c == '*' || c == ':' || c == '|' ||
		    c == '<' || c == '>' || c == '"')
			return -EINVAL;
		short_name[i] = c;
	}
	if (last_dot >= 0) {
		for (i = 0; i < ext_len; i++) {
			char c = name[last_dot + 1 + i];

			if (c >= 'a' && c <= 'z')
				c = c - 'a' + 'A';
			if (c == ' ' || c == '.' || c == '/' || c == '\\' ||
			    c == '?' || c == '*' || c == ':' || c == '|' ||
			    c == '<' || c == '>' || c == '"')
				return -EINVAL;
			short_name[8 + i] = c;
		}
	}
	return 0;
}

int fat_short_name_equal(const uint8_t *entry, const uint8_t short_name[11])
{
	int i;

	for (i = 0; i < 11; i++) {
		uint8_t a = entry[i];
		uint8_t b = short_name[i];

		if (a >= 'a' && a <= 'z')
			a = a - 'a' + 'A';
		if (b >= 'a' && b <= 'z')
			b = b - 'a' + 'A';
		if (a != b)
			return 0;
	}
	return 1;
}

int fat_make_short_entry(uint8_t *entry, const uint8_t short_name[11],
			 uint8_t attributes, uint32_t start_cluster,
			 uint32_t size)
{
	memset(entry, 0, FAT_ENTRY_SIZE);
	memcpy(entry, short_name, 11);
	entry[11] = attributes;
	fat_write16(entry + 20, start_cluster >> 16);
	fat_write16(entry + 26, start_cluster);
	fat_write32(entry + 28, size);
	return 0;
}
