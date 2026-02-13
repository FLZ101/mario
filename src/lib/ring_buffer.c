#include <lib/ring_buffer.h>

#include <lib/stddef.h>
#include <lib/string.h>
#include <mm/uaccess.h>
#include <mm/page_alloc.h>
#include <errno.h>

int ring_buffer_init(struct ring_buffer *rb) {
	rb->data = (char *) page_alloc();
	if (!rb->data)
		return -ENOMEM;
	rb->size = PAGE_SIZE;
	rb->head = 0;
	rb->tail = 0;
	rb->len = 0;
	return 0;
}

void ring_buffer_destroy(struct ring_buffer *rb) {
	if (rb->data) {
		page_free((unsigned long)rb->data);
		rb->data = (char *) NULL;
	}
}

// Return number of bytes available for writing
size_t ring_buffer_space(const struct ring_buffer *rb) {
	return rb->size - rb->len;
}

// Return number of bytes available for reading
size_t ring_buffer_avail(const struct ring_buffer *rb) {
	return rb->len;
}

// Write up to 'count' bytes (non-blocking)
// Returns number of bytes written
size_t ring_buffer_write(struct ring_buffer *rb, const void *buf, size_t count, int u) {
	if (!rb->data || count == 0)
		return 0;
	size_t can_write = MIN(count, ring_buffer_space(rb));
	if (can_write == 0)
		return 0;

	const char *src = (const char *)buf;
	size_t mask = rb->size - 1;
	size_t chunk1 = (rb->head + can_write <= rb->size) ? can_write : rb->size - rb->head;

#define COPY(dst, src, num) do { \
	if (u) \
		memcpy_fromfs((dst), (src), (num)); \
	else \
		memcpy((dst), (src), (num)); \
} while (0)

	COPY(&rb->data[rb->head], src, chunk1);
	if (can_write > chunk1)
		COPY(rb->data, src + chunk1, can_write - chunk1);

#undef COPY

	rb->head = (rb->head + can_write) & mask;
	rb->len += can_write;
	return can_write;
}

// Read up to 'count' bytes (non-blocking)
// Returns number of bytes read
size_t ring_buffer_read(struct ring_buffer *rb, void *buf, size_t count, int u) {
	if (!rb->data || count == 0)
		return 0;
	size_t can_read = MIN(count, ring_buffer_avail(rb));
	if (can_read == 0)
		return 0;

	char *dst = (char *) buf;
	size_t mask = rb->size - 1;
	size_t chunk1 = (rb->tail + can_read <= rb->size) ? can_read : rb->size - rb->tail;

#define COPY(dst, src, num) do { \
	if (u) \
		memcpy_tofs((dst), (src), (num)); \
	else \
		memcpy((dst), (src), (num)); \
} while (0)

	COPY(dst, &rb->data[rb->tail], chunk1);
	if (can_read > chunk1)
		COPY(dst + chunk1, rb->data, can_read - chunk1);

#undef COPY

	rb->tail = (rb->tail + can_read) & mask;
	rb->len -= can_read;
	return can_read;
}

int ring_buffer_empty(const struct ring_buffer *rb) {
	return rb->len == 0;
}

int ring_buffer_full(const struct ring_buffer *rb) {
	return rb->len == rb->size;
}

// pop last written byte
void ring_buffer_pop(struct ring_buffer *rb) {
	if (!ring_buffer_empty(rb)) {
		size_t mask = rb->size - 1;
		rb->head = (rb->head - 1) & mask;
		--rb->len;
	}
}
