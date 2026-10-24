#include <bh.h>

#include <lib/stddef.h>

unsigned long bh_active = 0;
unsigned long bh_mask = 0;
struct bh_struct bh_base[32];

void do_bottom_half(void)
{
	unsigned long active = bh_active & bh_mask;
	unsigned long mask = 1, left = ~0;
	struct bh_struct *bh = bh_base;
	int bit = 0;

	for (; left & active; bh++, mask += mask, left += left, bit++) {
		if (mask & active) {
			void (*fn)(void *);
			/* Clear the active bit atomically: another CPU can mark
			 * the same bottom half while we are running it. */
			test_and_clear_bit(bit, &bh_active);
			fn = bh->routine;
			if (fn)
				fn(bh->data);
		}
	}
}
