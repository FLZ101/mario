#ifndef _E820_H
#define _E820_H

#include <multiboot.h>
#include <types.h>

#define E820MAX	32

#define E820_RAM	1

struct e820map {
    __u32 nr_map;
    struct e820entry {
	__u64 addr;
	__u64 len;
	__u32 type;
    } map[E820MAX];
};

extern struct e820map e820;

void setup_memory_region(struct multiboot_info *m);

#endif	/* _E820_H */