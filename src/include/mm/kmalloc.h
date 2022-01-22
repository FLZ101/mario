#ifndef _KMALLOC_H
#define _KMALLOC_H

#include <lib/stddef.h>

void *kmalloc(size_t size);
void kfree(void *ptr);

#endif /* _KMALLOC_H */