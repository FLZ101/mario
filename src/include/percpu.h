#ifndef _PERCPU_H
#define _PERCPU_H

#include <smp.h>

/* BSP-only accessor until scheduler work makes this CPU-local in Step 6. */
static inline struct cpu_info *this_cpu_info(void)
{
	return &cpu_info[0];
}

#endif	/* _PERCPU_H */
