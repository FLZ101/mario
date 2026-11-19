#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
	static uint8_t input_limit[GUI_PROTOCOL_MAX_MESSAGE_SIZE];
	input_log log = { 0 };
	gui_protocol_input *input = gui_protocol_input_create();

	assert(input != NULL);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_APP_READY,
				  0) == 0);
	for (int index = 0; index < 9; index++)
		assert(gui_protocol_input_append(input, frame, sizeof(frame)) == 0);
	assert(gui_protocol_input_drain(input, record_message, &log) == 8);
	assert(gui_protocol_input_pending(input));
	assert(gui_protocol_input_drain(input, record_message, &log) == 1);
	assert(!gui_protocol_input_pending(input));
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

	input = gui_protocol_input_create();
	assert(input != NULL);
	assert(gui_protocol_input_append(input, input_limit,
		GUI_PROTOCOL_MAX_MESSAGE_SIZE - 1) == 0);
	assert(gui_protocol_input_append(input, input_limit, 2) == -1);
	assert(errno == EOVERFLOW);
	gui_protocol_input_destroy(input);
}

typedef struct {
	const ssize_t *results;
	size_t result_count;
	size_t result_index;
	uint8_t bytes[128];
	size_t byte_count;
} output_log;

static ssize_t record_output(void *context, const void *bytes, size_t byte_count)
{
	output_log *log = context;
	ssize_t result;

	assert(log->result_index < log->result_count);
	result = log->results[log->result_index++];
	if (result < 0) {
		errno = (int)-result;
		return -1;
	}
	assert((size_t)result <= byte_count);
	memcpy(log->bytes + log->byte_count, bytes, (size_t)result);
	log->byte_count += (size_t)result;
	return result;
}

static void make_frame(uint8_t *frame, gui_protocol_type type, uint8_t marker)
{
	assert(gui_protocol_encode_header(frame, GUI_PROTOCOL_HEADER_SIZE + 1, type,
				  1) == 0);
	frame[GUI_PROTOCOL_HEADER_SIZE] = marker;
}

static void test_output_short_sends_and_order(void)
{
	uint8_t first[GUI_PROTOCOL_HEADER_SIZE + 1];
	uint8_t second[GUI_PROTOCOL_HEADER_SIZE + 1];
	const ssize_t first_results[] = { 3, -EINTR, 2, -EAGAIN };
	const ssize_t second_results[] = { 4, 9 };
	output_log first_log = { .results = first_results,
		.result_count = sizeof(first_results) / sizeof(first_results[0]) };
	output_log second_log = { .results = second_results,
		.result_count = sizeof(second_results) / sizeof(second_results[0]) };
	gui_protocol_output *output = gui_protocol_output_create(32);

	assert(output != NULL);
	make_frame(first, GUI_PROTOCOL_WINDOW_RAISE, 'a');
	make_frame(second, GUI_PROTOCOL_APP_READY, 'b');
	assert(gui_protocol_output_enqueue(output, first, sizeof(first)) == 0);
	assert(gui_protocol_output_enqueue(output, second, sizeof(second)) == 0);
	assert(gui_protocol_output_flush(output, record_output, &first_log) ==
	       GUI_PROTOCOL_OUTPUT_BLOCKED);
	assert(first_log.byte_count == 5);
	assert(gui_protocol_output_flush(output, record_output, &second_log) ==
	       GUI_PROTOCOL_OUTPUT_EMPTY);
	assert(second_log.byte_count == sizeof(first) + sizeof(second) - 5);
	assert(memcmp(first_log.bytes, first, 5) == 0);
	assert(memcmp(second_log.bytes, first + 5, sizeof(first) - 5) == 0);
	assert(memcmp(second_log.bytes + sizeof(first) - 5, second,
		      sizeof(second)) == 0);
	gui_protocol_output_destroy(output);
}

static void test_output_motion_coalescing_and_exhaustion(void)
{
	uint8_t motion_one[GUI_PROTOCOL_HEADER_SIZE + 1];
	uint8_t motion_two[GUI_PROTOCOL_HEADER_SIZE + 1];
	uint8_t key[GUI_PROTOCOL_HEADER_SIZE + 1];
	uint8_t motion_three[GUI_PROTOCOL_HEADER_SIZE + 1];
	uint8_t motion_four[GUI_PROTOCOL_HEADER_SIZE + 1];
	const ssize_t results[] = { 9, 9, 9 };
	output_log log = { .results = results,
		.result_count = sizeof(results) / sizeof(results[0]) };
	gui_protocol_output *output = gui_protocol_output_create(27);

	assert(output != NULL);
	make_frame(motion_one, GUI_PROTOCOL_INPUT_EVENT, '1');
	make_frame(motion_two, GUI_PROTOCOL_INPUT_EVENT, '2');
	make_frame(key, GUI_PROTOCOL_INPUT_EVENT, 'k');
	make_frame(motion_three, GUI_PROTOCOL_INPUT_EVENT, '3');
	make_frame(motion_four, GUI_PROTOCOL_INPUT_EVENT, '4');
	assert(gui_protocol_output_enqueue_motion(output, motion_one,
					  sizeof(motion_one)) == 0);
	assert(gui_protocol_output_enqueue_motion(output, motion_two,
					  sizeof(motion_two)) == 0);
	assert(gui_protocol_output_enqueue(output, key, sizeof(key)) == 0);
	assert(gui_protocol_output_enqueue_motion(output, motion_three,
					  sizeof(motion_three)) == 0);
	assert(gui_protocol_output_enqueue_motion(output, motion_four,
					  sizeof(motion_four)) == 0);
	assert(gui_protocol_output_enqueue(output, key, sizeof(key)) == -1);
	assert(errno == ENOBUFS);
	assert(gui_protocol_output_flush(output, record_output, &log) ==
	       GUI_PROTOCOL_OUTPUT_EMPTY);
	assert(log.byte_count == 27);
	assert(memcmp(log.bytes, motion_two, sizeof(motion_two)) == 0);
	assert(memcmp(log.bytes + sizeof(motion_two), key, sizeof(key)) == 0);
	assert(memcmp(log.bytes + sizeof(motion_two) + sizeof(key), motion_four,
		      sizeof(motion_four)) == 0);
	gui_protocol_output_destroy(output);
}

static void test_patch_wire_contract(void)
{
	uint8_t header_bytes[GUI_PROTOCOL_HEADER_SIZE];
	gui_protocol_header header;
	gui_protocol_type types[] = { GUI_PROTOCOL_CAPABILITIES, GUI_PROTOCOL_CAPABILITIES_REPLY,
		GUI_PROTOCOL_SURFACE_PATCH_BEGIN, GUI_PROTOCOL_SURFACE_CANCEL, GUI_PROTOCOL_WINDOW_OPACITY };

	assert(GUI_PROTOCOL_VERSION_MAJOR == 1 && GUI_PROTOCOL_VERSION_MINOR == 1);
	assert(GUI_PROTOCOL_CAP_SURFACE_PATCH == 1);
	assert(GUI_PROTOCOL_CAP_WINDOW_OPACITY == 2);
	assert(sizeof(gui_protocol_wire_window_opacity) == 8);
	assert(sizeof(gui_protocol_wire_hello) == 4);
	assert(sizeof(gui_protocol_wire_capabilities) == 4);
	assert(sizeof(gui_protocol_wire_surface_begin) == 36);
	assert(sizeof(gui_protocol_wire_surface_patch_begin) == 44);
	assert(offsetof(gui_protocol_wire_surface_patch_begin, base_frame_serial_le) == 12);
	assert(offsetof(gui_protocol_wire_surface_patch_begin, frame_serial_le) == 20);
	assert(offsetof(gui_protocol_wire_surface_patch_begin, x_le) == 28);
	assert(sizeof(gui_protocol_wire_surface_data) == 28);
	assert(sizeof(gui_protocol_wire_surface_cancel) == 20);
	for (unsigned int index = 0; index < sizeof(types) / sizeof(types[0]); index++) {
		assert(gui_protocol_encode_header(header_bytes, sizeof(header_bytes), types[index], 44) == 0);
		assert(gui_protocol_decode_header(header_bytes, sizeof(header_bytes), &header) == 0);
		assert(header.type == types[index] && header.length == 52);
	}
}

int main(void)
{
	test_header();
	test_hello();
	test_patch_wire_contract();
	test_endpoint_roles_are_server_assigned();
	test_fragmented_input();
	test_input_errors_and_drain_limit();
	test_output_short_sends_and_order();
	test_output_motion_coalescing_and_exhaustion();
	puts("gui-protocol: header ok");
	puts("gui-protocol: hello ok");
	puts("gui-protocol: roles ok");
	puts("gui-protocol: input ok");
	puts("gui-protocol: output ok");
	puts("gui-protocol: negotiated patch wire layouts preserve v1.0 payloads");
	return 0;
}
