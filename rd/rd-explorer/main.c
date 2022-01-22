#include "../rd-make/mariofs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A very simple tool to explore a rd made by rd-make
 */

FILE *rd;

/* Absolute pathname of 'Current Working Directory' */
char cwd[1024];

struct mario_dir_entry cur_dir_entry;
uint32_t block_size;

#define MAX_USED	(block_size - 4)

void init_explorer(void)
{
	struct mario_super_block sb;

	/* read super block */
	fread(&sb, sizeof(sb), 1, rd);
	if (MARIO_MAGIC != sb.magic) {
		printf("Not an invalid rd made by rd-make\n");
		exit(-1);
	}
	memcpy(&cur_dir_entry, &sb.root, sizeof(cur_dir_entry));
	block_size = sb.block_size;
	strcpy(cwd, "/");

	printf("The following build-in commands is supported:\n"
		"    cd, cat, exit, help, ls\n"
		"Type `help' for more information\n\n");
}

char cmd[128];

/*
 * Read a mario_dir_entry
 * @dent_offset: offset into rd of that directory entry
 */
void read_dir_entry(uint32_t dent_offset, struct mario_dir_entry *entry)
{
	fseek(rd, dent_offset, SEEK_SET);
	fread(entry, sizeof(*entry), 1, rd);
}

/*
 * find mario_dir_entry by name in a directory
 * @dir: block number of that directory
 */
void find_dir_entry(uint32_t block_nr, char *name, struct mario_dir_entry *entry)
{
	int i, n;
	uint32_t offset;

	n = MAX_USED / sizeof(*entry);
	offset = block_nr * block_size;

scan_a_new_block:
	for (i = 0; i < n; i++) {
		read_dir_entry(offset + i*sizeof(*entry), entry);
		/* end of that directory? */
		if (!entry->data)
			return;
		if (!strcmp(entry->name, name))
			return;
	}
	fseek(rd, offset + MAX_USED, SEEK_SET);
	fread(&offset, 4, 1, rd);
	if (offset) {
		offset *= block_size;
		goto scan_a_new_block;
	}
	entry->data = 0;
}

/*
 * cd ..
 */
void cut_cwd(void)
{
	int i;

	if ((i = strlen(cwd)) > 1)
		cwd[i-1] = '\0';
	for (; cwd[i-1] != '/'; i--)
		;
	cwd[i] = '\0';
}

void cd(void)
{
	struct mario_dir_entry entry;

	/* read dirname */
	scanf("%s", cmd);
	find_dir_entry(cur_dir_entry.data, cmd, &entry);
	if (!entry.data) {
		printf("No such file or directory\n");
		return;
	}
	if (entry.mode != MODE_DIR) {
		printf("Not a directory\n");
		return;
	}

	if (!strcmp(entry.name, "."))
		goto tail;
	if (!strcmp(entry.name, "..")) {
		cut_cwd();
	} else {
		strcat(cwd, entry.name);
		strcat(cwd, "/");
	}
tail:
	cur_dir_entry = entry;
}

/* print content of a file */
void print_file(struct mario_dir_entry *entry)
{
	int i;
	uint32_t size;
	uint32_t offset;
	char *buffer;

	/* Invalid? */
	if (!entry->data)
		return;

	size = entry->size;
	offset = entry->data * block_size;
	buffer = (char *)malloc(MAX_USED);

print_a_new_block:
	fseek(rd, offset, SEEK_SET);
	fread(buffer, 1, MAX_USED, rd);
	for (i = 0; i < MAX_USED; i++) {
		if (size--)
			putchar(buffer[i]);
		else
			goto tail;
	}
	fseek(rd, offset + MAX_USED, SEEK_SET);
	fread(&offset, 4, 1, rd);
	offset *= block_size;	/* offset > 0? */
	goto print_a_new_block;
tail:
	free(buffer);
	printf(">>> END OF CAT >>>\n");
}

void cat(void)
{
	struct mario_dir_entry entry;

	/* read filename */
	scanf("%s", cmd);

	find_dir_entry(cur_dir_entry.data, cmd, &entry);
	if (!entry.data) {
		printf("No such file or directory\n");
		return;
	}
	if (entry.mode != MODE_REG) {
		printf("Not a regular file\n");
		return;
	}
	print_file(&entry);
}

void help(void)
{
	printf("cd DIR      Change CWD\n");
	printf("cat FILE    View content of a (text) file\n");
	printf("exit        Exit the program\n");
	printf("help        Display this help text\n");
	printf("ls          List entries in CWD\n");
	printf("NOTE:\n");
	printf("    DIR and FILE must be a SIMPLE name, ie they "
		"couldn't contain '/'\n");
}

void print_entry(struct mario_dir_entry *entry)
{
	char *type[] = {"REG", "DIR", "BLK", "CHR"};

	printf("%s   ", type[entry->mode]);
	printf("%10u   ", entry->data);
	if (entry->mode == MODE_REG)
		printf("%6u    ", entry->size);
	else
		printf("          ");
	printf("%6u    ", entry->blocks);
	printf("%s\n", entry->name);
}

void ls(void)
{
	int i, n;
	uint32_t offset;
	struct mario_dir_entry entry;

	n = MAX_USED / sizeof(entry);
	offset = cur_dir_entry.data * block_size;

	printf("TYPE        DATA     SIZE    BLOCKS    NAME\n");
	printf("-------------------------------------------\n");

scan_a_new_block:
	for (i = 0; i < n; i++) {
		read_dir_entry(offset + i*sizeof(entry), &entry);
		/* end of that directory? */
		if (!entry.data)
			return;
		print_entry(&entry);
	}
	fseek(rd, offset + MAX_USED, SEEK_SET);
	fread(&offset, 4, 1, rd);
	if (offset) {
		offset *= block_size;
		goto scan_a_new_block;
	}
}

void pwd(void)
{
	puts(cwd);
}

void run_explorer(void)
{
	while (1) {
		printf(">>> ");
		if (EOF == scanf("%s", cmd))
			break;
		if (!strcmp(cmd, "cd"))
			cd();
		else if (!strcmp(cmd, "cat"))
			cat();
		else if (!strcmp(cmd, "exit"))
			break;
		else if (!strcmp(cmd, "help"))
			help();
		else if (!strcmp(cmd, "ls"))
			ls();
		else if (!strcmp(cmd, "pwd"))
			pwd();
	}
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		puts("Usage: rd-explorer RD");
		puts("Explore a rd made by rd-make");
		exit(-1);
	}

	rd = fopen(argv[1], "rb");
	if (!rd) {
		printf("Error:\tfail to open %s\n", argv[1]);
		exit(-1);
	}

	init_explorer();
	run_explorer();

	fclose(rd);
	return 0;
}
