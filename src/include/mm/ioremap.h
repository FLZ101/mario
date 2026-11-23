#ifndef _IOREMAP_H
#define _IOREMAP_H

#include <types.h>

/* One PDE below the fixed ACPI and LAPIC mappings, outside the RAM direct map. */
#define IOREMAP_BASE 0xff400000UL

/* Map/unmap only on the boot CPU before starting any AP or task. */
void *ioremap(unsigned long phys, size_t size);
void iounmap(void *virt);
void ioremap_seal(void);

static inline uint8_t readb(const volatile void *addr)
{
	return *(volatile uint8_t *)addr;
}

static inline uint16_t readw(const volatile void *addr)
{
	return *(volatile uint16_t *)addr;
}

static inline uint32_t readl(const volatile void *addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void writeb(uint8_t value, volatile void *addr)
{
	*(volatile uint8_t *)addr = value;
}

static inline void writew(uint16_t value, volatile void *addr)
{
	*(volatile uint16_t *)addr = value;
}

static inline void writel(uint32_t value, volatile void *addr)
{
	*(volatile uint32_t *)addr = value;
}

#endif /* _IOREMAP_H */
