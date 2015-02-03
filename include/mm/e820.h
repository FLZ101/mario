#ifndef _E820_H
#define _E820_H

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

#endif	/* _E820_H */