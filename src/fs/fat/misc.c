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
