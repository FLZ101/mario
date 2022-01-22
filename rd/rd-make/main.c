#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "mariofs.h"

/*
 * A little tool to copy a directory into a new rd. The implementation is not
 * good enough.
 */

/* maximum rd size in Bytes */
#define MAX_RD_SIZE	(64*1024*1024)

/* supported block size */
uint32_t blksz[] = {512, 0};

void print_hlp_msg(void)
{
	int i;

	puts("Usage: rd-make [OPTION]...");
	puts("Archive files in a directory into a rd\n");
	puts("  --block-size=SIZE\tBytes of a block. The default is 512");
	puts("  --device-list=FILE\t"
		"Add device files listed in FILE into /dev/");
	puts("  --dir=DIRECTORY\tPathname of the directory");
	puts("  --rd=RD\tFilename of the rd");
	puts("  --nr-block=NR\tThe number of blocks");
	puts("  --help\tDisplay this text and exit\n");
	printf("SIZE may be one of the following: ");

	i = 0;
	while (1) {
		printf("%d", blksz[i]);
		if (!blksz[++i])
			break;
		printf(", ");
	}
	puts("\n");

	puts("The maximum size of a rd is 64*1024*1024B and SIZE*NR must "
		"be less than that. If NR is not specified,"
		" the program can decide a proper value of NR itself\n");
	printf("The special directory /dev/ must not exist and this "
		"program will create it itself if FILE is given, which "
		"is to make sure /dev/ contains only device files. "
		"A line in FILE can define a device file, the format of a valid "
		"line is like the following:\n\tNAME MODE MAJOR MINOR\n"
		"For example the following line\n\trd0 2 1 0\ndefines a block "
		"device file /dev/rd0, of which the major device number "
		"is 1 and the minor device number is 0. "
		"NAME must be shorter than %d and the program doesn't check "
		"that. Currently supported "
		"modes by mariofs is MODE_BLK (2) and MODE_CHR (3). For "
		"complete list of devices currently supported by mario, see "
		"doc/devices.txt in source code directory of mario\n\n",
		MAX_NAME_LEN);
	puts("Exit status is 0 if OK, -1 if error");
}

uint32_t block_size = 512;
uint32_t nr_block = 0;
char *device_list = NULL;
char *dir = NULL;
char *rd = NULL;

void do_some_check(void)
{
	int i;

	for (i = 0; blksz[i]; i++)
		if (block_size == blksz[i])
			break;
	if (!blksz[i]) {
		printf("Error:\tUnsupported block size %u\n", block_size);
		exit(-1);
	}
	if (block_size*nr_block > MAX_RD_SIZE) {
		printf("Error:\tSIZE*NR is too big\n");
		exit(-1);
	}
	if (!dir) {
		printf("Error:\tDIRECTORY missed\n");
		exit(-1);
	}
	if (!rd) {
		printf("Error:\tRD missed\n");
		exit(-1);
	}
}

void parse_opt(int argc, char *argv[])
{
	int c;

	struct option opts[] = {
		{"block-size", required_argument, NULL, 'a'},
		{"device-list", required_argument, NULL, 'b'},
		{"dir", required_argument, NULL, 'c'},
		{"rd", required_argument, NULL, 'd'},
		{"nr-block", required_argument, NULL, 'e'},
		{"help", no_argument, NULL, 'f'},
		{0, 0, 0, 0}
	};

	while (1) {
		/* It's "", not NULL */
		c = getopt_long(argc, argv, "", opts, NULL);
		if (-1 == c)
			break;

		switch (c) {
		case 'a':
			block_size = strtoul(optarg, NULL, 10);
			break;
		case 'b':
			device_list = optarg;
			break;
		case 'c':
			dir = strdup(optarg);
			break;
		case 'd':
			rd = optarg;
			break;
		case 'e':
			nr_block = strtoul(optarg, NULL, 10);
			break;
		case 'f':
			print_hlp_msg();
			exit(0);
		default:
			exit(-1);
		}
	}
	if (argv[optind]) {
		printf("Error:\toperand is not needed\n");
		exit(-1);
	}

	do_some_check();
}

/* the maximum number of device files */
#define MAX_DEV	32

struct mario_dir_entry devs[MAX_DEV + 1];

#define MKDEV(major, minor) ((((major) << 8) | (minor)) & 0x0000ffffUL)

void parse_device_list(void)
{
	int i, j;
	FILE *file;

	if (!device_list)
		return;
	for (i = 0; i < MAX_DEV; i++)
		devs[i].data = 0;

	if (!(file = fopen(device_list, "r"))) {
		printf("Error:\tfail to open %s\n", device_list);
		exit(-1);
	}

	i = j = 0;
	while (i < MAX_DEV) {
		int c;
		uint32_t mode, major, minor;

		c = fscanf(file, "%s%u%u%u", devs[i].name, &mode, &major, &minor);
		/* end of file? */
		if (EOF == c)
			break;
		/* empty line? */
		if (0 == c)
			continue;
		/* syntax error? */
		if (4 != c) {
			printf("Error:\tincorrect syntax in line %d at %s\n",
					i+1, device_list);
			exit(-1);
		}

		if (mode != MODE_BLK && mode != MODE_CHR) {
			printf("Error:\t%d is not valid device file mode\n", mode);
			exit(-1);
		}

		devs[i].mode = mode;
		devs[i].data = MKDEV(major, minor);
		devs[i].flags = 7;

		i++;
	}
	if (MAX_DEV == i)
		printf("Warning:\tToo many devices defined in %s\n", device_list);

	fclose(file);
}

FILE *rd_file;
int blocks;	/* the number of blocks written to rd */

char *buffer;	/* a buffer of size @block_size */
uint32_t used;	/* the number of bytes used in the buffer */

/* the maximum value of @used */
#define MAX_USED	(block_size - 4)

/* new a block to write to rd, it could also start a new pour */
#define new_block() \
do { \
	used = 0; \
	memset(buffer, 0, block_size); \
} while (0)

/* write a block into rd */
#define write_block() \
do { \
	fwrite(buffer, block_size, 1, rd_file); \
	blocks++; \
} while (0)

/*
 * pour data into rd. @size can not be bigger than MAX_USED.
 * If @bank is not 0, the whole @data will start at a new block
 * when the current block can not hold it.
 */
void pour_data(void *data, size_t size, int bank)
{
	int n = MAX_USED - used;

	if (size > n) {
		if (!bank) {
			memcpy(buffer + used, data, n);
			size -= n;
			data = (char *)data + n;
		}
		/* Set the last 4 bytes of buffer to the next block number */
		*(uint32_t *)(buffer + MAX_USED) = blocks + 1;
		write_block();
		new_block();
		pour_data(data, size, bank);
	} else {
		memcpy(buffer + used, data, size);
		used += size;
	}
}

/*
 * Don't forget to end the current pour when it's done
 */
void end_pour(void)
{
	if (used)
		write_block();
}

struct mario_super_block sb;

void make_begin(void)
{
	buffer = (char *)malloc(block_size);
	if (!buffer) {
		printf("Error:\tfail to allocate memory for buffer\n");
		exit(-1);
	}

	rd_file = fopen(rd, "wb+");
	if (!rd_file) {
		printf("Error:\tfail to open/create %s\n", rd);
		exit(-1);
	}

	blocks = 0;

	/*
	 * write super block into rd
	 */
	memset(&sb, 0, sizeof(sb));
	sb.sector_size = 512;
	sb.block_size = block_size;
	sb.magic = MARIO_MAGIC;

	sb.root.mode = MODE_DIR;
	sb.root.flags = 7;
	sb.root.size = 0;
	sb.root.blocks = 0;
	strcpy(sb.root.name, "mario");

	memset(buffer, 0, block_size);
	memcpy(buffer, &sb, sizeof(sb));
	write_block();
}

/* Copy directory entries into rd */
void pass1(void)
{
	DIR *dir;
	struct dirent *entry;

	dir = opendir(".");
	if (!dir) {
		printf("Error:\tfail to open dir .\n");
		exit(-1);
	}

	while ((entry = readdir(dir))) {
		struct stat st;
		struct mario_dir_entry tmp;

		if (strlen(entry->d_name) > MAX_NAME_LEN - 1) {
			printf("Error:\t\"%s\" is a really long name\n",
				entry->d_name);
			exit(-1);
		}

		stat(entry->d_name, &st);
		if (S_ISDIR(st.st_mode)) {
			tmp.mode = MODE_DIR;
			tmp.size = 0;
			tmp.blocks = 0;
		} else if (S_ISREG(st.st_mode)) {
			tmp.mode = MODE_REG;
			tmp.size = st.st_size;
			tmp.blocks = (tmp.size + MAX_USED - 1) / MAX_USED;
		} else {
			continue;
		}

		tmp.flags = 7;
		memset(tmp.name, 0, MAX_NAME_LEN);
		strcpy(tmp.name, entry->d_name);
		pour_data(&tmp, sizeof(tmp), 1);
	}
	closedir(dir);
	end_pour();
}

/*
 * Read a mario_dir_entry
 * @dent_offset: offset into rd of that directory entry
 * NOTE:
 *   This function doesn't change the file position
 */
void read_dir_entry(uint32_t dent_offset, struct mario_dir_entry *entry)
{
	long int pos;

	/* save file positon */
	pos = ftell(rd_file);
	/* read that mario_dir_entry */
	fseek(rd_file, dent_offset, SEEK_SET);
	fread(entry, sizeof(*entry), 1, rd_file);
	/* restore file position */
	fseek(rd_file, pos, SEEK_SET);
}

/*
 * Update a mario_dir_entry which has been written to rd
 * @dent_offset: offset into rd of that directory entry
 * NOTE:
 *   This function doesn't change the file position
 */
void update_mario_dir_entry(uint32_t dent_offset, uint32_t data)
{
	long int pos;
	struct mario_dir_entry entry;

	/* save file positon */
	pos = ftell(rd_file);
	/* read that mario_dir_entry */
	fseek(rd_file, dent_offset, SEEK_SET);
	fread(&entry, sizeof(entry), 1, rd_file);
	/* update it */
	entry.data = data;
	/* write it back */
	fseek(rd_file, dent_offset, SEEK_SET);
	fwrite(&entry, sizeof(entry), 1, rd_file);
	/* restore file positon */
	fseek(rd_file, pos, SEEK_SET);
}

/*
 * Copy a file into rd and update directory entry of that file
 * @dent_offset: offset into rd of that directory entry
 */
void copy_file(const char *name, uint32_t dent_offset)
{
	FILE *file;
	char box[256];

	/* used to update the directory entry */
	uint32_t __data;

	if (!(file = fopen(name, "rb"))) {
		printf("Error:\tfail to open %s\n", name);
		exit(-1);
	}

	new_block();
	fseek(rd_file, 0, SEEK_END);

	__data = blocks;
	update_mario_dir_entry(dent_offset, __data);

	while (1) {
		int n = fread(box, 1, 256, file);
		pour_data(box, n, 0);
		/* end of file? */
		if (n < 256)
			break;
	}
	fclose(file);
	end_pour();
}

void copy_dir(char *dirname, uint32_t dent_offset, uint32_t parent_dent_offset);

/*
 * Scan directory entries which have been written into rd for
 * FILE and DIR to copy into rd.
 * @offset: offset into rd of that directory
 * @dent: offset into rd of that directory entry
 * @parent_dent: offset into rd of parent directory entry
 */
void pass2(uint32_t offset, uint32_t dent_offset, uint32_t parent_dent_offset)
{
	int i, n;
	struct mario_dir_entry entry;

	/* maximum mario_dir_entries a block can contain */
	n = MAX_USED / sizeof(struct mario_dir_entry);

scan_a_new_block:
	for (i = 0; i < n; i++) {
		uint32_t dent;

		dent = offset + i*sizeof(entry);
		read_dir_entry(dent, &entry);
		/* end of this directory */
		if (!entry.name[0])
			return;
		if (entry.mode == MODE_REG) {
			if (entry.size)	/* not empty */
				copy_file(entry.name, dent);
			else
				update_mario_dir_entry(dent, MARIO_ZERO_ENTRY);
		}

		if (entry.mode == MODE_DIR) {
			struct mario_dir_entry tmp;

			if (!strcmp(".", entry.name)) {
				read_dir_entry(dent_offset, &tmp);
				update_mario_dir_entry(dent, tmp.data);
			} else if (!strcmp("..", entry.name)) {
				read_dir_entry(parent_dent_offset, &tmp);
				update_mario_dir_entry(dent, tmp.data);
			} else {
				copy_dir(entry.name, dent, dent_offset);
			}
		}
	}
	fseek(rd_file, offset + MAX_USED, SEEK_SET);
	fread(&offset, 4, 1, rd_file);
	if (offset) {
		offset *= block_size;
		goto scan_a_new_block;
	}
}

/*
 * Copy a directory recursively into rd.
 * @dirname: the pathname of the directory to be copied
 * @dent: offset into rd of the corresponding directory entry
 * @parent_dent: offset into rd of the parent directory entry
 */
void copy_dir(char *dirname, uint32_t dent_offset, uint32_t parent_dent_offset)
{
	/* Used to update the directory entry */
	uint32_t __data;

	if (chdir(dirname)) {
		printf("Error:\tfail to cd %s\n", dirname);
		exit(-1);
	}
	printf(">>> cd %s\n", dirname);

	__data = blocks;
	update_mario_dir_entry(dent_offset, __data);

	new_block();
	fseek(rd_file, 0, SEEK_END);

	pass1();
	pass2(__data*block_size, dent_offset, parent_dent_offset);

	/* Is it the root directory? */
	if (dent_offset < block_size)
		return;

	if (chdir("..")) {
		printf("Error:\tfail to cd ..\n");
		exit(-1);
	} else {
		printf(">>> cd ..\n");
	}
}

/* Add device files into rd */
void add_devices(void)
{
	int i, n;
	uint32_t tmp;
	uint32_t dent;
	uint32_t offset;
	struct mario_dir_entry entry;

	if (!device_list)
		return;
	n = MAX_USED / sizeof(struct mario_dir_entry);
	offset = MARIO_ROOT * block_size;
scan_a_new_block:
	for (i = 0; i < n; i++) {
		dent = offset + i*sizeof(entry);
		read_dir_entry(dent, &entry);
		/* end of that directory? */
		if (!entry.data)
			goto add_dev;
		if (!strcmp(entry.name, "dev")) {
			printf("Error:\t/dev already exists\n");
			exit(-1);
		}
	}
	fseek(rd_file, offset + MAX_USED, SEEK_SET);
	fread(&tmp, 4, 1, rd_file);
	if (tmp) {
		offset = tmp * block_size;
		goto scan_a_new_block;
	}
	/* expand the root directory */
	tmp = blocks;
	dent = tmp * block_size;
	fseek(rd_file, offset + MAX_USED, SEEK_SET);
	fwrite(&tmp, 4, 1, rd_file);
	fseek(rd_file, 0, SEEK_END);
	new_block();
	write_block();

add_dev:
	/* add dev/ into / */
	printf("Add /dev/\n");
	entry.mode = MODE_DIR;
	entry.flags = 7;
	entry.data = blocks;
	entry.size = 0;
	entry.blocks = 0;
	strcpy(entry.name, "dev");
	fseek(rd_file, dent, SEEK_SET);
	fwrite(&entry, sizeof(entry), 1, rd_file);

	/* add . and .. into /dev/ */
	new_block();
	fseek(rd_file, 0, SEEK_END);
	strcpy(entry.name, ".");
	pour_data(&entry, sizeof(entry), 1);
	entry.data = MARIO_ROOT;
	strcpy(entry.name, "..");
	pour_data(&entry, sizeof(entry), 1);

	/* add device into /dev/ */
	for (i = 0; i < MAX_DEV && devs[i].data; i++) {
		printf("Add /dev/%s\n", devs[i].name);
		pour_data(&devs[i], sizeof(struct mario_dir_entry), 1);
	}
	end_pour();
}

#define MIN(x, y) ((x) < (y) ? (x) : (y))

/* Decide value of @nr_block */
void decide_nr_block(void)
{
	uint32_t max_nr_block;

	if (nr_block) {
		if (nr_block >= blocks)
			return;
		printf("Warning:\tNR you gave is too small\n");
	}

	max_nr_block = MAX_RD_SIZE / block_size;

	if (blocks > max_nr_block/2)
		nr_block = MIN(blocks + max_nr_block/2, max_nr_block);
	else if (blocks > max_nr_block/4)
		nr_block = blocks + max_nr_block/4;
	else if (blocks > max_nr_block/8)
		nr_block = blocks + max_nr_block/8;
	else if (blocks > max_nr_block/16)
		nr_block = blocks + max_nr_block/16;
	else if (blocks > max_nr_block/32)
		nr_block = blocks + max_nr_block/32;
	else if (blocks > max_nr_block/64)
		nr_block = blocks + max_nr_block/64;
	else
		nr_block = blocks + max_nr_block/128;
}

void truncate_rd(void)
{
	decide_nr_block();

	sb.nr_blocks = nr_block;
	sb.nr_free = nr_block - blocks;
	if (!sb.nr_free)
		return;
	sb.free = blocks;

	new_block();
	while (blocks < nr_block) {
		*(uint32_t *)(buffer + MAX_USED) = (blocks + 1) % nr_block;
		write_block();
	}
}

void update_super_block(void)
{
	struct mario_super_block tmp;

	printf("super block:\n");
	printf("    sector_size=%u\n", sb.sector_size);
	printf("    block_size=%u\n", sb.block_size);
	printf("    nr_blocks=%u\n", sb.nr_blocks);
	printf("    nr_free=%u\n", sb.nr_free);
	printf("    free=%u\n", sb.free);

	fseek(rd_file, 0, SEEK_SET);
	fread(&tmp, sizeof(tmp), 1, rd_file);
	tmp.nr_blocks = sb.nr_blocks;
	tmp.nr_free = sb.nr_free;
	tmp.free = sb.free;
	fseek(rd_file, 0, SEEK_SET);
	fwrite(&tmp, sizeof(tmp), 1, rd_file);
}

void make_end(void)
{
	if (block_size*blocks > MAX_RD_SIZE) {
		printf("Error:\tthe rd we just made is too big\n");
		exit(-1);
	}
	add_devices();
	truncate_rd();
	update_super_block();

	free(buffer);
	fclose(rd_file);
}

int main(int argc, char *argv[])
{
	uint32_t dent_offset;

	parse_opt(argc, argv);
	parse_device_list();

	make_begin();
	dent_offset = (unsigned long)&sb.root - (unsigned long)&sb;
	copy_dir(dir, dent_offset, dent_offset);
	make_end();
	return 0;
}

