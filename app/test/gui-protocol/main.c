#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <gui/protocol.h>

static void test_header(void)
{
	uint8_t buffer[GUI_PROTOCOL_HEADER_SIZE];
	gui_protocol_header header;
	uint32_t length;

	assert(gui_protocol_message_length(0, &length) == 0);
	assert(length == GUI_PROTOCOL_HEADER_SIZE);
	assert(gui_protocol_message_length(GUI_PROTOCOL_MAX_MESSAGE_SIZE,
				   &length) == -1);
	assert(errno == EMSGSIZE);
	assert(gui_protocol_message_length(0, NULL) == -1);
	assert(errno == EINVAL);
	assert(gui_protocol_encode_header(buffer, sizeof(buffer),
				  GUI_PROTOCOL_WINDOW_CREATE, 12) == 0);
	assert(buffer[0] == 20 && buffer[1] == 0 && buffer[4] ==
	       GUI_PROTOCOL_WINDOW_CREATE && buffer[5] == 0);
	assert(gui_protocol_decode_header(buffer, sizeof(buffer), &header) == 0);
	assert(header.length == 20 && header.type == GUI_PROTOCOL_WINDOW_CREATE);
	buffer[6] = 1;
	assert(gui_protocol_decode_header(buffer, sizeof(buffer), &header) == -1);
	assert(errno == EPROTO);
	buffer[6] = 0;
	buffer[0] = GUI_PROTOCOL_HEADER_SIZE - 1;
	assert(gui_protocol_decode_header(buffer, sizeof(buffer), &header) == -1);
	assert(errno == EPROTO);
}

static void test_hello(void)
{
	uint8_t buffer[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_hello)];
	gui_protocol_hello sent = {
		.major = GUI_PROTOCOL_VERSION_MAJOR,
		.minor = GUI_PROTOCOL_VERSION_MINOR,
	};
	gui_protocol_hello received = { 0 };

	assert(sizeof(gui_protocol_wire_hello) == 4);
	assert(gui_protocol_encode_hello(buffer, sizeof(buffer), &sent) == 0);
	assert(gui_protocol_decode_hello(buffer, sizeof(buffer), &received) == 0);
	assert(received.major == sent.major && received.minor == sent.minor);
	assert(buffer[GUI_PROTOCOL_HEADER_SIZE] == GUI_PROTOCOL_VERSION_MAJOR);
}

static void test_endpoint_roles_are_server_assigned(void)
{
	gui_protocol_endpoint_role role = GUI_PROTOCOL_ENDPOINT_SHELL;

	assert(role == GUI_PROTOCOL_ENDPOINT_SHELL);
	assert(sizeof(gui_protocol_wire_hello) == 4);
	/* HELLO contains only protocol version fields, never an endpoint role. */
}

typedef struct {
	int count;
	gui_protocol_type types[9];
} input_log;

static void record_message(void *context, const gui_protocol_header *header,
			   const void *payload, size_t payload_bytes)
{
	input_log *log = context;

	assert(payload == NULL || payload_bytes == 0);
	log->types[log->count++] = header->type;
}

static void test_fragmented_input(void)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE];
	input_log log = { 0 };
	gui_protocol_input *input = gui_protocol_input_create();

	assert(input != NULL);
	assert(gui_protocol_encode_header(frame, sizeof(frame),
				  GUI_PROTOCOL_WINDOW_RAISE, 0) == 0);
	assert(gui_protocol_input_append(input, frame, 3) == 0);
	assert(gui_protocol_input_drain(input, record_message, &log) == 0);
	assert(gui_protocol_input_append(input, frame + 3, sizeof(frame) - 3) == 0);
	assert(gui_protocol_input_drain(input, record_message, &log) == 1);
	assert(log.count == 1 && log.types[0] == GUI_PROTOCOL_WINDOW_RAISE);
	gui_protocol_input_destroy(input);
}

static void test_input_errors_and_drain_limit(void)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE];
	uint8_t oversized[GUI_PROTOCOL_HEADER_SIZE] = { 1, 0, 1 };
	input_log log = { 0 };
	gui_protocol_input *input = gui_protocol_input_create();

	assert(input != NULL);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_APP_READY,
				  0) == 0);
	for (int index = 0; index < 9; index++)
		assert(gui_protocol_input_append(input, frame, sizeof(frame)) == 0);
	assert(gui_protocol_input_drain(input, record_message, &log) == 8);
	assert(gui_protocol_input_drain(input, record_message, &log) == 1);
	assert(log.count == 9);
	assert(gui_protocol_input_append(input, frame, sizeof(frame) - 1) == 0);
	gui_protocol_input_finish(input);
	assert(gui_protocol_input_drain(input, record_message, &log) == -1);
	assert(errno == EPROTO);
	gui_protocol_input_destroy(input);

	input = gui_protocol_input_create();
	assert(input != NULL);
	assert(gui_protocol_input_append(input, oversized, sizeof(oversized)) == 0);
	assert(gui_protocol_input_drain(input, record_message, &log) == -1);
	assert(errno == EPROTO);
	gui_protocol_input_destroy(input);
}

int main(void)
{
	test_header();
	test_hello();
	test_endpoint_roles_are_server_assigned();
	test_fragmented_input();
	test_input_errors_and_drain_limit();
	puts("gui-protocol: header ok");
	puts("gui-protocol: hello ok");
	puts("gui-protocol: roles ok");
	puts("gui-protocol: input ok");
	return 0;
}
