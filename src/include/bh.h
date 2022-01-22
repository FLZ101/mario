#ifndef _BH_H
#define _BH_H

#include <lib/bitops.h>

struct bh_struct {
	void (*routine)(void *);
	void *data;
};

extern unsigned long bh_active;
extern unsigned long bh_mask;
extern struct bh_struct bh_base[32];

#define PIT_BH		0
#define KEYBOARD_BH	1

static inline void mark_bh(int nr)
{
	set_bit(nr, &bh_active);
}

static inline void disable_bh(int nr)
{
	clear_bit(nr, &bh_mask);
}

static inline void enable_bh(int nr)
{
	set_bit(nr, &bh_mask);
}

#endif /* _BH_H */