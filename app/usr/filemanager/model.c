#include "model.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int filemanager_entry_compare(const void *left, const void *right)
{
	const filemanager_entry *left_entry = left;
	const filemanager_entry *right_entry = right;

	return strcmp(left_entry->name, right_entry->name);
}

void filemanager_directory_init(filemanager_directory *directory)
{
	memset(directory, 0, sizeof(*directory));
}

filemanager_entry_type filemanager_entry_type_from_dtype(unsigned char dtype)
{
	switch (dtype) {
	case DT_DIR:
		return FILEMANAGER_ENTRY_DIRECTORY;
	case DT_REG:
		return FILEMANAGER_ENTRY_FILE;
	case DT_LNK:
		return FILEMANAGER_ENTRY_SYMLINK;
	case DT_UNKNOWN:
		return FILEMANAGER_ENTRY_UNKNOWN;
	default:
		return FILEMANAGER_ENTRY_OTHER;
	}
}

int filemanager_directory_add(filemanager_directory *directory, const char *name,
				  unsigned char dtype)
{
	size_t name_bytes;
	filemanager_entry *entry;

	if (directory == NULL || name == NULL) {
		errno = EINVAL;
		return -1;
	}
	name_bytes = strnlen(name, FILEMANAGER_NAME_BYTES + 1);
	if (name_bytes == 0 || name_bytes > FILEMANAGER_NAME_BYTES ||
	    directory->count == FILEMANAGER_ENTRY_LIMIT) {
		directory->omitted_count++;
		return 0;
	}
	entry = &directory->entries[directory->count++];
	entry->type = filemanager_entry_type_from_dtype(dtype);
	memcpy(entry->name, name, name_bytes);
	entry->name[name_bytes] = '\0';
	return 0;
}

int filemanager_directory_load(filemanager_directory *directory, const char *path)
{
	filemanager_directory *loaded;
	DIR *stream;

	if (directory == NULL || path == NULL) {
		errno = EINVAL;
		return -1;
	}
	stream = opendir(path);
	if (stream == NULL)
		return -1;
	/* Keep this transactional listing off the small initial application stack. */
	loaded = malloc(sizeof(*loaded));
	if (loaded == NULL) {
		int saved_errno = errno;

		closedir(stream);
		errno = saved_errno;
		return -1;
	}
	filemanager_directory_init(loaded);
	for (;;) {
		struct dirent *entry;

		errno = 0;
		entry = readdir(stream);
		if (entry == NULL)
			break;
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (filemanager_directory_add(loaded, entry->d_name, entry->d_type) < 0) {
			int saved_errno = errno;

			closedir(stream);
			free(loaded);
			errno = saved_errno;
			return -1;
		}
	}
	if (errno != 0) {
		int saved_errno = errno;

		closedir(stream);
		free(loaded);
		errno = saved_errno;
		return -1;
	}
	if (closedir(stream) < 0) {
		free(loaded);
		return -1;
	}
	qsort(loaded->entries, loaded->count, sizeof(loaded->entries[0]),
	      filemanager_entry_compare);
	/* Leave the caller's prior listing usable when opening a new directory fails. */
	*directory = *loaded;
	free(loaded);
	return 0;
}
