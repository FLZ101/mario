#ifndef _KMALLOC_H
#define _KMALLOC_H

#include <lib/stddef.h>
#include <types.h>

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);

#if defined(CONFIG_KMALLOC_DEBUG) && defined(CONFIG_KMALLOC_SELF_TEST)
void kmalloc_churn_selftest(void);
#endif

#endif /* _KMALLOC_H */
