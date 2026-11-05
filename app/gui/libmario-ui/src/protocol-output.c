#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <gui/protocol.h>

typedef struct gui_protocol_output_item {
	struct gui_protocol_output_item *next;
	uint8_t *bytes;
	size_t length;
	size_t offset;
	int motion;
} gui_protocol_output_item;

struct gui_protocol_output {
	gui_protocol_output_item *head;
	gui_protocol_output_item *tail;
	size_t byte_limit;
	size_t byte_count;
};

static int gui_protocol_output_message_valid(const void *message,
					     size_t byte_count)
{
	gui_protocol_header header;

	if (message == NULL) {
		errno = EINVAL;
		return 0;
	}
	if (gui_protocol_decode_header(message, byte_count, &header) < 0)
		return 0;
	if (header.length != byte_count) {
		errno = EPROTO;
		return 0;
	}
	return 1;
}

static gui_protocol_output_item *gui_protocol_output_item_create(
	const void *message, size_t byte_count, int motion)
{
	gui_protocol_output_item *item = calloc(1, sizeof(*item));

	if (item == NULL)
		return NULL;
	item->bytes = malloc(byte_count);
	if (item->bytes == NULL) {
		free(item);
		return NULL;
	}
	memcpy(item->bytes, message, byte_count);
	item->length = byte_count;
	item->motion = motion;
	return item;
}

static void gui_protocol_output_item_destroy(gui_protocol_output_item *item)
{
	if (item == NULL)
		return;
	free(item->bytes);
	free(item);
}

gui_protocol_output *gui_protocol_output_create(size_t byte_limit)
{
	gui_protocol_output *output;

	if (byte_limit == 0) {
		errno = EINVAL;
		return NULL;
	}
	output = calloc(1, sizeof(*output));
	if (output != NULL)
		output->byte_limit = byte_limit;
	return output;
}

void gui_protocol_output_destroy(gui_protocol_output *output)
{
	gui_protocol_output_item *item;

	if (output == NULL)
		return;
	for (item = output->head; item != NULL;) {
		gui_protocol_output_item *next = item->next;

		gui_protocol_output_item_destroy(item);
		item = next;
	}
	free(output);
}

static int gui_protocol_output_enqueue_item(gui_protocol_output *output,
					    const void *message, size_t byte_count,
					    int motion)
{
	gui_protocol_output_item *item;

	if (output == NULL || !gui_protocol_output_message_valid(message, byte_count))
		return -1;
	if (byte_count > output->byte_limit - output->byte_count) {
		errno = ENOBUFS;
		return -1;
	}
	item = gui_protocol_output_item_create(message, byte_count, motion);
	if (item == NULL)
		return -1;
	if (output->tail == NULL)
		output->head = item;
	else
		output->tail->next = item;
	output->tail = item;
	output->byte_count += byte_count;
	return 0;
}

int gui_protocol_output_enqueue(gui_protocol_output *output, const void *message,
					size_t byte_count)
{
	return gui_protocol_output_enqueue_item(output, message, byte_count, 0);
}

int gui_protocol_output_enqueue_motion(gui_protocol_output *output,
				       const void *message, size_t byte_count)
{
	gui_protocol_output_item *item;

	if (output == NULL || !gui_protocol_output_message_valid(message, byte_count))
		return -1;
	item = output->tail;
	/* A partially written frame is immutable: replacing it would corrupt it. */
	if (item == NULL || !item->motion || item->offset != 0)
		return gui_protocol_output_enqueue_item(output, message, byte_count, 1);
	if (byte_count > output->byte_limit - (output->byte_count - item->length)) {
		errno = ENOBUFS;
		return -1;
	}
	/* Allocate first so an allocation failure leaves the old motion intact. */
	uint8_t *bytes = malloc(byte_count);

	if (bytes == NULL)
		return -1;
	memcpy(bytes, message, byte_count);
	free(item->bytes);
	item->bytes = bytes;
	output->byte_count -= item->length;
	item->length = byte_count;
	output->byte_count += byte_count;
	return 0;
}

gui_protocol_output_result gui_protocol_output_flush(
	gui_protocol_output *output, gui_protocol_write_fn writer, void *context)
{
	if (output == NULL || writer == NULL) {
		errno = EINVAL;
		return GUI_PROTOCOL_OUTPUT_ERROR;
	}
	while (output->head != NULL) {
		gui_protocol_output_item *item = output->head;
		size_t pending = item->length - item->offset;
		ssize_t written = writer(context, item->bytes + item->offset, pending);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return GUI_PROTOCOL_OUTPUT_BLOCKED;
			return GUI_PROTOCOL_OUTPUT_ERROR;
		}
		if (written == 0)
			return GUI_PROTOCOL_OUTPUT_BLOCKED;
		if ((size_t)written > pending) {
			errno = EIO;
			return GUI_PROTOCOL_OUTPUT_ERROR;
		}
		item->offset += written;
		output->byte_count -= written;
		if (item->offset != item->length)
			continue;
		output->head = item->next;
		if (output->head == NULL)
			output->tail = NULL;
		gui_protocol_output_item_destroy(item);
	}
	return GUI_PROTOCOL_OUTPUT_EMPTY;
}
