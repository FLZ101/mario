#ifndef _PERCPU_H
#define _PERCPU_H

#include <smp.h>

/* Single-CPU per-CPU accessor until APs have their own cpu_info slots. */
static inline struct cpu_info *this_cpu_info(void)
{
	return &bsp_cpu_info;
}

#endif	/* _PERCPU_H */
