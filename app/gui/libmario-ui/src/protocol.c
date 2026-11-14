#include <errno.h>
#include <stdint.h>

#include <gui/protocol.h>

static void gui_protocol_put_u16(uint8_t *buffer, uint16_t value)
{
	buffer[0] = (uint8_t)value;
	buffer[1] = (uint8_t)(value >> 8);
}

static void gui_protocol_put_u32(uint8_t *buffer, uint32_t value)
{
	for (unsigned int index = 0; index < 4; index++)
		buffer[index] = (uint8_t)(value >> (index * 8));
}

static uint16_t gui_protocol_get_u16(const uint8_t *buffer)
{
	return (uint16_t)buffer[0] | (uint16_t)buffer[1] << 8;
}

static uint32_t gui_protocol_get_u32(const uint8_t *buffer)
{
	uint32_t value = 0;

	for (unsigned int index = 0; index < 4; index++)
		value |= (uint32_t)buffer[index] << (index * 8);
	return value;
}

static int gui_protocol_type_valid(uint16_t type)
{
	return type >= GUI_PROTOCOL_HELLO && type <= GUI_PROTOCOL_WINDOW_STATE_REQUEST;
}

int gui_protocol_message_length(size_t payload_bytes, uint32_t *length)
{
	if (length == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (payload_bytes > GUI_PROTOCOL_MAX_MESSAGE_SIZE -
	    GUI_PROTOCOL_HEADER_SIZE) {
		errno = EMSGSIZE;
		return -1;
	}
	*length = GUI_PROTOCOL_HEADER_SIZE + (uint32_t)payload_bytes;
	return 0;
}

int gui_protocol_encode_header(void *buffer, size_t buffer_bytes,
			       gui_protocol_type type, size_t payload_bytes)
{
	uint8_t *bytes = buffer;
	uint32_t length;

	if (buffer == NULL || !gui_protocol_type_valid(type)) {
		errno = EINVAL;
		return -1;
	}
	if (gui_protocol_message_length(payload_bytes, &length) < 0)
		return -1;
	if (buffer_bytes < GUI_PROTOCOL_HEADER_SIZE) {
		errno = EMSGSIZE;
		return -1;
	}
	gui_protocol_put_u32(bytes, length);
	gui_protocol_put_u16(bytes + 4, type);
	gui_protocol_put_u16(bytes + 6, 0);
	return 0;
}

int gui_protocol_decode_header(const void *buffer, size_t buffer_bytes,
			       gui_protocol_header *header)
{
	const uint8_t *bytes = buffer;
	uint32_t length;
	uint16_t type;
	uint16_t flags;

	if (buffer == NULL || header == NULL ||
	    buffer_bytes < GUI_PROTOCOL_HEADER_SIZE) {
		errno = EMSGSIZE;
		return -1;
	}
	length = gui_protocol_get_u32(bytes);
	type = gui_protocol_get_u16(bytes + 4);
	flags = gui_protocol_get_u16(bytes + 6);
	if (length < GUI_PROTOCOL_MIN_MESSAGE_SIZE ||
	    length > GUI_PROTOCOL_MAX_MESSAGE_SIZE || flags != 0 ||
	    !gui_protocol_type_valid(type)) {
		errno = EPROTO;
		return -1;
	}
	header->length = length;
	header->type = type;
	return 0;
}

int gui_protocol_encode_hello(void *buffer, size_t buffer_bytes,
			      const gui_protocol_hello *hello)
{
	uint8_t *bytes = buffer;
	uint32_t length;

	if (buffer == NULL || hello == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (gui_protocol_message_length(sizeof(gui_protocol_wire_hello), &length) <
	    0 || buffer_bytes < length) {
		errno = EMSGSIZE;
		return -1;
	}
	if (gui_protocol_encode_header(buffer, buffer_bytes, GUI_PROTOCOL_HELLO,
				       sizeof(gui_protocol_wire_hello)) < 0)
		return -1;
	gui_protocol_put_u16(bytes + GUI_PROTOCOL_HEADER_SIZE, hello->major);
	gui_protocol_put_u16(bytes + GUI_PROTOCOL_HEADER_SIZE + 2, hello->minor);
	return 0;
}

int gui_protocol_decode_hello(const void *buffer, size_t buffer_bytes,
			      gui_protocol_hello *hello)
{
	const uint8_t *bytes = buffer;
	gui_protocol_header header;

	if (hello == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (gui_protocol_decode_header(buffer, buffer_bytes, &header) < 0)
		return -1;
	if (header.type != GUI_PROTOCOL_HELLO ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE +
	    sizeof(gui_protocol_wire_hello) || buffer_bytes < header.length) {
		errno = EPROTO;
		return -1;
	}
	hello->major = gui_protocol_get_u16(bytes + GUI_PROTOCOL_HEADER_SIZE);
	hello->minor = gui_protocol_get_u16(bytes + GUI_PROTOCOL_HEADER_SIZE + 2);
	return 0;
}
