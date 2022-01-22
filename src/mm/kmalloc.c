#include <misc.h>

#include <mm/page_alloc.h>
#include <mm/kmalloc.h>

struct board_header;
struct block_header {
	/* the board this block belongs to */
	struct board_header *board;
	/* the next free block in a board */
	struct block_header *next;
} __attribute__((gcc_struct, packed));

struct bucket;
struct board_header {
	/* the number of free block(s) in this board */
	unsigned long nfree;
	/* the first free block in this board */
	struct block_header *firstfree;
	/* the bucket this board belongs to */
	struct bucket_desc *bucket;
	/* the next free board in a bucket */
	struct board_header *next;
} __attribute__((gcc_struct, packed));

struct bucket_desc {
	/* the first free board in this bucket */
	struct board_header *firstfree;
	/* a board in this bucket is contiguous 2^order page(s) */
	unsigned long order;
	/* size of a block in this bucket */
	unsigned long block_size;
	/* the number of blocks a board in this bucket contains */
	unsigned long nblocks;
};

struct bucket_desc buckets[] = {
	{NULL, 0, 34, 120},
	{NULL, 0, 60, 68},
	{NULL, 0, 120, 34},
	{NULL, 0, 255, 16},
	{NULL, 0, 510, 8},
	{NULL, 0, 1020, 4},
	{NULL, 0, 2040, 2},
	{NULL, 0, 4096-16, 1},
	{NULL, 1, 8192-16, 1},
	{NULL, 2, 16384-16, 1},
	{NULL, 3, 32768-16, 1},
	{NULL, 4, 65536-16, 1},
	{NULL, 5, 131072-16, 1},
	{NULL, 6, 262144-16, 1},
	{NULL, 0, 0, 0}
};

struct board_header *alloc_board(struct bucket_desc *bucket)
{
	struct board_header *board =
		(struct board_header *)pages_alloc(bucket->order);
	if (!board)
		return NULL;

	struct block_header *block = (struct block_header *)(board + 1);

	int i;
	for (i = bucket->nblocks - 1; i > 0; i--) {
		block->board = board;
		block->next =
			(struct block_header *)
				((unsigned long)block + bucket->block_size);
		block = block->next;
	}
	block->board = board;
	block->next = NULL;

	board->nfree = bucket->nblocks;
	board->firstfree = (struct block_header *)(board + 1);
	board->bucket = bucket;
	board->next = bucket->firstfree;
	return board;
}

void free_board(struct board_header *board)
{
	pages_free((unsigned long)board, board->bucket->order);
}

void *kmalloc(size_t size)
{
	irq_save();

	if (!size)
		goto fail;

	struct bucket_desc *bucket;
	for (bucket = buckets; bucket->block_size; bucket++)
		if (bucket->block_size - 4 >= size)
			break;
	if (!bucket->block_size)
		goto fail;

	if (!bucket->firstfree)
		bucket->firstfree = alloc_board(bucket);
	if (!bucket->firstfree)
		goto fail;

	struct board_header *board = bucket->firstfree;
	struct block_header *block = board->firstfree;

	if (!--board->nfree)
		bucket->firstfree = board->next;

	board->firstfree = block->next;

	irq_restore();
	return &block->next;
fail:
	irq_restore();
	return NULL;
}

void kfree(void *ptr)
{
	irq_save();

	struct block_header *block =
		(struct block_header *)((unsigned long)ptr - 4);
	struct board_header *board = block->board;
	struct bucket_desc *bucket = board->bucket;

	block->next = board->firstfree;
	board->firstfree = block;

	board->nfree++;
	if (board->nfree == 1) {
		board->next = bucket->firstfree;
		bucket->firstfree = board;
	}
	if (board->nfree == bucket->nblocks) {
		bucket->firstfree = board->next;
		free_board(board);
	}

	irq_restore();
}
