#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gui/input.h>
#include <gui/protocol.h>
#include <gui/surface.h>

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

static uint16_t get16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static uint32_t get32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	       (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void read_full(int fd, void *buffer, size_t byte_count)
{
	uint8_t *bytes = buffer;

	while (byte_count != 0) {
		ssize_t count = read(fd, bytes, byte_count);

		assert(count > 0);
		bytes += count;
		byte_count -= count;
	}
}

static void read_frame(int fd, uint8_t *frame, gui_protocol_type type)
{
	gui_protocol_header header;

	read_full(fd, frame, GUI_PROTOCOL_HEADER_SIZE);
	assert(gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE, &header) == 0);
	assert(header.type == type);
	read_full(fd, frame + GUI_PROTOCOL_HEADER_SIZE,
		  header.length - GUI_PROTOCOL_HEADER_SIZE);
}

static int connect_client(void)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	assert(fd >= 0);
	strcpy(address.sun_path, "/tmp/mario-wm.sock");
	assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	return fd;
}

static void create_window(int fd, uint32_t request_id, uint32_t width,
			  uint32_t height)
{
	uint8_t frame[64];

	assert(gui_protocol_encode_hello(frame, sizeof(frame),
		&(gui_protocol_hello) { GUI_PROTOCOL_VERSION_MAJOR,
			GUI_PROTOCOL_VERSION_MINOR }) == 0);
	assert(write(fd, frame, 12) == 12);
	read_frame(fd, frame, GUI_PROTOCOL_HELLO_REPLY);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_CREATE,
				  sizeof(gui_protocol_wire_window_create)) == 0);
	memset(frame + GUI_PROTOCOL_HEADER_SIZE, 0,
	       sizeof(gui_protocol_wire_window_create));
	put32(frame + 8, request_id);
	put32(frame + 12, width);
	put32(frame + 16, height);
	assert(write(fd, frame, 24) == 24);
	read_frame(fd, frame, GUI_PROTOCOL_WINDOW_CREATE_REPLY);
	assert(get32(frame + 8) == request_id && get32(frame + 12) == 1);
	read_frame(fd, frame, GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get32(frame + 8) == 1 && get32(frame + 12) == 1);
	read_frame(fd, frame, GUI_PROTOCOL_INPUT_RESET);
	assert(get32(frame) == GUI_PROTOCOL_HEADER_SIZE);
}

static void begin_surface(int fd, uint32_t width, uint32_t height,
			  uint64_t frame_serial)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) +
		      sizeof(gui_protocol_wire_surface_begin)];

	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_SURFACE_BEGIN,
				  sizeof(gui_protocol_wire_surface_begin)) == 0);
	put32(frame + 8, 1);
	put64(frame + 12, 1);
	put64(frame + 20, frame_serial);
	put32(frame + 28, 0);
	put32(frame + 32, 0);
	put32(frame + 36, width);
	put32(frame + 40, height);
	assert(write(fd, frame, sizeof(frame)) == sizeof(frame));
}

static void read_error(int fd, int status, gui_protocol_type failed_type)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) + sizeof(gui_protocol_wire_error)];

	read_frame(fd, frame, GUI_PROTOCOL_ERROR);
	assert((int32_t)get32(frame + 8) == status);
	assert(get16(frame + 12) == failed_type);
}

static void send_surface_data(int fd, uint32_t row_offset, uint32_t rows,
			      uint64_t frame_serial, gui_color color)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) +
		      sizeof(gui_protocol_wire_surface_data) + 16];
	size_t pixel_count = (size_t)rows * 2;
	size_t payload_bytes = sizeof(gui_protocol_wire_surface_data) +
		pixel_count * sizeof(gui_color);

	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_SURFACE_DATA,
				  payload_bytes) == 0);
	put32(frame + 8, 1);
	put64(frame + 12, 1);
	put64(frame + 20, frame_serial);
	put32(frame + 28, row_offset);
	put32(frame + 32, rows);
	for (size_t index = 0; index < pixel_count; index++)
		put32(frame + 36 + index * sizeof(gui_color), color);
	assert(write(fd, frame, GUI_PROTOCOL_HEADER_SIZE + payload_bytes) ==
	       (ssize_t)(GUI_PROTOCOL_HEADER_SIZE + payload_bytes));
}

static void commit_surface(int fd, uint64_t frame_serial)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) +
		      sizeof(gui_protocol_wire_surface_commit)];

	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_SURFACE_COMMIT,
				  sizeof(gui_protocol_wire_surface_commit)) == 0);
	put32(frame + 8, 1);
	put64(frame + 12, 1);
	put64(frame + 20, frame_serial);
	assert(write(fd, frame, sizeof(frame)) == sizeof(frame));
}

static void test_staging(void)
{
	int stager = connect_client();
	int large[3];

	create_window(stager, 9, 64, 48);
	begin_surface(stager, UINT32_MAX, 48, 1);
	read_error(stager, -EINVAL, GUI_PROTOCOL_SURFACE_BEGIN);
	begin_surface(stager, 64, 48, 1);
	begin_surface(stager, 64, 48, 2);
	read_error(stager, -EBUSY, GUI_PROTOCOL_SURFACE_BEGIN);
	for (size_t index = 0; index < sizeof(large) / sizeof(large[0]); index++) {
		large[index] = connect_client();
		create_window(large[index], (uint32_t)(10 + index), 1024, 768);
		begin_surface(large[index], 1024, 768, 1);
	}
	read_error(large[2], -ENOMEM, GUI_PROTOCOL_SURFACE_BEGIN);
	for (size_t index = 0; index < sizeof(large) / sizeof(large[0]); index++)
		close(large[index]);
	close(stager);
}

static void test_commit(void)
{
	int fd = connect_client();

	create_window(fd, 20, 2, 2);
	begin_surface(fd, 2, 2, 1);
	send_surface_data(fd, 1, 1, 1, 0x00ff0000);
	read_error(fd, -EINVAL, GUI_PROTOCOL_SURFACE_DATA);
	send_surface_data(fd, 0, 2, 1, 0x00ff0000);
	commit_surface(fd, 1);
	begin_surface(fd, 2, 2, 2);
	/* A stale update cannot overwrite the committed red frame. */
	send_surface_data(fd, 0, 1, 1, 0x000000ff);
	read_error(fd, -EINVAL, GUI_PROTOCOL_SURFACE_DATA);
	commit_surface(fd, 1);
	read_error(fd, -EINVAL, GUI_PROTOCOL_SURFACE_COMMIT);
	/* Dropping a partially transferred replacement must retain the old frame. */
	send_surface_data(fd, 0, 1, 2, 0x000000ff);
	assert(shutdown(fd, SHUT_WR) == 0);
	assert(read(fd, &(uint8_t) { 0 }, 1) == 0);
	close(fd);
}

int main(int argc, char **argv)
{
	uint8_t frame[64];
	int first;
	int second;

	assert(argc == 1 || (argc == 2 && (!strcmp(argv[1], "--surface-only") ||
		!strcmp(argv[1], "--commit-only"))));
	if (argc == 2) {
		if (!strcmp(argv[1], "--surface-only")) {
			test_staging();
			puts("wm-client: staging ok");
		} else {
			test_commit();
			puts("wm-client: commit ok");
		}
		return 0;
	}
	first = connect_client();

	create_window(first, 7, 64, 48);
	second = connect_client();
	create_window(second, 8, 64, 48);
	read_frame(first, frame, GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get32(frame + 8) == 1 && get32(frame + 12) == 0);
	read_frame(first, frame, GUI_PROTOCOL_INPUT_RESET);
	close(second);
	/* The first owner must be reset again before it can receive later input. */
	read_frame(first, frame, GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get32(frame + 8) == 1 && get32(frame + 12) == 1);
	read_frame(first, frame, GUI_PROTOCOL_INPUT_RESET);
	puts("wm-client: focus reset ready");
	fflush(stdout);
	/* F6 then F8 each invalidate physical state at the seat boundary. */
	read_frame(first, frame, GUI_PROTOCOL_INPUT_RESET);
	puts("wm-client: text seat reset ready");
	fflush(stdout);
	read_frame(first, frame, GUI_PROTOCOL_INPUT_RESET);
	puts("wm-client: graphics seat reset ready");
	fflush(stdout);
	read_frame(first, frame, GUI_PROTOCOL_INPUT_EVENT);
	assert(get32(frame + 8) == 1 && get32(frame + 12) == GUI_INPUT_EVENT_KEY);
	/* GUI-TYPE sends the matching key release after the key press. */
	read_frame(first, frame, GUI_PROTOCOL_INPUT_EVENT);
	assert(get32(frame + 8) == 1 && get32(frame + 12) == GUI_INPUT_EVENT_KEY);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_DESTROY, 4) == 0);
	put32(frame + 8, 1);
	assert(write(first, frame, 12) == 12);
	close(first);
	puts("wm-client: ownership resets ok");
	return 0;
}
