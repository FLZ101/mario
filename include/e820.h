#ifndef _E820_H
#define _E820_H

#include <types.h>

#define E820_RAM	1

#define E820MAX	32

#define FAKE_LOWER_MEM	0x080000	/* 512 KB */
#define FAKE_UPPER_MEM	0xe00000	/* 14 MB */

/*
 * Maximum physical memory supported
 */
#define MAX_MEMORY  0x80000000ULL
 
struct e820map {
    __u32 nr_map;
    struct e820entry {
	__u64 addr;
	__u64 len;
	__u32 type;
    } map[E820MAX];
};

extern struct e820map e820;

void make_e820_map(struct multiboot_info *m);

#endif	/* _E820_H */
