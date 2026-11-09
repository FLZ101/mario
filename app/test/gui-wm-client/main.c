#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gui/input.h>
#include <gui/protocol.h>
#include <gui/surface.h>

#define SURFACE_PIXELS_PER_MESSAGE 1000U

/* Large pixel runs must not consume the small initial userspace stack. */
static uint8_t surface_frame[sizeof(gui_protocol_wire_header) +
			     sizeof(gui_protocol_wire_surface_data) +
			     SURFACE_PIXELS_PER_MESSAGE * sizeof(gui_color)];
static unsigned int test_window_placement;

static void put32(uint8_t *bytes, uint32_t value)
{
	for (unsigned int index = 0; index < 4; index++)
		bytes[index] = value >> (index * 8);
}

static void put16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
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

static uint64_t get64(const uint8_t *bytes)
{
	uint64_t value = 0;

	for (unsigned int index = 0; index < 8; index++)
		value |= (uint64_t)bytes[index] << (index * 8);
	return value;
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

static void write_full(int fd, const void *buffer, size_t byte_count)
{
	const uint8_t *bytes = buffer;

	while (byte_count != 0) {
		ssize_t count = write(fd, bytes, byte_count);

		if (count < 0 && errno == EINTR)
			continue;
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

static void hello(int fd)
{
	uint8_t frame[64];

	assert(gui_protocol_encode_hello(frame, sizeof(frame),
		&(gui_protocol_hello) { GUI_PROTOCOL_VERSION_MAJOR,
			GUI_PROTOCOL_VERSION_MINOR }) == 0);
	write_full(fd, frame, 12);
	read_frame(fd, frame, GUI_PROTOCOL_HELLO_REPLY);
}

static uint32_t create_window(int fd, uint32_t request_id, uint32_t width,
			      uint32_t height)
{
	uint8_t frame[64];
	uint32_t window_id;
	uint32_t placement = test_window_placement++;

	hello(fd);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_CREATE,
				  sizeof(gui_protocol_wire_window_create)) == 0);
	memset(frame + GUI_PROTOCOL_HEADER_SIZE, 0,
	       sizeof(gui_protocol_wire_window_create));
	put32(frame + 8, request_id);
	put32(frame + 12, width);
	put32(frame + 16, height);
	write_full(fd, frame, 24);
	read_frame(fd, frame, GUI_PROTOCOL_WINDOW_CREATE_REPLY);
	window_id = get32(frame + 12);
	assert(get32(frame + 8) == request_id && window_id != 0);
	read_frame(fd, frame, GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get32(frame + 8) == window_id && get32(frame + 12) == 1);
	read_frame(fd, frame, GUI_PROTOCOL_INPUT_RESET);
	assert(get32(frame) == GUI_PROTOCOL_HEADER_SIZE);
	/* Keep fixture geometry independent of the shell's ordinary diagnostics. */
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_MOVE,
		       sizeof(gui_protocol_wire_window_move)) == 0);
	put32(frame + 8, window_id);
	put32(frame + 12, 64 + placement * 32);
	put32(frame + 16, 64 + placement * 16);
	write_full(fd, frame, 20);
	return window_id;
}

static void begin_surface(int fd, uint32_t window_id, uint32_t width, uint32_t height,
			  uint64_t frame_serial)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) +
		      sizeof(gui_protocol_wire_surface_begin)];

	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_SURFACE_BEGIN,
				  sizeof(gui_protocol_wire_surface_begin)) == 0);
	put32(frame + 8, window_id);
	put64(frame + 12, 1);
	put64(frame + 20, frame_serial);
	put32(frame + 28, 0);
	put32(frame + 32, 0);
	put32(frame + 36, width);
	put32(frame + 40, height);
	write_full(fd, frame, sizeof(frame));
}

static void read_error(int fd, int status, gui_protocol_type failed_type)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) + sizeof(gui_protocol_wire_error)];

	read_frame(fd, frame, GUI_PROTOCOL_ERROR);
	assert((int32_t)get32(frame + 8) == status);
	assert(get16(frame + 12) == failed_type);
}

static void read_launch_result(int fd, uint32_t request_id, int status)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) +
		      sizeof(gui_protocol_wire_launch_result)];

	read_frame(fd, frame, GUI_PROTOCOL_LAUNCH_RESULT);
	assert(get32(frame + 8) == request_id && (int32_t)get32(frame + 12) == status);
}

static void read_surface_data_reply(int fd, uint32_t window_id, uint32_t pixel_offset,
				    uint32_t pixel_count, uint64_t frame_serial)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) +
		      sizeof(gui_protocol_wire_surface_data_reply)];

	read_frame(fd, frame, GUI_PROTOCOL_SURFACE_DATA_REPLY);
	assert(get32(frame + 8) == window_id && get32(frame + 12) == 1 &&
	       get32(frame + 20) == (uint32_t)frame_serial &&
	       get32(frame + 24) == (uint32_t)(frame_serial >> 32) &&
	       get32(frame + 28) == pixel_offset && get32(frame + 32) == pixel_count);
}


static void send_surface_data(int fd, uint32_t window_id, uint32_t pixel_offset,
			      uint32_t pixel_count,
			      uint64_t frame_serial, gui_color color)
{
	size_t payload_bytes = sizeof(gui_protocol_wire_surface_data) +
		pixel_count * sizeof(gui_color);

	assert(pixel_count <= SURFACE_PIXELS_PER_MESSAGE);
	assert(gui_protocol_encode_header(surface_frame, sizeof(surface_frame), GUI_PROTOCOL_SURFACE_DATA,
				  payload_bytes) == 0);
	put32(surface_frame + 8, window_id);
	put64(surface_frame + 12, 1);
	put64(surface_frame + 20, frame_serial);
	put32(surface_frame + 28, pixel_offset);
	put32(surface_frame + 32, pixel_count);
	for (size_t index = 0; index < pixel_count; index++)
		put32(surface_frame + 36 + index * sizeof(gui_color), color);
	write_full(fd, surface_frame, GUI_PROTOCOL_HEADER_SIZE + payload_bytes);
}

static void commit_surface(int fd, uint32_t window_id, uint64_t frame_serial)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) +
		      sizeof(gui_protocol_wire_surface_commit)];

	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_SURFACE_COMMIT,
				  sizeof(gui_protocol_wire_surface_commit)) == 0);
	put32(frame + 8, window_id);
	put64(frame + 12, 1);
	put64(frame + 20, frame_serial);
	write_full(fd, frame, sizeof(frame));
}

static void commit_color_surface(int fd, uint32_t window_id, uint32_t width, uint32_t height,
				 gui_color color)
{
	uint32_t total = width * height;

	begin_surface(fd, window_id, width, height, 1);
	for (uint32_t offset = 0; offset < total;) {
		uint32_t count = total - offset > SURFACE_PIXELS_PER_MESSAGE ?
			SURFACE_PIXELS_PER_MESSAGE : total - offset;

		send_surface_data(fd, window_id, offset, count, 1, color);
		read_surface_data_reply(fd, window_id, offset, count, 1);
		offset += count;
	}
	commit_surface(fd, window_id, 1);
}

static void read_configure(int fd, uint32_t window_id, uint64_t serial,
			   uint32_t width, uint32_t height)
{
	uint8_t frame[64];

	for (;;) {
		gui_protocol_header header;

		read_full(fd, frame, GUI_PROTOCOL_HEADER_SIZE);
		assert(gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE, &header) == 0);
		assert(header.length <= sizeof(frame));
		read_full(fd, frame + GUI_PROTOCOL_HEADER_SIZE,
			header.length - GUI_PROTOCOL_HEADER_SIZE);
		if (header.type == GUI_PROTOCOL_WINDOW_CONFIGURE) {
			assert(get32(frame + 8) == window_id && get64(frame + 12) == serial &&
			       get32(frame + 20) == width && get32(frame + 24) == height);
			return;
		}
		/* Pointer motion before a border press still follows ordinary focus routing. */
		assert(header.type == GUI_PROTOCOL_INPUT_EVENT);
	}
}

static void read_close_request(int fd, uint32_t window_id)
{
	uint8_t frame[64];

	for (;;) {
		gui_protocol_header header;

		read_full(fd, frame, GUI_PROTOCOL_HEADER_SIZE);
		assert(gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE, &header) == 0);
		assert(header.length <= sizeof(frame));
		read_full(fd, frame + GUI_PROTOCOL_HEADER_SIZE,
			header.length - GUI_PROTOCOL_HEADER_SIZE);
		if (header.type == GUI_PROTOCOL_WINDOW_CLOSE_REQUEST) {
			assert(get32(frame + 8) == window_id);
			return;
		}
		/* Super-W may follow a release or modifier event; neither closes the window. */
		assert(header.type == GUI_PROTOCOL_INPUT_EVENT);
	}
}

static void destroy_window(int fd, uint32_t window_id)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) + sizeof(gui_protocol_wire_window_id)];

	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_DESTROY,
		sizeof(gui_protocol_wire_window_id)) == 0);
	put32(frame + GUI_PROTOCOL_HEADER_SIZE, window_id);
	write_full(fd, frame, sizeof(frame));
}

static void test_staging(void)
{
	int stager = connect_client();
	int large[3];
	uint32_t stager_id;
	uint32_t large_id[3];

	stager_id = create_window(stager, 9, 64, 48);
	begin_surface(stager, stager_id, UINT32_MAX, 48, 1);
	read_error(stager, -EINVAL, GUI_PROTOCOL_SURFACE_BEGIN);
	begin_surface(stager, stager_id, 64, 48, 1);
	begin_surface(stager, stager_id, 64, 48, 2);
	read_error(stager, -EBUSY, GUI_PROTOCOL_SURFACE_BEGIN);
	for (size_t index = 0; index < sizeof(large) / sizeof(large[0]); index++) {
		large[index] = connect_client();
		large_id[index] = create_window(large[index], (uint32_t)(10 + index),
			1024, 768);
		begin_surface(large[index], large_id[index], 1024, 768, 1);
	}
	read_error(large[2], -ENOMEM, GUI_PROTOCOL_SURFACE_BEGIN);
	for (size_t index = 0; index < sizeof(large) / sizeof(large[0]); index++)
		close(large[index]);
	close(stager);
}

static void test_commit(void)
{
	int fd = connect_client();
	uint32_t window_id;

	window_id = create_window(fd, 20, 2, 2);
	begin_surface(fd, window_id, 2, 2, 1);
	send_surface_data(fd, window_id, 1, 1, 1, 0x00ff0000);
	read_error(fd, -EINVAL, GUI_PROTOCOL_SURFACE_DATA);
	send_surface_data(fd, window_id, 0, 4, 1, 0x00ff0000);
	read_surface_data_reply(fd, window_id, 0, 4, 1);
	commit_surface(fd, window_id, 1);
	begin_surface(fd, window_id, 2, 2, 2);
	/* A stale update cannot overwrite the committed red frame. */
	send_surface_data(fd, window_id, 0, 1, 1, 0x000000ff);
	read_error(fd, -EINVAL, GUI_PROTOCOL_SURFACE_DATA);
	commit_surface(fd, window_id, 1);
	read_error(fd, -EINVAL, GUI_PROTOCOL_SURFACE_COMMIT);
	/* Dropping a partially transferred replacement must retain the old frame. */
	send_surface_data(fd, window_id, 0, 1, 2, 0x000000ff);
	read_surface_data_reply(fd, window_id, 0, 1, 2);
	/* Keep the owner alive while the visual test proves staging did not leak. */
	puts("wm-client: commit ok");
	fflush(stdout);
	for (;;)
		pause();
}

static void test_recovery(void)
{
	int fd = connect_client();
	uint32_t window_id;

	window_id = create_window(fd, 21, 64, 48);
	commit_color_surface(fd, window_id, 64, 48, 0x00ff0000);
	puts("wm-client: recovery ready");
	fflush(stdout);
	/* Keep the committed owner alive while the shell is replaced. */
	for (;;)
		pause();
}

static void test_public_shell(void)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) +
		      sizeof(gui_protocol_wire_desktop_create)];
	int fd = connect_client();

	hello(fd);
	assert(gui_protocol_encode_header(frame, sizeof(frame),
		GUI_PROTOCOL_DESKTOP_CREATE,
		sizeof(gui_protocol_wire_desktop_create)) == 0);
	put32(frame + GUI_PROTOCOL_HEADER_SIZE, 1);
	write_full(fd, frame, sizeof(frame));
	read_error(fd, -EPERM, GUI_PROTOCOL_DESKTOP_CREATE);
	assert(gui_protocol_encode_header(frame, sizeof(frame),
		GUI_PROTOCOL_SHELL_ACTIVATE_WINDOW, sizeof(gui_protocol_wire_window_id)) == 0);
	put32(frame + GUI_PROTOCOL_HEADER_SIZE, 1);
	write_full(fd, frame, 12);
	read_error(fd, -EPERM, GUI_PROTOCOL_SHELL_ACTIVATE_WINDOW);
	close(fd);
}

static void test_stack(void)
{
	int back = connect_client();
	int front;
	uint8_t frame[64];
	uint32_t back_id;
	uint32_t front_id;

	back_id = create_window(back, 30, 64, 48);
	commit_color_surface(back, back_id, 64, 48, 0x00ff0000);
	front = connect_client();
	front_id = create_window(front, 31, 64, 48);
	commit_color_surface(front, front_id, 64, 48, 0x0000ff00);
	/* Creating the front window transfers the initial focus away from back. */
	read_frame(back, frame, GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get32(frame + 12) == 0);
	read_frame(back, frame, GUI_PROTOCOL_INPUT_RESET);
	puts("wm-client: stack ready");
	fflush(stdout);
	/* A click on the exposed red corner raises and focuses the back window. */
	read_frame(back, frame, GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get32(frame + 12) == 1);
	read_frame(back, frame, GUI_PROTOCOL_INPUT_RESET);
	read_frame(back, frame, GUI_PROTOCOL_INPUT_EVENT);
	assert(get32(frame + 12) == GUI_INPUT_EVENT_MOUSE && frame[32] == 1);
	puts("wm-client: click focus ready");
	fflush(stdout);
	for (;;)
		pause();
}

static void test_taskbar(void)
{
	int back = connect_client();
	int front;
	uint8_t frame[64];
	struct pollfd pollfd;
	uint32_t back_id;
	uint32_t front_id;

	back_id = create_window(back, 40, 64, 48);
	commit_color_surface(back, back_id, 64, 48, 0x00ff0000);
	front = connect_client();
	front_id = create_window(front, 41, 64, 48);
	commit_color_surface(front, front_id, 64, 48, 0x0000ff00);
	/* The second create focused front, leaving back as an inactive task button. */
	read_frame(back, frame, GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get32(frame + 12) == 0);
	read_frame(back, frame, GUI_PROTOCOL_INPUT_RESET);
	puts("wm-client: taskbar ready");
	fflush(stdout);
	/* A panel click is delivered to the shell, which requests this activation. */
	read_frame(back, frame, GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get32(frame + 12) == 1);
	read_frame(back, frame, GUI_PROTOCOL_INPUT_RESET);
	pollfd = (struct pollfd) { .fd = back, .events = POLLIN };
	assert(poll(&pollfd, 1, 100) == 0);
	puts("wm-client: taskbar activation ready");
	fflush(stdout);
	for (;;)
		pause();
}

static void test_shortcuts(void)
{
	int back = connect_client();
	int front;
	uint8_t frame[64];
	struct pollfd pollfd;
	uint32_t back_id;

	back_id = create_window(back, 50, 64, 48);
	front = connect_client();
	(void)create_window(front, 51, 64, 48);
	read_frame(back, frame, GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get32(frame + 8) == back_id && get32(frame + 12) == 0);
	read_frame(back, frame, GUI_PROTOCOL_INPUT_RESET);
	puts("wm-client: shortcuts ready");
	fflush(stdout);
	/* Alt reaches the previous owner, but the Tab chord itself changes focus. */
	read_frame(back, frame, GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get32(frame + 8) == back_id && get32(frame + 12) == 1);
	read_frame(back, frame, GUI_PROTOCOL_INPUT_RESET);
	pollfd = (struct pollfd) { .fd = back, .events = POLLIN };
	assert(poll(&pollfd, 1, 100) == 0);
	puts("wm-client: alt-tab intercepted");
	fflush(stdout);
	/* A modifier event is allowed, but Super-W must never forward the W keysym. */
	pollfd = (struct pollfd) { .fd = back, .events = POLLIN };
	while (poll(&pollfd, 1, 100) > 0) {
		read_frame(back, frame, GUI_PROTOCOL_INPUT_EVENT);
		assert(get32(frame + 8) == back_id &&
		       get32(frame + 12) == GUI_INPUT_EVENT_KEY);
		assert(get32(frame + 16) != 'w');
		pollfd.revents = 0;
	}
	puts("wm-client: super-w intercepted");
	fflush(stdout);
	for (;;)
		pause();
}

static void test_interact(void)
{
	int fd = connect_client();
	uint32_t window_id;

	window_id = create_window(fd, 60, 64, 48);
	commit_color_surface(fd, window_id, 64, 48, 0x00ff0000);
	puts("wm-client: interact ready");
	fflush(stdout);
	/* Each border drag emits one server-owned configure with its next serial. */
	read_configure(fd, window_id, 2, 84, 68);
	puts("wm-client: top-left resize ready");
	fflush(stdout);
	read_configure(fd, window_id, 3, 104, 88);
	puts("wm-client: top-right resize ready");
	fflush(stdout);
	read_configure(fd, window_id, 4, 124, 108);
	puts("wm-client: bottom-left resize ready");
	fflush(stdout);
	read_configure(fd, window_id, 5, 144, 128);
	puts("wm-client: bottom-right resize ready");
	fflush(stdout);
	read_configure(fd, window_id, 6, 64, 48);
	puts("wm-client: minimum resize ready");
	fflush(stdout);
	/* The first request is deferred; the second cooperatively destroys the owner. */
	read_close_request(fd, window_id);
	puts("wm-client: close deferred");
	fflush(stdout);
	read_close_request(fd, window_id);
	destroy_window(fd, window_id);
	puts("wm-client: close accepted");
	fflush(stdout);
	for (;;)
		pause();
}

static void test_launch(void)
{
	static const char marker[] = "/tmp/mario-wm-launch-marker";
	static const char image[] = "/tmp/mario-wm-launch-marker.bmp";
	uint8_t frame[64] = { 0 };
	int fd = connect_client();
	size_t path_bytes = sizeof(marker) - 1;

	unlink(marker);
	hello(fd);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_LAUNCH_REQUEST,
		       sizeof(gui_protocol_wire_launch_request)) == 0);
	put32(frame + 8, 1);
	put16(frame + 12, GUI_PROTOCOL_LAUNCH_TERMINAL);
	write_full(fd, frame, 16);
	read_launch_result(fd, 1, 0);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_LAUNCH_REQUEST,
		       sizeof(gui_protocol_wire_launch_request) + path_bytes) == 0);
	put32(frame + 8, 2);
	put16(frame + 12, GUI_PROTOCOL_LAUNCH_EXECUTABLE);
	put16(frame + 14, path_bytes);
	memcpy(frame + 16, marker, path_bytes);
	write_full(fd, frame, 16 + path_bytes);
	read_launch_result(fd, 2, -ENOSYS);
	assert(access(marker, F_OK) < 0 && errno == ENOENT);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_LAUNCH_REQUEST,
		       sizeof(gui_protocol_wire_launch_request) + sizeof(image) - 1) == 0);
	put32(frame + 8, 3);
	put16(frame + 12, GUI_PROTOCOL_LAUNCH_IMAGE_VIEWER);
	put16(frame + 14, sizeof(image) - 1);
	memcpy(frame + 16, image, sizeof(image) - 1);
	write_full(fd, frame, 16 + sizeof(image) - 1);
	read_launch_result(fd, 3, 0);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_LAUNCH_REQUEST,
		       sizeof(gui_protocol_wire_launch_request) + 1) == 0);
	put32(frame + 8, 4);
	put16(frame + 12, GUI_PROTOCOL_LAUNCH_TERMINAL);
	put16(frame + 14, 1);
	frame[16] = '/';
	write_full(fd, frame, 17);
	read_error(fd, -EINVAL, GUI_PROTOCOL_LAUNCH_REQUEST);
	close(fd);
}

int main(int argc, char **argv)
{
	uint8_t frame[64];
	int first;
	int second;
	uint32_t first_id;

	assert(argc == 1 || (argc == 2 && (!strcmp(argv[1], "--surface-only") ||
		!strcmp(argv[1], "--commit-only") ||
		!strcmp(argv[1], "--recovery-only") ||
		!strcmp(argv[1], "--public-shell-only") || !strcmp(argv[1], "--stack-only") ||
		!strcmp(argv[1], "--taskbar-only") || !strcmp(argv[1], "--shortcuts-only") ||
		!strcmp(argv[1], "--interact-only") || !strcmp(argv[1], "--launch-only"))));
	if (argc == 2) {
		if (!strcmp(argv[1], "--surface-only")) {
			test_staging();
			puts("wm-client: staging ok");
		} else if (!strcmp(argv[1], "--commit-only")) {
			test_commit();
		} else if (!strcmp(argv[1], "--recovery-only")) {
			test_recovery();
		} else if (!strcmp(argv[1], "--stack-only")) {
			test_stack();
		} else if (!strcmp(argv[1], "--taskbar-only")) {
			test_taskbar();
		} else if (!strcmp(argv[1], "--shortcuts-only")) {
			test_shortcuts();
		} else if (!strcmp(argv[1], "--interact-only")) {
			test_interact();
		} else if (!strcmp(argv[1], "--launch-only")) {
			test_launch();
			puts("wm-client: launch requests handled safely");
		} else {
			test_public_shell();
			puts("wm-client: public shell denied");
		}
		return 0;
	}
	first = connect_client();

	first_id = create_window(first, 7, 64, 48);
	second = connect_client();
	(void)create_window(second, 8, 64, 48);
	read_frame(first, frame, GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get32(frame + 8) == first_id && get32(frame + 12) == 0);
	read_frame(first, frame, GUI_PROTOCOL_INPUT_RESET);
	close(second);
	/* The first owner must be reset again before it can receive later input. */
	read_frame(first, frame, GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get32(frame + 8) == first_id && get32(frame + 12) == 1);
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
	assert(get32(frame + 8) == first_id && get32(frame + 12) == GUI_INPUT_EVENT_KEY);
	/* GUI-TYPE sends the matching key release after the key press. */
	read_frame(first, frame, GUI_PROTOCOL_INPUT_EVENT);
	assert(get32(frame + 8) == first_id && get32(frame + 12) == GUI_INPUT_EVENT_KEY);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_DESTROY, 4) == 0);
	put32(frame + 8, first_id);
	write_full(first, frame, 12);
	close(first);
	puts("wm-client: ownership resets ok");
	return 0;
}
