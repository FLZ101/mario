#ifndef _X87_H
#define _X87_H

#include <types.h>

struct x87_state {
	uint32_t control, status, tag;
	uint32_t ip, cs_opcode, data_offset, ds;
	unsigned char registers[80];
};

void x87_init_cpu(void);
void x87_reset(void);

static inline void x87_save(struct x87_state *state)
{
	asm volatile("fnsave %0" : "=m"(*state));
}

static inline void x87_restore(const struct x87_state *state)
{
	asm volatile("fnclex; frstor %0" : : "m"(*state));
}

#endif
