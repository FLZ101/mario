#ifndef _PAGING_H
#define _PAGING_H

#include <misc.h>

#include <mm/pagetable.h>

#define flush_tlb() \
asm volatile("movl %%cr3,%%eax; movl %%eax,%%cr3":::"eax")

void paging_init(void);

#endif /* _PAGING_H */