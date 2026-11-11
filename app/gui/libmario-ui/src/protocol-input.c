#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <gui/protocol.h>

struct gui_protocol_input {
	uint8_t bytes[GUI_PROTOCOL_MAX_MESSAGE_SIZE];
	size_t count;
	int finished;
};

gui_protocol_input *gui_protocol_input_create(void)
{
	return calloc(1, sizeof(gui_protocol_input));
}

void gui_protocol_input_destroy(gui_protocol_input *input)
{
	free(input);
}

int gui_protocol_input_append(gui_protocol_input *input, const void *bytes,
				      size_t byte_count)
{
	if (input == NULL || (bytes == NULL && byte_count != 0) || input->finished) {
		errno = EINVAL;
		return -1;
	}
	if (byte_count > sizeof(input->bytes) - input->count) {
		errno = EOVERFLOW;
		return -1;
	}
	if (byte_count != 0)
		memcpy(input->bytes + input->count, bytes, byte_count);
	input->count += byte_count;
	return 0;
}

void gui_protocol_input_finish(gui_protocol_input *input)
{
	if (input != NULL)
		input->finished = 1;
}

int gui_protocol_input_pending(const gui_protocol_input *input)
{
	return input != NULL && input->count != 0;
}

int gui_protocol_input_drain(gui_protocol_input *input,
				     gui_protocol_message_fn callback, void *context)
{
	int messages = 0;

	if (input == NULL || callback == NULL) {
		errno = EINVAL;
		return -1;
	}
	while (messages < 8 && input->count >= GUI_PROTOCOL_HEADER_SIZE) {
		gui_protocol_header header;

		if (gui_protocol_decode_header(input->bytes, input->count, &header) < 0)
			return -1;
		if (input->count < header.length)
			break;
		callback(context, &header, input->bytes + GUI_PROTOCOL_HEADER_SIZE,
			 header.length - GUI_PROTOCOL_HEADER_SIZE);
		input->count -= header.length;
		memmove(input->bytes, input->bytes + header.length, input->count);
		messages++;
	}
	/* EOF is valid only between frames, never in a partial header or payload. */
	if (input->finished && input->count != 0) {
		errno = EPROTO;
		return -1;
	}
	return messages;
}
