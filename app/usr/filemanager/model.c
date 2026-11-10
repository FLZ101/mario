#include "model.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static filemanager_entry_type filemanager_entry_type_from_mode(mode_t mode)
{
	if (S_ISDIR(mode))
		return FILEMANAGER_ENTRY_DIRECTORY;
	if (S_ISREG(mode))
		return FILEMANAGER_ENTRY_FILE;
	if (S_ISLNK(mode))
		return FILEMANAGER_ENTRY_SYMLINK;
	return FILEMANAGER_ENTRY_OTHER;
}

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
		size_t count;

		errno = 0;
		entry = readdir(stream);
		if (entry == NULL)
			break;
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		count = loaded->count;
		if (filemanager_directory_add(loaded, entry->d_name, entry->d_type) < 0) {
			int saved_errno = errno;

			closedir(stream);
			free(loaded);
			errno = saved_errno;
			return -1;
		}
		if (loaded->count != count) {
			filemanager_entry *loaded_entry = &loaded->entries[count];
			struct stat status;

			/* Do not lose a visible entry when it vanishes after readdir(). */
			if (fstatat(dirfd(stream), entry->d_name, &status,
				AT_SYMLINK_NOFOLLOW) == 0) {
				loaded_entry->size = status.st_size < 0 ? 0 : (uint64_t)status.st_size;
				loaded_entry->modified_time = status.st_mtime;
				loaded_entry->metadata_available = 1;
				if (loaded_entry->type == FILEMANAGER_ENTRY_UNKNOWN)
					loaded_entry->type = filemanager_entry_type_from_mode(status.st_mode);
			}
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
