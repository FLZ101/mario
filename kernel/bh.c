#include <bh.h>

unsigned long bh_active = 0;
unsigned long bh_mask = 0;
struct bh_struct bh_base[32];

void do_bottom_half(void)
{
	unsigned long active = bh_active & bh_mask;
	unsigned long mask = 1, left = ~0;
	struct bh_struct *bh = bh_base;

	for (; left & active; bh++, mask += mask, left += left) {
		if (mask & active) {
			void (*fn)(void *);
			bh_active &= ~mask;
			fn = bh->routine;
			if (fn)
				fn(bh->data);
		}
	}
}