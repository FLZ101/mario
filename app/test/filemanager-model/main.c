#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../usr/filemanager/model.h"

static void test_sorted_directory(void)
{
	char path[64];
	filemanager_directory directory;

	assert(snprintf(path, sizeof(path), "/tmp/filemanager-model-%d", getpid()) > 0);
	assert(mkdir(path, 0700) == 0);
	assert(chdir(path) == 0);
	assert(close(creat("zeta", 0600)) == 0);
	assert(close(creat("alpha", 0600)) == 0);
	assert(mkdir("directory", 0700) == 0);
	assert(chdir("/") == 0);
	assert(filemanager_directory_load(&directory, path) == 0);
	assert(directory.count == 3 && directory.omitted_count == 0);
	assert(!strcmp(directory.entries[0].name, "alpha"));
	assert(directory.entries[0].metadata_available);
	assert(directory.entries[0].size == 0);
	assert(directory.entries[0].modified_time > 0);
	assert(!strcmp(directory.entries[1].name, "directory"));
	assert(directory.entries[1].type == FILEMANAGER_ENTRY_DIRECTORY);
	assert(directory.entries[1].metadata_available);
	assert(!strcmp(directory.entries[2].name, "zeta"));
	assert(chdir(path) == 0);
	assert(unlink("alpha") == 0);
	assert(unlink("zeta") == 0);
	assert(rmdir("directory") == 0);
	assert(chdir("/") == 0);
	assert(rmdir(path) == 0);
}

static void test_empty_directory(void)
{
	char path[64];
	filemanager_directory directory;

	assert(snprintf(path, sizeof(path), "/tmp/filemanager-empty-%d", getpid()) > 0);
	assert(mkdir(path, 0700) == 0);
	assert(filemanager_directory_load(&directory, path) == 0);
	assert(directory.count == 0 && directory.omitted_count == 0);
	assert(rmdir(path) == 0);
}

static void test_bounded_and_unknown_entries(void)
{
	char long_name[FILEMANAGER_NAME_BYTES + 2];
	char name[16];
	filemanager_directory directory;

	memset(long_name, 'a', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';
	filemanager_directory_init(&directory);
	assert(filemanager_directory_add(&directory, long_name, DT_REG) == 0);
	assert(directory.count == 0 && directory.omitted_count == 1);
	assert(filemanager_directory_add(&directory, "unknown", DT_UNKNOWN) == 0);
	assert(directory.count == 1);
	assert(directory.entries[0].type == FILEMANAGER_ENTRY_UNKNOWN);
	assert(!directory.entries[0].metadata_available);
	filemanager_directory_init(&directory);
	for (size_t index = 0; index < FILEMANAGER_ENTRY_LIMIT; index++) {
		assert(snprintf(name, sizeof(name), "%zu", index) > 0);
		assert(filemanager_directory_add(&directory, name, DT_REG) == 0);
	}
	assert(filemanager_directory_add(&directory, "overflow", DT_REG) == 0);
	assert(directory.count == FILEMANAGER_ENTRY_LIMIT && directory.omitted_count == 1);
}

int main(void)
{
	test_sorted_directory();
	test_empty_directory();
	test_bounded_and_unknown_entries();
	puts("filemanager-model: sorted, empty, bounded, and unknown entries ok");
	return 0;
}
