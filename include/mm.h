#ifndef _MM_H
#define _MM_H

#define KERNEL_CS   0x10
#define KERNEL_DS   0x18

#define USER_CS     0x23
#define USER_DS     0x2b

#ifndef __ASSEMBLY__

#include <multiboot.h>

void mem_init(struct multiboot_info *m);

#endif	/* __ASSEMBLY__ */

#endif	/* _MM_H */