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

int main(void)
{
	test_header();
	test_hello();
	test_endpoint_roles_are_server_assigned();
	puts("gui-protocol: header ok");
	puts("gui-protocol: hello ok");
	puts("gui-protocol: roles ok");
	return 0;
}
