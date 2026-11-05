#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gui/protocol.h>

#define WM_FD 3

static void read_full(int fd, void *buffer, size_t byte_count)
{
	uint8_t *bytes = buffer;

	while (byte_count != 0) {
		ssize_t count = read(fd, bytes, byte_count);

		if (count <= 0)
			_exit(1);
		bytes += count;
		byte_count -= count;
	}
}

static int private_fd_only(void)
{
	if (fcntl(WM_FD, F_GETFD) < 0)
		return -1;
	for (int fd = WM_FD + 1; fd < 1024; fd++) {
		if (fcntl(fd, F_GETFD) >= 0 || errno != EBADF)
			return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_hello)];
	gui_protocol_header header;
	uint8_t discard[256];

	if (argc != 2 || strcmp(argv[1], "--wm-fd=3") != 0 || private_fd_only() < 0) {
		fprintf(stderr, "mario-desktop: invalid inherited descriptors\n");
		return 1;
	}
	if (gui_protocol_encode_hello(frame, sizeof(frame),
		&(gui_protocol_hello) { GUI_PROTOCOL_VERSION_MAJOR,
			GUI_PROTOCOL_VERSION_MINOR }) < 0 ||
	    write(WM_FD, frame, sizeof(frame)) != sizeof(frame))
		return 1;
	read_full(WM_FD, frame, GUI_PROTOCOL_HEADER_SIZE);
	if (gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE, &header) < 0 ||
	    header.type != GUI_PROTOCOL_HELLO_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_hello))
		return 1;
	read_full(WM_FD, discard, sizeof(gui_protocol_wire_hello));
	puts("mario-desktop: private connection ready");
	fflush(stdout);
	while (read(WM_FD, discard, sizeof(discard)) > 0)
		;
	return 0;
}
