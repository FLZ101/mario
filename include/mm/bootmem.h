#ifndef _BOOTMEM_H
#define _BOOTMEM_H

void *__alloc_bootmem(unsigned long size, unsigned long p2align);
void *alloc_bootmem(unsigned long size);
void *alloc_page_bootmem(void);

#endif	/* _BOOTMEM_H */