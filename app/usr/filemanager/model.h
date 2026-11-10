#ifndef FILEMANAGER_MODEL_H
#define FILEMANAGER_MODEL_H

#include <stddef.h>

#define FILEMANAGER_ENTRY_LIMIT 256U
#define FILEMANAGER_NAME_BYTES 255U

typedef enum {
	FILEMANAGER_ENTRY_DIRECTORY,
	FILEMANAGER_ENTRY_FILE,
	FILEMANAGER_ENTRY_SYMLINK,
	FILEMANAGER_ENTRY_OTHER,
	FILEMANAGER_ENTRY_UNKNOWN,
} filemanager_entry_type;

typedef struct {
	filemanager_entry_type type;
	char name[FILEMANAGER_NAME_BYTES + 1];
} filemanager_entry;

typedef struct {
	filemanager_entry entries[FILEMANAGER_ENTRY_LIMIT];
	size_t count;
	/* Entries beyond a fixed client-side budget are intentionally not retained. */
	size_t omitted_count;
} filemanager_directory;

void filemanager_directory_init(filemanager_directory *directory);
filemanager_entry_type filemanager_entry_type_from_dtype(unsigned char dtype);
int filemanager_directory_add(filemanager_directory *directory, const char *name,
				  unsigned char dtype);
int filemanager_directory_load(filemanager_directory *directory, const char *path);

#endif
