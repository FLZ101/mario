#include <stdlib.h>
#include <unistd.h>

#include <assert.h>
#include <string.h>

typedef struct block_t {
	struct block_t *prev, *next;
	unsigned size;
	char data[0];
} block;

#define BLOCK_META_SIZE sizeof(block)

#define MIN_ALLOC_SIZE 16

// ordered by address
static block free_list_head = {NULL, NULL, 0};

#ifndef NDEBUG

#include <stdio.h>

void print_free_list()
{
	block *p = &free_list_head;
	printf("free_list:\n");
	while (p->next) {
		p = p->next;
		printf("%x, %x\n", p, p->size);
	}
}

void verify_free_list()
{
	block *p = &free_list_head;
	while (p->next) {
		block *q = p->next;
		assert(q->prev == p);
		assert(q->size >= MIN_ALLOC_SIZE);
		assert((void *) (q->data + q->size) <= sbrk(0));
		p = q;
	}
}

#else

void print_free_list() {}

void verify_free_list() {}

#endif

static block *get_new_block(unsigned size)
{
	block *res = sbrk(BLOCK_META_SIZE + size);
	if (res == (void *)-1)
		return NULL;
	assert(res->data + size == sbrk(0));

	res->prev = NULL;
	res->next = NULL;
	res->size = size;
	return res;
}

static void split_free_block(block *b, unsigned size)
{
	assert(b->size >= size);

	if (b->size - size >= BLOCK_META_SIZE + MIN_ALLOC_SIZE) {
		block *new = (block *)(b->data + size);
		new->size = b->size - size - BLOCK_META_SIZE;
		new->next = b->next;
		new->prev = b;

		if (b->next)
			b->next->prev = new;

		b->size = size;
		b->next = new;
	}
}

static block *get_free_block(unsigned size)
{
	if (!size)
		return NULL;

	block *p = free_list_head.next;
	unsigned min_diff =  (unsigned) -1;
	block *min_p;
	while (p) {
		if (p->size >= size) {
			// find a better fit
			if (p->size - size < min_diff) {
				min_diff = p->size - size;
				min_p = p;
			}
		}
		p = p->next;
	}

	// found a fit
	if (min_diff != (unsigned) -1) {
		p = min_p;

		split_free_block(p, size);

		p->prev->next = p->next;
		if (p->next)
			p->next->prev = p->prev;

		p->next = NULL;
		p->prev = NULL;

		verify_free_list();
		return p;
	}

	return get_new_block(size);
}

void merge_free_blocks()
{
	block *o = &free_list_head;
	while (o->next) {
		block *p = o->next;

		if (p->data + p->size == (void *) p->next) {
			block *q = p->next;
			p->size += BLOCK_META_SIZE + q->size;

			p->next = q->next;
			if (p->next)
				p->next->prev = p;
		} else {
			o = o->next;
		}
	}

	if (o->data + o->size == sbrk(0)) {
		// remove o from the list. MUST be done before we return memory occupied by o to os
		assert(o->prev && !o->next);
		o->prev->next = NULL;

		sbrk(-(BLOCK_META_SIZE + o->size));
	}
}

void put_free_block(block *b)
{
	block *p = &free_list_head;
	while (p < b && p->next)
		p = p->next;
	assert(p != b);
	if (p > b)
		p = p->prev;

	b->prev = p;
	b->next = p->next;
	if (p->next)
		p->next->prev = b;
	p->next = b;

	merge_free_blocks();

	verify_free_list();
}

static void zero_block(block *p) { memset(p->data, 0, p->size); }

void *malloc(unsigned size)
{
	if (!size)
		return NULL;

	if (size < MIN_ALLOC_SIZE)
		size = MIN_ALLOC_SIZE;

	block *p = get_free_block(size);
	if (!p)
		return NULL;
	zero_block(p);
	return p->data;
}

void *calloc(unsigned num, unsigned size)
{
	return malloc(num * size);
}

static void split_block(block *b, unsigned size)
{
	assert(b->size >= size);

	if (b->size - size >= BLOCK_META_SIZE + MIN_ALLOC_SIZE) {
		block *new = (block *)(b->data + size);
		new->size = b->size - size - BLOCK_META_SIZE;
		new->next = NULL;
		new->prev = NULL;

		put_free_block(new);

		b->size = size;
	}
}

void *realloc(void *ptr, unsigned new_size)
{
	if (!ptr)
		return malloc(new_size);

	if (!new_size) {
		free(ptr);
		return NULL;
	}

	if (new_size < MIN_ALLOC_SIZE)
		new_size = MIN_ALLOC_SIZE;

	block *b = (block *)(ptr - BLOCK_META_SIZE);
	if (b->size >= new_size) {
		split_block(b, new_size);
		return ptr;
	}

	block *p = get_free_block(new_size);
	if (!p)
		return NULL;
	zero_block(p);
	memcpy(p->data, b->data, b->size);

	put_free_block(b);

	return p->data;
}

void free(void *ptr) {
	if (!ptr)
		return;
	block *b = (block *)(ptr - BLOCK_META_SIZE);
	put_free_block(b);
}
