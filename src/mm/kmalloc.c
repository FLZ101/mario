#include <misc.h>

#include <mm/page_alloc.h>
#include <mm/kmalloc.h>
#include <lib/spinlock.h>
#include <lib/string.h>
#include <task.h>

/*
 * kmalloc bucket lock.  As with the buddy allocator, interrupts disabled on
 * one CPU are not enough once another CPU can allocate a block concurrently.
 * The kmalloc -> buddy lock order is intentional: kmalloc may allocate whole
 * boards, but the buddy allocator never calls back into kmalloc.
 */
static spinlock_t kmalloc_lock = SPINLOCK_UNLOCKED;

struct board_header;
struct block_header {
#ifdef CONFIG_KMALLOC_DEBUG
	uint32_t magic;
	int owner;
	size_t size;
#endif
	/* the board this block belongs to */
	struct board_header *board;
	/* the next free block in a board */
	struct block_header *next;
} __attribute__((gcc_struct, packed));

#ifdef CONFIG_KMALLOC_DEBUG
/* The allocator keeps the user pointer at &block->next in both layouts. */
#define KMALLOC_USER_OFFSET	__builtin_offsetof(struct block_header, next)
#define KMALLOC_REDZONE_SIZE	sizeof(uint32_t)
#define KMALLOC_MAGIC_ALLOC	0x6d6b616cU /* "mlak" */
#define KMALLOC_MAGIC_FREED	0x6d6b6672U /* "mfrk" */
#define KMALLOC_REDZONE_VALUE	0x7265647aU /* "redz" */
#else
#define KMALLOC_USER_OFFSET	4
#define KMALLOC_REDZONE_SIZE	0
#endif

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
	unsigned long flags;

	spin_lock_irqsave(&kmalloc_lock, flags);

	if (!size)
		goto fail;

	struct bucket_desc *bucket;
	for (bucket = buckets; bucket->block_size; bucket++)
		if (bucket->block_size - KMALLOC_USER_OFFSET >=
		    size + KMALLOC_REDZONE_SIZE)
			break;
	if (!bucket->block_size)
		goto fail;

	if (!bucket->firstfree)
		bucket->firstfree = alloc_board(bucket);
	if (!bucket->firstfree)
		goto fail;

	struct board_header *board = bucket->firstfree;
	struct block_header *block = board->firstfree;

#ifdef CONFIG_KMALLOC_DEBUG
	block->magic = KMALLOC_MAGIC_ALLOC;
	block->owner = current->pid;
	block->size = size;
#endif

	if (!--board->nfree)
		bucket->firstfree = board->next;

	board->firstfree = block->next;

#ifdef CONFIG_KMALLOC_DEBUG
	*(uint32_t *)((unsigned long)&block->next + size) =
		KMALLOC_REDZONE_VALUE;
#endif

	spin_unlock_irqrestore(&kmalloc_lock, flags);
	return &block->next;
fail:
	spin_unlock_irqrestore(&kmalloc_lock, flags);
	pr_err("kmalloc size=%u failed\n", (unsigned int)size);
	return NULL;
}

void *kzalloc(size_t size)
{
	void *ptr = kmalloc(size);

	if (ptr)
		memset(ptr, 0, size);
	return ptr;
}

void kfree(void *ptr)
{
	unsigned long flags;

	spin_lock_irqsave(&kmalloc_lock, flags);

#ifdef CONFIG_KMALLOC_DEBUG
	assert(ptr);
	struct block_header *block =
		(struct block_header *)((unsigned long)ptr -
			KMALLOC_USER_OFFSET);
	assert(block->magic == KMALLOC_MAGIC_ALLOC);
	assert(*(uint32_t *)((unsigned long)ptr + block->size) ==
	       KMALLOC_REDZONE_VALUE);
	memset(ptr, 0xcc, block->size);
	block->magic = KMALLOC_MAGIC_FREED;
#else
	struct block_header *block =
		(struct block_header *)((unsigned long)ptr - 4);
#endif
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

	spin_unlock_irqrestore(&kmalloc_lock, flags);
}
