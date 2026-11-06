#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gui/protocol.h>

#define WM_FD 3
#define SURFACE_PIXELS_PER_MESSAGE 1000U

/* Keep the largest wire frame out of the small initial userspace stack. */
static uint8_t desktop_frame[GUI_PROTOCOL_HEADER_SIZE +
	sizeof(gui_protocol_wire_surface_data) +
	SURFACE_PIXELS_PER_MESSAGE * sizeof(uint32_t)];

static void read_full(int fd, void *buffer, size_t byte_count)
{
	uint8_t *bytes = buffer;

	while (byte_count != 0) {
		ssize_t count = read(fd, bytes, byte_count);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			_exit(1);
		bytes += count;
		byte_count -= count;
	}
}

static int write_full(int fd, const void *buffer, size_t byte_count)
{
	const uint8_t *bytes = buffer;

	while (byte_count != 0) {
		ssize_t count = write(fd, bytes, byte_count);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return -1;
		bytes += count;
		byte_count -= count;
	}
	return 0;
}

static void put32(uint8_t *bytes, uint32_t value)
{
	for (unsigned int index = 0; index < 4; index++)
		bytes[index] = value >> (index * 8);
}

static void put64(uint8_t *bytes, uint64_t value)
{
	for (unsigned int index = 0; index < 8; index++)
		bytes[index] = value >> (index * 8);
}

static uint32_t get32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
		(uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint64_t get64(const uint8_t *bytes)
{
	uint64_t value = 0;

	for (unsigned int index = 0; index < 8; index++)
		value |= (uint64_t)bytes[index] << (index * 8);
	return value;
}

static int read_surface_data_reply(uint32_t pixel_offset, uint32_t pixel_count)
{
	uint8_t reply[GUI_PROTOCOL_HEADER_SIZE +
		      sizeof(gui_protocol_wire_surface_data_reply)];
	gui_protocol_header header;

	read_full(WM_FD, reply, GUI_PROTOCOL_HEADER_SIZE);
	if (gui_protocol_decode_header(reply, GUI_PROTOCOL_HEADER_SIZE, &header) < 0 ||
	    header.type != GUI_PROTOCOL_SURFACE_DATA_REPLY ||
	    header.length != sizeof(reply))
		return -1;
	read_full(WM_FD, reply + GUI_PROTOCOL_HEADER_SIZE,
		sizeof(gui_protocol_wire_surface_data_reply));
	return get32(reply + 8) == 1 && get64(reply + 12) == 1 &&
		get64(reply + 20) == 1 && get32(reply + 28) == pixel_offset &&
		get32(reply + 32) == pixel_count ? 0 : -1;
}

static int send_desktop(uint32_t width, uint32_t height)
{
	uint8_t begin[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_begin)] = { 0 };
	uint8_t commit[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_commit)];
	uint32_t total;

	if (width != 0 && height > UINT32_MAX / width)
		return -1;
	total = width * height;

	if (gui_protocol_encode_header(begin, sizeof(begin), GUI_PROTOCOL_SURFACE_BEGIN,
		sizeof(gui_protocol_wire_surface_begin)) < 0)
		return -1;
	put32(begin + 8, 1); put64(begin + 12, 1); put64(begin + 20, 1);
	put32(begin + 36, width); put32(begin + 40, height);
	if (write_full(WM_FD, begin, sizeof(begin)) < 0)
		return -1;
	for (uint32_t pixel_offset = 0; pixel_offset < total;) {
		uint32_t pixels = total - pixel_offset > SURFACE_PIXELS_PER_MESSAGE ?
			SURFACE_PIXELS_PER_MESSAGE : total - pixel_offset;
		size_t payload = sizeof(gui_protocol_wire_surface_data) + pixels * sizeof(uint32_t);

		/* The protocol keeps each frame below Mario's one-page socket ring. */
		if (gui_protocol_encode_header(desktop_frame, sizeof(desktop_frame),
			GUI_PROTOCOL_SURFACE_DATA,
			payload) < 0)
			return -1;
		put32(desktop_frame + 8, 1); put64(desktop_frame + 12, 1);
		put64(desktop_frame + 20, 1);
		put32(desktop_frame + 28, pixel_offset); put32(desktop_frame + 32, pixels);
		if (write_full(WM_FD, desktop_frame,
			GUI_PROTOCOL_HEADER_SIZE + payload) < 0)
			return -1;
		if (read_surface_data_reply(pixel_offset, pixels) < 0)
			return -1;
		pixel_offset += pixels;
	}
	if (gui_protocol_encode_header(commit, sizeof(commit), GUI_PROTOCOL_SURFACE_COMMIT,
		sizeof(gui_protocol_wire_surface_commit)) < 0)
		return -1;
	put32(commit + 8, 1); put64(commit + 12, 1); put64(commit + 20, 1);
	return write_full(WM_FD, commit, sizeof(commit));
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
	uint8_t frame[64];
	gui_protocol_header header;
	uint8_t discard[256];

	if (argc != 2 || strcmp(argv[1], "--wm-fd=3") != 0 || private_fd_only() < 0) {
		fprintf(stderr, "mario-desktop: invalid inherited descriptors\n");
		return 1;
	}
	if (gui_protocol_encode_hello(frame, sizeof(frame),
		&(gui_protocol_hello) { GUI_PROTOCOL_VERSION_MAJOR,
			GUI_PROTOCOL_VERSION_MINOR }) < 0 ||
	    write_full(WM_FD, frame, GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_hello)) < 0)
		return 1;
	read_full(WM_FD, frame, GUI_PROTOCOL_HEADER_SIZE);
	if (gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE, &header) < 0 ||
	    header.type != GUI_PROTOCOL_HELLO_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_hello))
		return 1;
	read_full(WM_FD, discard, sizeof(gui_protocol_wire_hello));
	if (gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_DESKTOP_CREATE,
		sizeof(gui_protocol_wire_desktop_create)) < 0)
		return 1;
	put32(frame + GUI_PROTOCOL_HEADER_SIZE, 1);
	if (write_full(WM_FD, frame, GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_desktop_create)) < 0)
		return 1;
	read_full(WM_FD, frame, GUI_PROTOCOL_HEADER_SIZE);
	if (gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE, &header) < 0 ||
	    header.type != GUI_PROTOCOL_DESKTOP_CREATE_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_desktop_create_reply))
		return 1;
	read_full(WM_FD, frame + GUI_PROTOCOL_HEADER_SIZE,
		sizeof(gui_protocol_wire_desktop_create_reply));
	if (send_desktop(get32(frame + 24), get32(frame + 28)) < 0)
		return 1;
	puts("mario-desktop: private connection ready");
	fflush(stdout);
	while (read(WM_FD, discard, sizeof(discard)) > 0)
		;
	return 0;
}
