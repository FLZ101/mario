#include <x87.h>

_Static_assert(sizeof(struct x87_state) == 108, "32-bit FSAVE image size");

static const struct x87_state initial_state = {
	.control = 0x037f,
	.tag = 0xffff,
};

#define CR0_MP (1UL << 1)
#define CR0_EM (1UL << 2)
#define CR0_TS (1UL << 3)
#define CR0_NE (1UL << 5)

void x87_reset(void)
{
	x87_restore(&initial_state);
}

void x87_init_cpu(void)
{
	unsigned long cr0;

	asm volatile("mov %%cr0, %0" : "=r"(cr0));
	cr0 = (cr0 | CR0_MP | CR0_NE) & ~(CR0_EM | CR0_TS);
	asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
	x87_reset();
}
