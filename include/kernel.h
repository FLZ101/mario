#ifndef _KERNEL_H
#define _KERNEL_H

#define barrier() __asm__ __volatile__("": : :"memory")

#define INT_MAX		((int)(~0U>>1))
#define INT_MIN		(-INT_MAX - 1)
#define UINT_MAX	(~0U)
#define LONG_MAX	((long)(~0UL>>1))
#define LONG_MIN	(-LONG_MAX - 1)
#define ULONG_MAX	(~0UL)

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/*
 * Mark a function (variable) as being only used at initialization time
 */
#define __tinit __attribute__ ((section (".tinit")))
#define __dinit __attribute__ ((section (".dinit")))

/*
 * early_print.c
 */
void cls(void);
void set_pos(int x, int y);
void early_print(const char *fmt, ...);



#endif	/* _KERNEL_H */
