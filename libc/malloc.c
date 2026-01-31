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

static block free_list_head = {NULL, NULL, 0};

#ifndef NDEBUG

#include <stdio.h>

void print_free_list()
{
	block *p = &free_list_head;
	printf("free_list:\n");
	while (p->next) {
		p = p->next;
		printf("%x, %x, %x\n", p, p->data, p->size);
	}
}
#endif

static block *get_new_block(block *prev, unsigned size)
{
	if (size < MIN_ALLOC_SIZE)
		size = MIN_ALLOC_SIZE;

	block *res = sbrk(BLOCK_META_SIZE + size);
	if (res == (void *)-1)
		return NULL;

	prev->next = res;

	res->prev = prev;
	res->next = NULL;
	res->size = size;
	return res;
}

static block *get_free_block(unsigned size)
{
	if (!size)
		return NULL;

	block *p = &free_list_head;
	while (p->next) {
		p = p->next;
		if (p->size >= size)
			break;
	}

	if (p->size < size)
		p = get_new_block(p, size);

	p->prev->next = p->next;
	if (p->next)
		p->next->prev = p->prev;

	return p;
}

void merge_free_blocks(block *p)
{
	if (p == &free_list_head)
		p = p->next;
	if (!p)
		return;

	while (p->data + p->size == (void *)p->next) {
		block *q = p->next;
		p->size += BLOCK_META_SIZE + q->size;

		p->next = q->next;
		if (q->next)
			q->next->prev = p;
	}

	if (sbrk(0) == p->data + p->size) {
		assert(!p->next);

		sbrk(-(BLOCK_META_SIZE + p->size));
		p->prev->next = NULL;
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

	merge_free_blocks(b->prev);
}

static void zero_block(block *p) { memset(p->data, 0, p->size); }

void *malloc(unsigned size)
{
	block *p = get_free_block(size);
	if (!p)
		return NULL;
	zero_block(p);
	return p->data;
}

void *calloc(unsigned num, unsigned size)
{
	void *p = malloc(num * size);
	if (!p)
		return NULL;
	memset(p, 0, num * size);
	return p;
}

void *realloc(void *ptr, unsigned new_size)
{
	block *b = (block *)(ptr - BLOCK_META_SIZE);
	if (b->size >= new_size) {
		if (b->size - new_size >= BLOCK_META_SIZE + MIN_ALLOC_SIZE) {
			block *b2 = (block *)(b->data + new_size);
			b2->size = b->size - new_size - BLOCK_META_SIZE;
			b->size = new_size;
			put_free_block(b2);
		}
		return ptr;
	}

	block *p = get_free_block(new_size);
	if (!p)
		return NULL;
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
