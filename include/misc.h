#ifndef _MISC_H
#define _MISC_H

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

#define PFN_UP(x)	(((unsigned long)(x) + PAGE_SIZE-1) >> PAGE_SHIFT)
#define PFN_DOWN(x)	((unsigned long)(x) >> PAGE_SHIFT)
#define PFN_PHYS(x)	((unsigned long)(x) << PAGE_SHIFT)

#define SYSCALL_VECTOR	0x80

#define ENTRY(name) \
 	.globl _##name; \
 	_##name##:

#ifndef __ASSEMBLY__

/*
 * Mark a function (variable) as being only used at initialization time
 */
#define __tinit __attribute__ ((section (".tinit")))
#define __dinit __attribute__ ((section (".dinit")))

#include <multiboot.h>

void early_print_init(struct multiboot_info *m);
void early_print(const char *fmt, ...);

#define early_hang(...) do {\
	early_print(__VA_ARGS__);\
	__asm__ __volatile__ ("cli; 1:hlt; jmp 1b");\
} while (0)

#define cli() __asm__ __volatile__ ("cli": : :"memory")
#define sti() __asm__ __volatile__ ("sti": : :"memory")

#endif	/* __ASSEMBLY__ */

#endif	/* _MISC_H */
