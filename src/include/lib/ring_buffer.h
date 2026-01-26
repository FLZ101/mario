#ifndef _RING_BUFFER_H
#define _RING_BUFFER_H

#include <types.h>

struct ring_buffer {
	char *data; 	// dynamically allocated buffer
	size_t size;	// total capacity (must be power of two)
	size_t head;	// index of next write
	size_t tail;	// index of next read
	size_t len; 	// number of bytes currently stored
};

int ring_buffer_init(struct ring_buffer *rb);
void ring_buffer_destroy(struct ring_buffer *rb);

size_t ring_buffer_space(const struct ring_buffer *rb);
size_t ring_buffer_avail(const struct ring_buffer *rb);

size_t ring_buffer_write(struct ring_buffer *rb, const void *buf, size_t count, int u);
size_t ring_buffer_read(struct ring_buffer *rb, void *buf, size_t count, int u);

int ring_buffer_empty(const struct ring_buffer *rb);
int ring_buffer_full(const struct ring_buffer *rb);

#endif // _RING_BUFFER_H
