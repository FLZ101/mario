#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gui/protocol.h>

static void put32(uint8_t *bytes, uint32_t value)
{
	for (unsigned int index = 0; index < 4; index++)
		bytes[index] = value >> (index * 8);
}

static uint32_t get32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	       (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void read_frame(int fd, uint8_t *frame, gui_protocol_type type)
{
	gui_protocol_header header;

	assert(read(fd, frame, GUI_PROTOCOL_HEADER_SIZE) == GUI_PROTOCOL_HEADER_SIZE);
	assert(gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE, &header) == 0);
	assert(header.type == type);
	assert(read(fd, frame + GUI_PROTOCOL_HEADER_SIZE,
		    header.length - GUI_PROTOCOL_HEADER_SIZE) ==
	       (ssize_t)(header.length - GUI_PROTOCOL_HEADER_SIZE));
}

int main(void)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	uint8_t frame[64];
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	assert(fd >= 0);
	strcpy(address.sun_path, "/tmp/mario-wm.sock");
	assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	assert(gui_protocol_encode_hello(frame, sizeof(frame),
		&(gui_protocol_hello) { GUI_PROTOCOL_VERSION_MAJOR,
			GUI_PROTOCOL_VERSION_MINOR }) == 0);
	assert(write(fd, frame, 12) == 12);
	read_frame(fd, frame, GUI_PROTOCOL_HELLO_REPLY);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_CREATE,
				  sizeof(gui_protocol_wire_window_create)) == 0);
	memset(frame + GUI_PROTOCOL_HEADER_SIZE, 0,
	       sizeof(gui_protocol_wire_window_create));
	put32(frame + 8, 7);
	put32(frame + 12, 64);
	put32(frame + 16, 48);
	assert(write(fd, frame, 24) == 24);
	read_frame(fd, frame, GUI_PROTOCOL_WINDOW_CREATE_REPLY);
	assert(get32(frame + 8) == 7 && get32(frame + 12) == 1);
	read_frame(fd, frame, GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get32(frame + 8) == 1 && get32(frame + 12) == 1);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_DESTROY, 4) == 0);
	put32(frame + 8, 1);
	assert(write(fd, frame, 12) == 12);
	close(fd);
	puts("wm-client: create focus close ok");
	return 0;
}
