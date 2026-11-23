#ifndef _X87_H
#define _X87_H

#include <types.h>

#define XSAVE_AREA_SIZE 1024

struct x87_state {
	uint32_t control, status, tag;
	uint32_t ip, cs_opcode, data_offset, ds;
	unsigned char registers[80];
};

struct fpu_state {
	unsigned char data[XSAVE_AREA_SIZE];
} __attribute__((aligned(64)));

void x87_init_cpu(void);
void x87_reset(void);
void fpu_init_cpu(void);
void fpu_reset(void);

extern int fpu_xsave_enabled;

static inline void x87_save(struct x87_state *state)
{
	asm volatile("fnsave %0" : "=m"(*state));
}

static inline void x87_restore(const struct x87_state *state)
{
	asm volatile("fnclex; frstor %0" : : "m"(*state));
}

static inline void fpu_save(struct fpu_state *state)
{
	if (fpu_xsave_enabled)
		asm volatile("xsave %0" : "=m"(*state)
			: "a"(0x7), "d"(0) : "memory");
	else
		asm volatile("fxsave %0" : "=m"(*state) : : "memory");
}

static inline void fpu_restore(const struct fpu_state *state)
{
	if (fpu_xsave_enabled)
		asm volatile("xrstor %0" : : "m"(*state),
			"a"(0x7), "d"(0) : "memory");
	else
		asm volatile("fxrstor %0" : : "m"(*state) : "memory");
}

#endif
