#ifndef _MM_H
#define _MM_H

#define KERNEL_CS   0x10
#define KERNEL_DS   0x18

#define USER_CS     0x23
#define USER_DS     0x2b

/*
 * kernel space: 0 ~ 2G-1
 * user space: 2G ~ 4G-1
 */
#define USER_BASE	0x80000000UL	/* 2G */

#define PAGE_SHIFT	12
#define PAGE_SIZE	(1UL << PAGE_SHIFT)

#define PFN_UP(x)	(((x) + PAGE_SIZE-1) >> PAGE_SHIFT)
#define PFN_DOWN(x)	((x) >> PAGE_SHIFT)
#define PFN_PHYS(x)	((x) << PAGE_SHIFT)

#ifndef __ASSEMBLY__

#include <multiboot.h>

void setup_memory_region(struct multiboot_info *m);

void *__alloc_bootmem(unsigned long size, unsigned long p2align);
void *alloc_bootmem(unsigned long size);
void *alloc_page_bootmem(void);

#endif	/* __ASSEMBLY__ */

#endif	/* _MM_H */