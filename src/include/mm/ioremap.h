#ifndef _IOREMAP_H
#define _IOREMAP_H

#include <types.h>

/* Map a physical MMIO range and provide naturally-sized MMIO load/store helpers. */
void *ioremap(unsigned long phys, size_t size);
void iounmap(void *virt);

static inline uint32_t readl(const volatile void *addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void writel(uint32_t value, volatile void *addr)
{
	*(volatile uint32_t *)addr = value;
}

#endif /* _IOREMAP_H */
