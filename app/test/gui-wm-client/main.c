#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <gui/input.h>
#include <gui/protocol.h>
#include <gui/surface.h>
#include <app/gui-limits.h>

#define SURFACE_PIXELS_PER_MESSAGE 1000U

/* Large pixel runs must not consume the small initial userspace stack. */
static uint8_t surface_frame[sizeof(gui_protocol_wire_header) +
			     sizeof(gui_protocol_wire_surface_data) +
			     SURFACE_PIXELS_PER_MESSAGE * sizeof(gui_color)];
static unsigned int test_window_placement;

_Noreturn void __assert_fail(const char *expression, const char *file, int line,
		const char *function)
{
	fprintf(stderr, "wm-client: assertion failed: %s:%d: %s: %s\n",
		file, line, function, expression);
	fflush(NULL);
	_exit(1);
}

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

static void write_zeroes(int fd, size_t byte_count)
{
	uint8_t zeroes[4096] = { 0 };

	while (byte_count != 0) {
		size_t count = byte_count < sizeof(zeroes) ? byte_count : sizeof(zeroes);

		write_full(fd, zeroes, count);
		byte_count -= count;
	}
}

static void let_server_observe_disconnect(void)
{
	/* The following connection must measure post-disconnect surface accounting. */
	assert(usleep(100000) == 0);
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

static void begin_surface_configured(int fd, uint32_t window_id, uint32_t width,
		uint32_t height, uint64_t configure_serial, uint64_t frame_serial)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) +
		      sizeof(gui_protocol_wire_surface_begin)];

	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_SURFACE_BEGIN,
				  sizeof(gui_protocol_wire_surface_begin)) == 0);
	put32(frame + 8, window_id);
	put64(frame + 12, configure_serial);
	put64(frame + 20, frame_serial);
	put32(frame + 28, 0);
	put32(frame + 32, 0);
	put32(frame + 36, width);
	put32(frame + 40, height);
	write_full(fd, frame, sizeof(frame));
}

static void begin_surface(int fd, uint32_t window_id, uint32_t width, uint32_t height,
			  uint64_t frame_serial)
{
	begin_surface_configured(fd, window_id, width, height, 1, frame_serial);
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

static void read_surface_data_reply_configured(int fd, uint32_t window_id,
		uint32_t pixel_offset, uint32_t pixel_count,
		uint64_t configure_serial, uint64_t frame_serial)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) +
		      sizeof(gui_protocol_wire_surface_data_reply)];

	read_frame(fd, frame, GUI_PROTOCOL_SURFACE_DATA_REPLY);
	assert(get32(frame + 8) == window_id &&
	       get32(frame + 12) == (uint32_t)configure_serial &&
	       get32(frame + 16) == (uint32_t)(configure_serial >> 32) &&
	       get32(frame + 20) == (uint32_t)frame_serial &&
	       get32(frame + 24) == (uint32_t)(frame_serial >> 32) &&
	       get32(frame + 28) == pixel_offset && get32(frame + 32) == pixel_count);
}

static void read_surface_data_reply(int fd, uint32_t window_id, uint32_t pixel_offset,
				    uint32_t pixel_count, uint64_t frame_serial)
{
	read_surface_data_reply_configured(fd, window_id, pixel_offset, pixel_count,
		1, frame_serial);
}


static void send_surface_data_configured(int fd, uint32_t window_id,
		uint32_t pixel_offset, uint32_t pixel_count, uint64_t configure_serial,
		uint64_t frame_serial, gui_color color)
{
	size_t payload_bytes = sizeof(gui_protocol_wire_surface_data) +
		pixel_count * sizeof(gui_color);

	assert(pixel_count <= SURFACE_PIXELS_PER_MESSAGE);
	assert(gui_protocol_encode_header(surface_frame, sizeof(surface_frame), GUI_PROTOCOL_SURFACE_DATA,
				  payload_bytes) == 0);
	put32(surface_frame + 8, window_id);
	put64(surface_frame + 12, configure_serial);
	put64(surface_frame + 20, frame_serial);
	put32(surface_frame + 28, pixel_offset);
	put32(surface_frame + 32, pixel_count);
	for (size_t index = 0; index < pixel_count; index++)
		put32(surface_frame + 36 + index * sizeof(gui_color), color);
	write_full(fd, surface_frame, GUI_PROTOCOL_HEADER_SIZE + payload_bytes);
}

static void send_surface_data(int fd, uint32_t window_id, uint32_t pixel_offset,
		uint32_t pixel_count, uint64_t frame_serial, gui_color color)
{
	send_surface_data_configured(fd, window_id, pixel_offset, pixel_count,
		1, frame_serial, color);
}

static void commit_surface_configured(int fd, uint32_t window_id,
		uint64_t configure_serial, uint64_t frame_serial)
{
	uint8_t frame[sizeof(gui_protocol_wire_header) +
		      sizeof(gui_protocol_wire_surface_commit)];

	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_SURFACE_COMMIT,
				  sizeof(gui_protocol_wire_surface_commit)) == 0);
	put32(frame + 8, window_id);
	put64(frame + 12, configure_serial);
	put64(frame + 20, frame_serial);
	write_full(fd, frame, sizeof(frame));
}

static void commit_surface(int fd, uint32_t window_id, uint64_t frame_serial)
{
	commit_surface_configured(fd, window_id, 1, frame_serial);
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
			if (get32(frame + 8) != window_id ||
			    get64(frame + 12) != serial ||
			    get32(frame + 20) != width ||
			    get32(frame + 24) != height) {
				printf("wm-client: configure mismatch: expected %u/%llu/%u/%u got %u/%llu/%u/%u\n",
					window_id, (unsigned long long)serial, width, height,
					get32(frame + 8), (unsigned long long)get64(frame + 12),
					get32(frame + 20), get32(frame + 24));
				fflush(stdout);
				_exit(2);
			}
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
		send_surface_data(large[index], large_id[index], 0, 1, 1, 0x00123456);
		read_surface_data_reply(large[index], large_id[index], 0, 1, 1);
	}
	for (size_t index = 0; index < sizeof(large) / sizeof(large[0]); index++)
		close(large[index]);
	close(stager);
}

static void budget_reply(int fd, uint8_t *frame, gui_protocol_type expected)
{
	for (;;) {
		gui_protocol_header header;

		read_full(fd, frame, GUI_PROTOCOL_HEADER_SIZE);
		assert(gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE, &header) == 0);
		assert(header.length <= 64);
		read_full(fd, frame + GUI_PROTOCOL_HEADER_SIZE, header.length - GUI_PROTOCOL_HEADER_SIZE);
		if (header.type == expected)
			return;
		assert(header.type == GUI_PROTOCOL_WINDOW_FOCUS ||
			header.type == GUI_PROTOCOL_INPUT_RESET || header.type == GUI_PROTOCOL_INPUT_EVENT);
	}
}

static void budget_fill(int fd, uint32_t window_id, uint32_t width, uint32_t height,
		uint64_t configure_serial, uint64_t frame_serial, gui_color color)
{
	uint32_t total = width * height;
	uint8_t reply[64];

	for (uint32_t offset = 0; offset < total;) {
		uint32_t count = total - offset > SURFACE_PIXELS_PER_MESSAGE ?
			SURFACE_PIXELS_PER_MESSAGE : total - offset;

		send_surface_data_configured(fd, window_id, offset, count,
			configure_serial, frame_serial, color);
		budget_reply(fd, reply, GUI_PROTOCOL_SURFACE_DATA_REPLY);
		assert(get32(reply + 8) == window_id && get64(reply + 12) == configure_serial &&
			get64(reply + 20) == frame_serial && get32(reply + 28) == offset &&
			get32(reply + 32) == count);
		offset += count;
	}
	commit_surface_configured(fd, window_id, configure_serial, frame_serial);
}

static void budget_marker(const char *path)
{
	FILE *file = fopen(path, "w");

	assert(file != NULL && fputs("ready\n", file) >= 0 && fclose(file) == 0);
}

static void budget_wait(const char *path)
{
	struct timespec start;
	struct timespec now;

	assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	while (access(path, F_OK) != 0) {
		assert(usleep(50000) == 0);
		assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
		assert(now.tv_sec - start.tv_sec < 300);
	}
}

static void test_surface_budget(void)
{
	int peers[4];
	uint32_t windows[4];
	uint8_t reply[64];
	uint8_t resize[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_resize)];
	uint32_t width = 1016;
	uint32_t height = 736;

	assert(app_gui_surface_budget(1024 * 768 * 4) == 16 * 1024 * 768 * 4);
	for (size_t index = 0; index < 4; index++) {
		peers[index] = connect_client();
		windows[index] = create_window(peers[index], (uint32_t)index + 100, width, height);
		begin_surface(peers[index], windows[index], width, height, 1);
		printf("wm-client: reserved near-screen window %zu\n", index + 1);
		fflush(stdout);
	}
	for (size_t index = 0; index < 4; index++) {
		budget_fill(peers[index], windows[index], width, height, 1, 1, 0x00225688);
		printf("wm-client: committed near-screen window %zu\n", index + 1);
		fflush(stdout);
	}
	for (size_t index = 0; index < 4; index++)
		begin_surface(peers[index], windows[index], width, height, 2);
	for (size_t index = 0; index < 4; index++) {
		send_surface_data(peers[index], windows[index], 0, 1, 2, 0x00dd8844);
		budget_reply(peers[index], reply, GUI_PROTOCOL_SURFACE_DATA_REPLY);
		assert(get32(reply + 8) == windows[index] && get32(reply + 28) == 0);
	}
	budget_marker("/tmp/gui-budget.staged");
	puts("wm-client: overlapping staging ready");
	fflush(stdout);
	budget_wait("/tmp/gui-budget.paint");
	for (size_t index = 0; index < 4; index++) {
		assert(gui_protocol_encode_header(resize, sizeof(resize), GUI_PROTOCOL_WINDOW_RESIZE,
			sizeof(gui_protocol_wire_window_resize)) == 0);
		put32(resize + 8, windows[index]);
		put32(resize + 12, width - 2);
		put32(resize + 16, height - 2);
		write_full(peers[index], resize, sizeof(resize));
		budget_reply(peers[index], reply, GUI_PROTOCOL_WINDOW_CONFIGURE);
		assert(get32(reply + 8) == windows[index] && get64(reply + 12) == 2);
		assert(get32(reply + 20) == width - 2 && get32(reply + 24) == height - 2);
		begin_surface(peers[index], windows[index], width, height, 3);
		budget_reply(peers[index], reply, GUI_PROTOCOL_ERROR);
		assert((int32_t)get32(reply + 8) == -EINVAL &&
			get16(reply + 12) == GUI_PROTOCOL_SURFACE_BEGIN);
		send_surface_data(peers[index], windows[index], 0, 1, 2, 0x00ffffff);
		budget_reply(peers[index], reply, GUI_PROTOCOL_ERROR);
		assert((int32_t)get32(reply + 8) == -EINVAL &&
			get16(reply + 12) == GUI_PROTOCOL_SURFACE_DATA);
		commit_surface(peers[index], windows[index], 2);
		budget_reply(peers[index], reply, GUI_PROTOCOL_ERROR);
		assert((int32_t)get32(reply + 8) == -EINVAL &&
			get16(reply + 12) == GUI_PROTOCOL_SURFACE_COMMIT);
		begin_surface_configured(peers[index], windows[index], width - 2, height - 2, 2, 3);
	}
	width -= 2;
	height -= 2;
	for (uint64_t round = 3; round <= 4; round++) {
		for (size_t index = 0; index < 4; index++)
			budget_fill(peers[index], windows[index], width, height, 2, round,
				round == 3 ? 0x00dd8844 : 0x00448866);
		if (round == 3) {
			for (size_t index = 0; index < 4; index++)
				begin_surface_configured(peers[index], windows[index], width, height, 2, 4);
		}
	}
	for (size_t index = 0; index < 4; index++) {
		begin_surface_configured(peers[index], windows[index], width, height, 2, 5);
		send_surface_data_configured(peers[index], windows[index], 0, 1, 2, 5, 0x00448866);
		budget_reply(peers[index], reply, GUI_PROTOCOL_SURFACE_DATA_REPLY);
		assert(get32(reply + 8) == windows[index] && get64(reply + 12) == 2 &&
			get64(reply + 20) == 5 && get32(reply + 28) == 0 && get32(reply + 32) == 1);
	}
	budget_marker("/tmp/gui-budget.painted");
	budget_wait("/tmp/gui-budget.release");
	for (size_t index = 0; index < 4; index++)
		close(peers[index]);
	let_server_observe_disconnect();
	for (size_t index = 0; index < 4; index++) {
		peers[index] = connect_client();
		windows[index] = create_window(peers[index], (uint32_t)index + 200, 1022, 742);
		begin_surface(peers[index], windows[index], 1022, 742, 1);
		send_surface_data(peers[index], windows[index], 0, 1, 1, 0x00123456);
		budget_reply(peers[index], reply, GUI_PROTOCOL_SURFACE_DATA_REPLY);
		assert(get32(reply + 8) == windows[index]);
	}
	for (size_t index = 0; index < 4; index++)
		close(peers[index]);
	puts("wm-client: surface workload and reclamation pass");
	budget_marker("/tmp/gui-budget.done");
}

static uint32_t lifecycle_create(int fd, uint32_t request_id, uint32_t width, uint32_t height)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create)] = { 0 };
	uint8_t reply[64];

	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_CREATE,
		sizeof(gui_protocol_wire_window_create)) == 0);
	put32(frame + 8, request_id);
	put32(frame + 12, width);
	put32(frame + 16, height);
	write_full(fd, frame, sizeof(frame));
	budget_reply(fd, reply, GUI_PROTOCOL_WINDOW_CREATE_REPLY);
	assert(get32(reply + 8) == request_id && get32(reply + 12) != 0);
	assert(get64(reply + 16) == 1 && get32(reply + 24) == width && get32(reply + 28) == height);
	return get32(reply + 12);
}

static void lifecycle_move(int fd, uint32_t window_id, uint32_t x, uint32_t y)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_move)];

	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_MOVE,
		sizeof(gui_protocol_wire_window_move)) == 0);
	put32(frame + 8, window_id);
	put32(frame + 12, x);
	put32(frame + 16, y);
	write_full(fd, frame, sizeof(frame));
}

static gui_color lifecycle_color(unsigned int index)
{
	static const gui_color colors[] = { 0x00225688, 0x00448866, 0x00dd8844, 0x008855aa };

	return colors[index % (sizeof(colors) / sizeof(colors[0]))];
}

static void lifecycle_reply(int fd, uint32_t window_id, uint64_t configure_serial,
			    uint64_t frame_serial, uint32_t offset, uint32_t count)
{
	uint8_t reply[64];

	budget_reply(fd, reply, GUI_PROTOCOL_SURFACE_DATA_REPLY);
	assert(get32(reply + 8) == window_id && get64(reply + 12) == configure_serial);
	assert(get64(reply + 20) == frame_serial && get32(reply + 28) == offset && get32(reply + 32) == count);
}

static void lifecycle_finish(int fd, uint32_t window_id, uint64_t frame_serial, gui_color color)
{
	uint32_t total = 96 * 64;

	for (uint32_t offset = 1; offset < total;) {
		uint32_t count = total - offset > SURFACE_PIXELS_PER_MESSAGE ?
			SURFACE_PIXELS_PER_MESSAGE : total - offset;

		send_surface_data(fd, window_id, offset, count, frame_serial, color);
		lifecycle_reply(fd, window_id, 1, frame_serial, offset, count);
		offset += count;
	}
	commit_surface(fd, window_id, frame_serial);
}

static void lifecycle_paint(int *peers, uint32_t *windows)
{
	for (unsigned int index = 0; index < 40; index++)
		begin_surface(peers[index], windows[index], 96, 64, 1);
	for (unsigned int index = 0; index < 40; index++) {
		send_surface_data(peers[index], windows[index], 0, 1, 1, lifecycle_color(index));
		lifecycle_reply(peers[index], windows[index], 1, 1, 0, 1);
	}
	for (unsigned int index = 0; index < 40; index++)
		lifecycle_finish(peers[index], windows[index], 1, lifecycle_color(index));
	for (unsigned int index = 0; index < 40; index++) {
		begin_surface(peers[index], windows[index], 96, 64, 2);
		send_surface_data(peers[index], windows[index], 0, 1, 2, 0x00448866);
		lifecycle_reply(peers[index], windows[index], 1, 2, 0, 1);
	}
}

static void lifecycle_error(int fd, gui_protocol_type type)
{
	uint8_t reply[64];

	budget_reply(fd, reply, GUI_PROTOCOL_ERROR);
	assert((int32_t)get32(reply + 8) == -EINVAL && get16(reply + 12) == type);
}

static void lifecycle_invalid_window(int fd, uint32_t window_id, gui_protocol_type type)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_begin)] = { 0 };
	size_t payload_bytes;

	switch (type) {
	case GUI_PROTOCOL_WINDOW_MOVE:
		payload_bytes = sizeof(gui_protocol_wire_window_move);
		break;
	case GUI_PROTOCOL_WINDOW_RESIZE:
		payload_bytes = sizeof(gui_protocol_wire_window_resize);
		break;
	case GUI_PROTOCOL_WINDOW_DESTROY:
		payload_bytes = sizeof(gui_protocol_wire_window_id);
		break;
	case GUI_PROTOCOL_SURFACE_BEGIN:
		payload_bytes = sizeof(gui_protocol_wire_surface_begin);
		break;
	default:
		assert(0);
		return;
	}
	assert(gui_protocol_encode_header(frame, sizeof(frame), type, payload_bytes) == 0);
	put32(frame + 8, window_id);
	write_full(fd, frame, GUI_PROTOCOL_HEADER_SIZE + payload_bytes);
	lifecycle_error(fd, type);
}

static void test_window_lifecycle(void)
{
	int peers[40];
	uint32_t windows[40];
	uint8_t reply[64];
	uint8_t resize[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_resize)];
	int shared = connect_client();
	int outsider;
	uint32_t outsider_window;
	uint32_t last_identity;

	assert(40 * 2 * 96 * 64 * sizeof(gui_color) <= app_gui_peer_surface_budget(1024 * 768 * 4));
	hello(shared);
	for (unsigned int index = 0; index < 40; index++) {
		peers[index] = shared;
		windows[index] = lifecycle_create(shared, 1000 + index, 96, 64);
		assert(index == 0 || windows[index] > windows[index - 1]);
		lifecycle_move(shared, windows[index], 16 + index % 8 * 112, 48 + index / 8 * 96);
	}
	lifecycle_paint(peers, windows);
	puts("wm-client: 40 sibling windows and interleaved stages ready");
	fflush(stdout);
	budget_marker("/tmp/gui-lifecycle.multi");
	budget_wait("/tmp/gui-lifecycle.mutate");
	assert(gui_protocol_encode_header(resize, sizeof(resize), GUI_PROTOCOL_WINDOW_RESIZE,
		sizeof(gui_protocol_wire_window_resize)) == 0);
	put32(resize + 8, windows[0]);
	put32(resize + 12, 112);
	put32(resize + 16, 80);
	write_full(shared, resize, sizeof(resize));
	budget_reply(shared, reply, GUI_PROTOCOL_WINDOW_CONFIGURE);
	assert(get32(reply + 8) == windows[0] && get64(reply + 12) == 2);
	assert(get32(reply + 20) == 112 && get32(reply + 24) == 80);
	begin_surface(shared, windows[0], 96, 64, 3);
	lifecycle_error(shared, GUI_PROTOCOL_SURFACE_BEGIN);
	send_surface_data(shared, windows[0], 1, 1, 2, 0x00ff0000);
	lifecycle_error(shared, GUI_PROTOCOL_SURFACE_DATA);
	commit_surface(shared, windows[0], 2);
	lifecycle_error(shared, GUI_PROTOCOL_SURFACE_COMMIT);
	begin_surface_configured(shared, windows[0], 112, 80, 2, 4);
	budget_fill(shared, windows[0], 112, 80, 2, 4, 0x00cc4433);
	begin_surface_configured(shared, windows[0], 112, 80, 2, 5);
	send_surface_data_configured(shared, windows[0], 0, 1, 2, 5, 0x00cc4433);
	lifecycle_reply(shared, windows[0], 2, 5, 0, 1);
	lifecycle_finish(shared, windows[39], 2, 0x00448866);
	begin_surface(shared, windows[39], 96, 64, 3);
	send_surface_data(shared, windows[39], 0, 1, 3, 0x00448866);
	lifecycle_reply(shared, windows[39], 1, 3, 0, 1);
	destroy_window(shared, windows[10]);
	lifecycle_invalid_window(shared, windows[10], GUI_PROTOCOL_WINDOW_MOVE);
	lifecycle_invalid_window(shared, windows[10], GUI_PROTOCOL_WINDOW_DESTROY);
	outsider = connect_client();
	hello(outsider);
	outsider_window = lifecycle_create(outsider, 2000, 64, 48);
	assert(outsider_window > windows[39]);
	lifecycle_move(outsider, outsider_window, 900, 620);
	lifecycle_invalid_window(outsider, windows[0], GUI_PROTOCOL_WINDOW_MOVE);
	lifecycle_invalid_window(outsider, windows[0], GUI_PROTOCOL_WINDOW_RESIZE);
	lifecycle_invalid_window(outsider, windows[0], GUI_PROTOCOL_SURFACE_BEGIN);
	lifecycle_invalid_window(outsider, windows[0], GUI_PROTOCOL_WINDOW_DESTROY);
	begin_surface(outsider, outsider_window, 64, 48, 1);
	budget_fill(outsider, outsider_window, 64, 48, 1, 1, 0x00224466);
	begin_surface(outsider, outsider_window, 64, 48, 2);
	send_surface_data(outsider, outsider_window, 0, 1, 2, 0x00224466);
	lifecycle_reply(outsider, outsider_window, 1, 2, 0, 1);
	last_identity = outsider_window;
	close(outsider);
	puts("wm-client: sibling close, stale serials and cross-peer rejection pass");
	fflush(stdout);
	budget_marker("/tmp/gui-lifecycle.mutated");
	budget_wait("/tmp/gui-lifecycle.connections");
	close(shared);
	let_server_observe_disconnect();
	for (unsigned int index = 0; index < 40; index++) {
		peers[index] = connect_client();
		hello(peers[index]);
		windows[index] = lifecycle_create(peers[index], 3000 + index, 96, 64);
		assert(windows[index] > last_identity);
		last_identity = windows[index];
		lifecycle_move(peers[index], windows[index], 16 + index % 8 * 112, 48 + index / 8 * 96);
	}
	lifecycle_paint(peers, windows);
	puts("wm-client: 40 independent connections and interleaved stages ready");
	fflush(stdout);
	budget_marker("/tmp/gui-lifecycle.many");
	budget_wait("/tmp/gui-lifecycle.finish");
	for (unsigned int index = 0; index < 40; index++)
		close(peers[index]);
	let_server_observe_disconnect();
	outsider = connect_client();
	hello(outsider);
	outsider_window = lifecycle_create(outsider, 4000, 64, 48);
	assert(outsider_window > last_identity);
	begin_surface(outsider, outsider_window, 64, 48, 1);
	budget_fill(outsider, outsider_window, 64, 48, 1, 1, 0x00224466);
	destroy_window(outsider, outsider_window);
	lifecycle_invalid_window(outsider, outsider_window, GUI_PROTOCOL_WINDOW_DESTROY);
	close(outsider);
	puts("wm-client: dynamic lifecycle and single-window reclamation pass");
	fflush(stdout);
	budget_marker("/tmp/gui-lifecycle.done");
}

static uint32_t many_large_create(int fd, uint32_t request_id)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create) +
		GUI_PROTOCOL_MAX_TITLE_BYTES] = { 0 };
	uint8_t reply[64];

	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_CREATE,
		sizeof(gui_protocol_wire_window_create) + GUI_PROTOCOL_MAX_TITLE_BYTES) == 0);
	put32(frame + 8, request_id);
	put32(frame + 12, 1);
	put32(frame + 16, 1);
	put16(frame + 20, GUI_PROTOCOL_MAX_TITLE_BYTES);
	memset(frame + GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create),
		's', GUI_PROTOCOL_MAX_TITLE_BYTES);
	write_full(fd, frame, sizeof(frame));
	budget_reply(fd, reply, GUI_PROTOCOL_WINDOW_CREATE_REPLY);
	assert(get32(reply + 8) == request_id && get64(reply + 16) == 1);
	return get32(reply + 12);
}

static void many_event(int fd, uint32_t *windows, unsigned int count,
		unsigned int *focus_counts, const char *phase)
{
	uint8_t frame[64];
	gui_protocol_header header;
	unsigned int index;

	read_full(fd, frame, GUI_PROTOCOL_HEADER_SIZE);
	assert(gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE, &header) == 0);
	assert(header.length <= sizeof(frame));
	read_full(fd, frame + GUI_PROTOCOL_HEADER_SIZE, header.length - GUI_PROTOCOL_HEADER_SIZE);
	if (header.type == GUI_PROTOCOL_INPUT_RESET || header.type == GUI_PROTOCOL_INPUT_EVENT)
		return;
	assert(header.type == GUI_PROTOCOL_WINDOW_FOCUS || header.type == GUI_PROTOCOL_WINDOW_CLOSE_REQUEST);
	if (header.type == GUI_PROTOCOL_WINDOW_FOCUS && !get32(frame + 12))
		return;
	for (index = 0; index < count && windows[index] != get32(frame + 8); index++)
		;
	assert(index < count && windows[index] != 0);
	if (header.type == GUI_PROTOCOL_WINDOW_FOCUS) {
		if (get32(frame + 12)) {
			printf("wm-client: %s focus %u %u\n", phase, index, ++focus_counts[index]);
			fflush(stdout);
		}
	} else {
		destroy_window(fd, windows[index]);
		windows[index] = 0;
		printf("wm-client: %s close %u\n", phase, index);
		fflush(stdout);
	}
}

static void many_wait(int fd, uint32_t *windows, unsigned int count, int large_fd,
		uint32_t *large_windows, const char *phase, const char *marker)
{
	static unsigned int focus_counts[41];
	static unsigned int large_counts[256];
	unsigned int peer_count = large_fd >= 0 ? 2 : 1;
	struct pollfd descriptors[2] = {
		{ .fd = fd, .events = POLLIN }, { .fd = large_fd, .events = POLLIN } };

	memset(focus_counts, 0, sizeof(focus_counts));
	memset(large_counts, 0, sizeof(large_counts));

	while (access(marker, F_OK) < 0) {
		int ready = poll(descriptors, peer_count, 100);

		assert(ready >= 0 || errno == EINTR);
		if (ready <= 0)
			continue;
		for (unsigned int peer = 0; peer < peer_count; peer++) {
			assert(!(descriptors[peer].revents & (POLLHUP | POLLERR | POLLNVAL)));
			if (!(descriptors[peer].revents & POLLIN))
				continue;
			many_event(descriptors[peer].fd, peer == 0 ? windows : large_windows,
				peer == 0 ? count : 256, peer == 0 ? focus_counts : large_counts,
				peer == 0 ? phase : "large");
		}
	}
}

static pid_t many_start_busy(int fd, uint32_t window_id)
{
	pid_t child = fork();

	assert(child >= 0);
	if (child == 0) {
		uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_set_title) + 16] = { 0 };

		assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_SET_TITLE,
			sizeof(gui_protocol_wire_window_set_title) + 16) == 0);
		put32(frame + 8, window_id);
		put16(frame + 12, 16);
		memset(frame + GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_set_title), 'b', 16);
		budget_marker("/tmp/gui-many.busy");
		while (access("/tmp/gui-many.busy-stop", F_OK) < 0) {
			for (unsigned int round = 0; round < 64; round++)
				write_full(fd, frame, sizeof(frame));
			assert(usleep(100000) == 0);
		}
		_exit(0);
	}
	return child;
}

static void test_many_windows(void)
{
	static uint32_t windows[41];
	static uint32_t large_windows[256];
	uint32_t removed_id;
	uint32_t last_id = 0;
	int fd = connect_client();
	int large_fd;
	pid_t busy;
	int status;

	hello(fd);
	for (unsigned int index = 0; index < 14; index++) {
		windows[index] = lifecycle_create(fd, 5000 + index, 320, 192);
		lifecycle_move(fd, windows[index], 48 + index % 4 * 144, 96 + index / 4 * 96);
		begin_surface(fd, windows[index], 320, 192, 1);
		budget_fill(fd, windows[index], 320, 192, 1, 1, lifecycle_color(index));
		last_id = windows[index];
		printf("wm-client: initial created %u id %u\n", index, last_id);
		fflush(stdout);
	}
	removed_id = windows[12];
	puts("wm-client: fourteen 320x192 windows ready beside two real terminals");
	fflush(stdout);
	budget_marker("/tmp/gui-many.initial");
	many_wait(fd, windows, 14, -1, NULL, "initial", "/tmp/gui-many.grow");
	assert(windows[12] == 0);
	lifecycle_invalid_window(fd, removed_id, GUI_PROTOCOL_WINDOW_MOVE);
	for (unsigned int index = 14; index < 41; index++) {
		windows[index] = lifecycle_create(fd, 5000 + index, 96, 64);
		assert(windows[index] > last_id);
		last_id = windows[index];
		lifecycle_move(fd, windows[index], 16 + index % 8 * 112, 48 + index / 8 * 96);
		begin_surface(fd, windows[index], 96, 64, 1);
		budget_fill(fd, windows[index], 96, 64, 1, 1, lifecycle_color(index));
	}
	busy = many_start_busy(fd, windows[0]);
	puts("wm-client: forty live windows, new identities and busy peer ready");
	fflush(stdout);
	budget_marker("/tmp/gui-many.grown");
	many_wait(fd, windows, 41, -1, NULL, "growth", "/tmp/gui-many.large-start");
	budget_marker("/tmp/gui-many.busy-stop");
	assert(waitpid(busy, &status, 0) == busy && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	large_fd = connect_client();
	hello(large_fd);
	for (unsigned int index = 0; index < 256; index++) {
		large_windows[index] = many_large_create(large_fd, 6000 + index);
		assert(large_windows[index] > last_id);
		last_id = large_windows[index];
		lifecycle_move(large_fd, large_windows[index], 900, 80);
	}
	begin_surface(large_fd, large_windows[255], 1, 1, 1);
	send_surface_data(large_fd, large_windows[255], 0, 1, 1, 0x00224466);
	lifecycle_reply(large_fd, large_windows[255], 1, 1, 0, 1);
	commit_surface(large_fd, large_windows[255], 1);
	assert(256 * (GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_list) +
		GUI_PROTOCOL_MAX_TITLE_BYTES) > GUI_PROTOCOL_OUTPUT_LIMIT);
	puts("wm-client: more than 64 KiB of live window-list records ready");
	fflush(stdout);
	budget_marker("/tmp/gui-many.large");
	many_wait(fd, windows, 41, large_fd, large_windows, "recovered", "/tmp/gui-many.drop-large");
	close(large_fd);
	let_server_observe_disconnect();
	begin_surface(fd, windows[0], 320, 192, 2);
	budget_fill(fd, windows[0], 320, 192, 1, 2, lifecycle_color(0));
	puts("wm-client: large disconnect reclaimed without losing the shell");
	fflush(stdout);
	budget_marker("/tmp/gui-many.dropped");
	many_wait(fd, windows, 41, -1, NULL, "post-large", "/tmp/gui-many.finish");
	close(fd);
	let_server_observe_disconnect();
	fd = connect_client();
	hello(fd);
	uint32_t fresh = lifecycle_create(fd, 7000, 64, 48);

	assert(fresh > last_id);
	begin_surface(fd, fresh, 64, 48, 1);
	budget_fill(fd, fresh, 64, 48, 1, 1, 0x00224466);
	destroy_window(fd, fresh);
	close(fd);
	puts("wm-client: paged workloads and resource reclamation pass");
	fflush(stdout);
	budget_marker("/tmp/gui-many.done");
}

static void test_occlusion(void)
{
	int fd = connect_client();
	uint32_t target;
	uint32_t left;
	uint32_t right;

	hello(fd);
	target = lifecycle_create(fd, 8000, 320, 192);
	lifecycle_move(fd, target, 64, 96);
	begin_surface(fd, target, 320, 192, 1);
	budget_fill(fd, target, 320, 192, 1, 1, 0x00225688);
	left = lifecycle_create(fd, 8001, 160, 192);
	lifecycle_move(fd, left, 64, 96);
	begin_surface(fd, left, 160, 192, 1);
	budget_fill(fd, left, 160, 192, 1, 1, 0x00dd8844);
	right = lifecycle_create(fd, 8002, 160, 192);
	lifecycle_move(fd, right, 224, 96);
	begin_surface(fd, right, 160, 192, 1);
	budget_fill(fd, right, 160, 192, 1, 1, 0x008855aa);
	budget_marker("/tmp/gui-occlusion.ready");
	budget_wait("/tmp/gui-occlusion.update");
	begin_surface(fd, target, 320, 192, 2);
	budget_fill(fd, target, 320, 192, 1, 2, 0x00448866);
	begin_surface(fd, target, 320, 192, 3);
	send_surface_data(fd, target, 0, 1, 3, 0x00dd8844);
	lifecycle_reply(fd, target, 1, 3, 0, 1);
	commit_surface(fd, target, 3);
	lifecycle_error(fd, GUI_PROTOCOL_SURFACE_COMMIT);
	budget_marker("/tmp/gui-occlusion.updated");
	budget_wait("/tmp/gui-occlusion.move");
	lifecycle_move(fd, right, 600, 96);
	budget_marker("/tmp/gui-occlusion.moved");
	budget_wait("/tmp/gui-occlusion.close");
	destroy_window(fd, left);
	budget_marker("/tmp/gui-occlusion.closed");
	budget_wait("/tmp/gui-occlusion.text-update");
	for (uint32_t offset = 1; offset < 320 * 192;) {
		uint32_t count = 320 * 192 - offset;

		if (count > SURFACE_PIXELS_PER_MESSAGE)
			count = SURFACE_PIXELS_PER_MESSAGE;
		send_surface_data(fd, target, offset, count, 3, 0x00dd8844);
		lifecycle_reply(fd, target, 1, 3, offset, count);
		offset += count;
	}
	commit_surface(fd, target, 3);
	budget_marker("/tmp/gui-occlusion.text-painted");
	budget_wait("/tmp/gui-occlusion.finish");
	close(fd);
	puts("wm-client: occlusion, partial staging and suspended reveal pass");
	fflush(stdout);
	budget_marker("/tmp/gui-occlusion.done");
}

typedef struct {
	uint32_t target;
	uint32_t sibling;
	uint32_t focused;
	uint32_t width;
	uint32_t height;
	int pending;
	unsigned int resets;
} fullscreen_fixture;

static void fullscreen_reply(int fd, fullscreen_fixture *fixture, uint8_t *frame,
			     gui_protocol_type expected)
{
	for (;;) {
		gui_protocol_header header;

		read_full(fd, frame, GUI_PROTOCOL_HEADER_SIZE);
		assert(gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE, &header) == 0);
		assert(header.length <= 64);
		read_full(fd, frame + GUI_PROTOCOL_HEADER_SIZE, header.length - GUI_PROTOCOL_HEADER_SIZE);
		if (header.type == expected)
			return;
		if (header.type == GUI_PROTOCOL_WINDOW_FOCUS) {
			uint32_t window = get32(frame + 8);

			assert(window == fixture->target || window == fixture->sibling);
			if (get32(frame + 12)) {
				fixture->focused = window;
				budget_marker(window == fixture->target ? "/tmp/gui-fullscreen.focus-target" :
					"/tmp/gui-fullscreen.focus-sibling");
			} else if (fixture->focused == window) {
				fixture->focused = 0;
			}
			continue;
		}
		if (header.type == GUI_PROTOCOL_INPUT_RESET) {
			fixture->resets++;
			continue;
		}
		assert(header.type == GUI_PROTOCOL_INPUT_EVENT && !fixture->pending);
		assert(get32(frame + 8) == fixture->target || get32(frame + 8) == fixture->sibling);
		if (get32(frame + 8) == fixture->target && fixture->width == 1024 &&
		    get32(frame + 12) == GUI_INPUT_EVENT_MOUSE) {
			uint32_t pointer_x = get32(frame + 24);
			uint32_t pointer_y = get32(frame + 28);

			assert(pointer_x < 1024 && pointer_y < 768);
			if (frame[32] & 1) {
				printf("wm-client: fullscreen click %u,%u\n", pointer_x, pointer_y);
				fflush(stdout);
				if (pointer_x == 0 && pointer_y == 0)
					budget_marker("/tmp/gui-fullscreen.edge-top-left");
				if (pointer_x == 1023 && pointer_y == 767)
					budget_marker("/tmp/gui-fullscreen.edge-bottom");
			}
		}
	}
}

static uint32_t fullscreen_create(int fd, fullscreen_fixture *fixture,
				 uint32_t request, uint32_t width, uint32_t height)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create)] = { 0 };
	uint8_t reply[64];

	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_CREATE,
		sizeof(gui_protocol_wire_window_create)) == 0);
	put32(frame + 8, request);
	put32(frame + 12, width);
	put32(frame + 16, height);
	write_full(fd, frame, sizeof(frame));
	fullscreen_reply(fd, fixture, reply, GUI_PROTOCOL_WINDOW_CREATE_REPLY);
	assert(get32(reply + 8) == request && get32(reply + 12) != 0 && get64(reply + 16) == 1);
	assert(get32(reply + 24) == width && get32(reply + 28) == height);
	return get32(reply + 12);
}

static void fullscreen_error(int fd, fullscreen_fixture *fixture, gui_protocol_type type,
			     int status)
{
	uint8_t reply[64];

	fullscreen_reply(fd, fixture, reply, GUI_PROTOCOL_ERROR);
	assert(get16(reply + 12) == type && (int32_t)get32(reply + 8) == status);
}

static void fullscreen_fill(int fd, fullscreen_fixture *fixture, uint32_t window,
			    uint32_t width, uint32_t height, uint64_t serial,
			    uint64_t frame_serial, uint32_t offset, gui_color color)
{
	uint8_t reply[64];

	while (offset < width * height) {
		uint32_t count = width * height - offset;

		if (count > SURFACE_PIXELS_PER_MESSAGE)
			count = SURFACE_PIXELS_PER_MESSAGE;
		send_surface_data_configured(fd, window, offset, count, serial, frame_serial, color);
		fullscreen_reply(fd, fixture, reply, GUI_PROTOCOL_SURFACE_DATA_REPLY);
		assert(get32(reply + 8) == window && get64(reply + 12) == serial &&
			get64(reply + 20) == frame_serial && get32(reply + 28) == offset &&
			get32(reply + 32) == count);
		offset += count;
	}
	commit_surface_configured(fd, window, serial, frame_serial);
	fixture->pending = 0;
	commit_surface_configured(fd, window, serial, frame_serial);
	fullscreen_error(fd, fixture, GUI_PROTOCOL_SURFACE_COMMIT, -EINVAL);
}

typedef struct {
	uint32_t windows[14];
	uint32_t focused;
	unsigned int cycle;
	unsigned int keys;
} capacity_fixture;

static void capacity_marker(unsigned int cycle, const char *phase, int wait)
{
	char path[64];

	assert(snprintf(path, sizeof(path), "/tmp/gui-capacity.%s-%u", phase, cycle) > 0);
	if (wait)
		budget_wait(path);
	else
		budget_marker(path);
}

static void capacity_reply(int fd, capacity_fixture *fixture, uint8_t *frame,
		gui_protocol_type expected)
{
	for (;;) {
		gui_protocol_header header;
		unsigned int index;

		read_full(fd, frame, GUI_PROTOCOL_HEADER_SIZE);
		assert(gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE, &header) == 0);
		assert(header.length <= 64);
		read_full(fd, frame + GUI_PROTOCOL_HEADER_SIZE, header.length - GUI_PROTOCOL_HEADER_SIZE);
		if (header.type == expected)
			return;
		if (header.type == GUI_PROTOCOL_INPUT_RESET)
			continue;
		for (index = 0; index < 14 && fixture->windows[index] != get32(frame + 8); index++)
			;
		assert(index < 14);
		if (header.type == GUI_PROTOCOL_WINDOW_FOCUS) {
			if (get32(frame + 12))
				fixture->focused = fixture->windows[index];
			else if (fixture->focused == fixture->windows[index])
				fixture->focused = 0;
			continue;
		}
		assert(header.type == GUI_PROTOCOL_INPUT_EVENT);
		if (get32(frame + 12) == GUI_INPUT_EVENT_KEY && get32(frame + 16) < GUI_KEYSYM_SPECIAL_BASE) {
			assert(index == 13 && get32(frame + 16) == 'a' && fixture->keys++ == 0);
			fixture->focused = fixture->windows[index];
			capacity_marker(fixture->cycle, "input", 0);
		}
	}
}

static void capacity_healthy(int fd, uint32_t window)
{
	struct pollfd descriptor = { .fd = fd, .events = POLLIN };

	while (poll(&descriptor, 1, 0) == 1) {
		uint8_t frame[64];
		gui_protocol_header header;

		assert(descriptor.revents == POLLIN);
		read_full(fd, frame, GUI_PROTOCOL_HEADER_SIZE);
		assert(gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE, &header) == 0);
		assert(header.length <= sizeof(frame));
		read_full(fd, frame + GUI_PROTOCOL_HEADER_SIZE, header.length - GUI_PROTOCOL_HEADER_SIZE);
		assert(get32(frame + 8) == window);
		assert(header.type == GUI_PROTOCOL_WINDOW_FOCUS || header.type == GUI_PROTOCOL_INPUT_RESET ||
			header.type == GUI_PROTOCOL_INPUT_EVENT);
		if (header.type == GUI_PROTOCOL_INPUT_EVENT && get32(frame + 12) == GUI_INPUT_EVENT_KEY)
			assert(get32(frame + 16) >= GUI_KEYSYM_SPECIAL_BASE);
	}
}

static void capacity_fill(int fd, capacity_fixture *fixture, uint32_t window,
		uint32_t width, uint32_t height, uint64_t serial, gui_color color)
{
	uint32_t total = width * height;
	uint8_t reply[64];

	for (uint32_t offset = 0; offset < total;) {
		uint32_t completed = offset;

		for (unsigned int message = 0; message < 4 && offset < total; message++) {
			uint32_t count = total - offset;

			if (count > SURFACE_PIXELS_PER_MESSAGE)
				count = SURFACE_PIXELS_PER_MESSAGE;
			send_surface_data_configured(fd, window, offset, count, serial, serial, color);
			offset += count;
		}
		while (completed < offset) {
			uint32_t count = offset - completed;

			if (count > SURFACE_PIXELS_PER_MESSAGE)
				count = SURFACE_PIXELS_PER_MESSAGE;
			capacity_reply(fd, fixture, reply, GUI_PROTOCOL_SURFACE_DATA_REPLY);
			assert(get32(reply + 8) == window && get64(reply + 12) == serial &&
				get64(reply + 20) == serial && get32(reply + 28) == completed &&
				get32(reply + 32) == count);
			completed += count;
		}
	}
	commit_surface_configured(fd, window, serial, serial);
}

static void test_capacity_stress(void)
{
	uint32_t previous_identity = 0;

	for (unsigned int cycle = 1; cycle <= 16; cycle++) {
		capacity_fixture fixture = { .cycle = cycle };
		uint8_t reply[64];
		uint32_t healthy_window;
		uint32_t target;
		int fd;
		int healthy;

		capacity_marker(cycle, "start", 1);
		healthy = connect_client();
		hello(healthy);
		healthy_window = lifecycle_create(healthy, 10000 + cycle, 128, 96);
		assert(healthy_window > previous_identity);
		lifecycle_move(healthy, healthy_window, 680, 80);
		begin_surface(healthy, healthy_window, 128, 96, 1);
		budget_fill(healthy, healthy_window, 128, 96, 1, 1, 0x00dd8844);
		fd = connect_client();
		hello(fd);
		for (unsigned int index = 0; index < 14; index++) {
			fixture.windows[index] = lifecycle_create(fd, 11000 + cycle * 14 + index, 320, 192);
			assert(fixture.windows[index] > (index == 0 ? healthy_window : fixture.windows[index - 1]));
			lifecycle_move(fd, fixture.windows[index], 128, 96);
			begin_surface(fd, fixture.windows[index], 320, 192, 1);
			budget_fill(fd, fixture.windows[index], 320, 192, 1, 1,
				index == 13 ? 0x00225688 : 0x00200000 + index);
		}
		target = fixture.windows[13];
		previous_identity = target;
		capacity_marker(cycle, "ready", 0);
		for (uint64_t serial = 2; serial <= 3; serial++) {
			uint32_t width = serial == 2 ? 1024 : 320;
			uint32_t height = serial == 2 ? 768 : 192;

			capacity_reply(fd, &fixture, reply, GUI_PROTOCOL_WINDOW_CONFIGURE);
			assert(fixture.keys == 1 && fixture.focused == target);
			assert(get32(reply + 8) == target && get64(reply + 12) == serial);
			assert(get32(reply + 20) == width && get32(reply + 24) == height);
			begin_surface_configured(fd, target, width, height, serial, serial);
			capacity_fill(fd, &fixture, target, width, height, serial,
				serial == 2 ? 0x00448866 : 0x00225688);
			capacity_marker(cycle, serial == 2 ? "fullscreen" : "restored", 0);
		}
		capacity_reply(fd, &fixture, reply, GUI_PROTOCOL_WINDOW_CLOSE_REQUEST);
		assert(get32(reply + 8) == target && fixture.keys == 1);
		for (unsigned int index = 0; index < 14; index++)
			destroy_window(fd, fixture.windows[index]);
		lifecycle_invalid_window(fd, target, GUI_PROTOCOL_WINDOW_MOVE);
		assert(close(fd) == 0);
		capacity_healthy(healthy, healthy_window);
		begin_surface(healthy, healthy_window, 128, 96, 2);
		budget_fill(healthy, healthy_window, 128, 96, 1, 2, 0x00dd8844);
		capacity_marker(cycle, "healthy", 0);
		capacity_marker(cycle, "finish", 1);
		capacity_healthy(healthy, healthy_window);
		destroy_window(healthy, healthy_window);
		lifecycle_invalid_window(healthy, healthy_window, GUI_PROTOCOL_WINDOW_MOVE);
		assert(close(healthy) == 0);
		printf("wm-client: capacity cycle %u preserves identities, configure, input and healthy peer\n", cycle);
		fflush(stdout);
		capacity_marker(cycle, "done", 0);
	}
	puts("wm-client: sixteen multiwindow capacity cycles pass");
	fflush(stdout);
}

static void test_fullscreen(void)
{
	fullscreen_fixture fixture = { .width = 320, .height = 192 };
	int fd = connect_client();
	uint8_t reply[64];
	uint8_t resize[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_resize)] = { 0 };
	char marker[64];

	hello(fd);
	fixture.target = fullscreen_create(fd, &fixture, 9000, 320, 192);
	lifecycle_move(fd, fixture.target, 144, 112);
	begin_surface(fd, fixture.target, 320, 192, 1);
	fullscreen_fill(fd, &fixture, fixture.target, 320, 192, 1, 1, 0, 0x00225688);
	fixture.sibling = fullscreen_create(fd, &fixture, 9001, 128, 96);
	lifecycle_move(fd, fixture.sibling, 680, 80);
	begin_surface(fd, fixture.sibling, 128, 96, 1);
	fullscreen_fill(fd, &fixture, fixture.sibling, 128, 96, 1, 1, 0, 0x00dd8844);
	assert(fixture.focused == fixture.sibling && fixture.resets >= 3);
	budget_marker("/tmp/gui-fullscreen.ready");
	for (uint64_t serial = 2; serial <= 6; serial++) {
		uint32_t width = serial % 2 == 0 ? 1024 : 320;
		uint32_t height = serial % 2 == 0 ? 768 : 192;
		gui_color color = serial % 2 == 0 ? 0x00448866 : 0x00225688;

		fullscreen_reply(fd, &fixture, reply, GUI_PROTOCOL_WINDOW_CONFIGURE);
		assert(get32(reply + 8) == fixture.target && get64(reply + 12) == serial);
		assert(get32(reply + 20) == width && get32(reply + 24) == height);
		assert(fixture.focused == fixture.target);
		fixture.pending = 1;
		fixture.width = width;
		fixture.height = height;
		if (serial == 2) {
			lifecycle_move(fd, fixture.target, 40, 60);
			fullscreen_error(fd, &fixture, GUI_PROTOCOL_WINDOW_MOVE, -EBUSY);
			assert(gui_protocol_encode_header(resize, sizeof(resize), GUI_PROTOCOL_WINDOW_RESIZE,
				sizeof(gui_protocol_wire_window_resize)) == 0);
			put32(resize + 8, fixture.target);
			put32(resize + 12, 64);
			put32(resize + 16, 48);
			write_full(fd, resize, sizeof(resize));
			fullscreen_error(fd, &fixture, GUI_PROTOCOL_WINDOW_RESIZE, -EBUSY);
		}
		begin_surface_configured(fd, fixture.target, width, height, serial - 1, serial);
		fullscreen_error(fd, &fixture, GUI_PROTOCOL_SURFACE_BEGIN, -EINVAL);
		begin_surface_configured(fd, fixture.target, width, height, serial, serial);
		send_surface_data_configured(fd, fixture.target, 0, 1, serial, serial, color);
		fullscreen_reply(fd, &fixture, reply, GUI_PROTOCOL_SURFACE_DATA_REPLY);
		assert(get32(reply + 8) == fixture.target && get64(reply + 12) == serial &&
			get64(reply + 20) == serial && get32(reply + 28) == 0 && get32(reply + 32) == 1);
		commit_surface_configured(fd, fixture.target, serial, serial);
		fullscreen_error(fd, &fixture, GUI_PROTOCOL_SURFACE_COMMIT, -EINVAL);
		assert(fixture.focused == fixture.target);
		assert(snprintf(marker, sizeof(marker), "/tmp/gui-fullscreen.pending-%llu",
			(unsigned long long)serial) > 0);
		budget_marker(marker);
		assert(snprintf(marker, sizeof(marker), "/tmp/gui-fullscreen.commit-%llu",
			(unsigned long long)serial) > 0);
		budget_wait(marker);
		fullscreen_fill(fd, &fixture, fixture.target, width, height, serial, serial, 1, color);
		assert(fixture.focused == (serial == 5 ? fixture.sibling : fixture.target));
		assert(snprintf(marker, sizeof(marker), "/tmp/gui-fullscreen.frame-%llu",
			(unsigned long long)serial) > 0);
		budget_marker(marker);
	}
	fullscreen_reply(fd, &fixture, reply, GUI_PROTOCOL_WINDOW_CLOSE_REQUEST);
	assert(get32(reply + 8) == fixture.target && fixture.resets >= 10);
	destroy_window(fd, fixture.target);
	begin_surface(fd, fixture.sibling, 128, 96, 2);
	fullscreen_fill(fd, &fixture, fixture.sibling, 128, 96, 1, 2, 0, 0x00dd8844);
	budget_marker("/tmp/gui-fullscreen.sibling-live");
	fullscreen_reply(fd, &fixture, reply, GUI_PROTOCOL_WINDOW_CLOSE_REQUEST);
	assert(get32(reply + 8) == fixture.sibling);
	destroy_window(fd, fixture.sibling);
	close(fd);
	puts("wm-client: fullscreen, exact restore, edges and cooperative sibling close pass");
	fflush(stdout);
	budget_marker("/tmp/gui-fullscreen.done");
}

static void test_quotas(void)
{
	uint8_t frames[sizeof(gui_protocol_wire_header) +
			   sizeof(gui_protocol_wire_hello) +
			   9 * (sizeof(gui_protocol_wire_header) +
				 sizeof(gui_protocol_wire_window_move))];
	uint8_t overflow[4096] = { 0 };
	uint8_t reply[64];
	struct pollfd pollfd;
	struct sigaction ignore_pipe = { .sa_handler = SIG_IGN };
	struct sigaction previous_pipe;
	struct timespec deadline;
	size_t overflow_sent = 0;
	int ready;
	int deferred;
	int offender;
	int healthy;

	/* Nine frames in one read leave two behind the protocol drain quota. */
	deferred = connect_client();
	assert(gui_protocol_encode_hello(frames, sizeof(frames),
		&(gui_protocol_hello) { GUI_PROTOCOL_VERSION_MAJOR,
			GUI_PROTOCOL_VERSION_MINOR }) == 0);
	for (size_t index = 0; index < 9; index++) {
		uint8_t *frame = frames + sizeof(gui_protocol_wire_header) +
			sizeof(gui_protocol_wire_hello) + index *
			(sizeof(gui_protocol_wire_header) +
			 sizeof(gui_protocol_wire_window_move));

		assert(gui_protocol_encode_header(frame,
			sizeof(gui_protocol_wire_header) +
			sizeof(gui_protocol_wire_window_move), GUI_PROTOCOL_WINDOW_MOVE,
			sizeof(gui_protocol_wire_window_move)) == 0);
	}
	write_full(deferred, frames, sizeof(frames));
	read_frame(deferred, reply, GUI_PROTOCOL_HELLO_REPLY);
	for (size_t index = 0; index < 9; index++)
		read_error(deferred, -EINVAL, GUI_PROTOCOL_WINDOW_MOVE);
	close(deferred);

	/* A client with a partial maximum-size frame cannot block a healthy peer. */
	offender = connect_client();
	hello(offender);
	assert(gui_protocol_encode_header(overflow, sizeof(overflow),
		GUI_PROTOCOL_WINDOW_MOVE,
		GUI_PROTOCOL_MAX_MESSAGE_SIZE - GUI_PROTOCOL_HEADER_SIZE) == 0);
	write_full(offender, overflow, sizeof(overflow));
	healthy = connect_client();
	(void)create_window(healthy, 41, 64, 48);
	close(healthy);

	/* One byte fits after 65535 bytes; the next must close the over-limit peer. */
	write_zeroes(offender, GUI_PROTOCOL_MAX_MESSAGE_SIZE - 1 - sizeof(overflow));
	pollfd = (struct pollfd) { .fd = offender, .events = POLLIN };
	assert(poll(&pollfd, 1, 0) == 0);
	assert(sigaction(SIGPIPE, &ignore_pipe, &previous_pipe) == 0);
	while (overflow_sent < 2) {
		ssize_t count = write(offender, overflow + overflow_sent, 2 - overflow_sent);

		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0 && errno == EPIPE)
			break;
		assert(count > 0);
		overflow_sent += count;
	}
	assert(sigaction(SIGPIPE, &previous_pipe, NULL) == 0);
	assert(overflow_sent > 0);
	assert(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0);
	deadline.tv_sec += 5;
	do {
		struct timespec now;
		int64_t remaining_ms;

		assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
		remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000LL +
			(deadline.tv_nsec - now.tv_nsec) / 1000000;
		assert(remaining_ms > 0 && remaining_ms <= 5000);
		ready = poll(&pollfd, 1, (int)remaining_ms);
		assert(ready >= 0 || errno == EINTR);
	} while (ready <= 0);
	assert(pollfd.revents & (POLLIN | POLLHUP | POLLERR));
	assert(read(offender, reply, 1) == 0);
	close(offender);
}

static void test_partial_disconnects(void)
{
	int partial;
	uint32_t window_id;
	int healthy;

	/* Closing at each transfer boundary must release an incomplete staging frame. */
	partial = connect_client();
	window_id = create_window(partial, 50, 64, 48);
	close(partial);
	let_server_observe_disconnect();

	partial = connect_client();
	window_id = create_window(partial, 51, 64, 48);
	begin_surface(partial, window_id, 64, 48, 1);
	close(partial);
	let_server_observe_disconnect();

	partial = connect_client();
	window_id = create_window(partial, 52, 64, 48);
	begin_surface(partial, window_id, 64, 48, 1);
	send_surface_data(partial, window_id, 0, 1, 1, 0x00ff0000);
	close(partial);
	let_server_observe_disconnect();

	/* A new owner can still stage and atomically commit after every partial close. */
	healthy = connect_client();
	window_id = create_window(healthy, 53, 64, 48);
	commit_color_surface(healthy, window_id, 64, 48, 0x00ff0000);
	close(healthy);
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

static void test_launch_cleanup(void)
{
	static const char path[] = "/usr/bin/test/gui-cleanup.exe";
	uint8_t frame[64] = { 0 };
	int fd = connect_client();

	(void)create_window(fd, 1, 64, 48);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_LAUNCH_REQUEST,
		       sizeof(gui_protocol_wire_launch_request) + sizeof(path) - 1) == 0);
	put32(frame + 8, 1);
	put16(frame + 12, GUI_PROTOCOL_LAUNCH_EXECUTABLE);
	put16(frame + 14, sizeof(path) - 1);
	memcpy(frame + 16, path, sizeof(path) - 1);
	write_full(fd, frame, 16 + sizeof(path) - 1);
	read_launch_result(fd, 1, 0);
	assert(close(fd) == 0);
}

static void test_launch(void)
{
	static const char marker[] = "/tmp/mario-wm-launch-marker";
	static const char image[] = "/tmp/mario-wm-launch-marker.bmp";
	static const char directory[] = "/";
	static const gui_protocol_launch_type shell_apps[] = {
		GUI_PROTOCOL_LAUNCH_TERMINAL,
		GUI_PROTOCOL_LAUNCH_FILE_MANAGER,
		GUI_PROTOCOL_LAUNCH_SYSTEM_MONITOR,
	};
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
	read_error(fd, -EPERM, GUI_PROTOCOL_LAUNCH_REQUEST);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_LAUNCH_REQUEST,
		       sizeof(gui_protocol_wire_launch_request) + path_bytes) == 0);
	put32(frame + 8, 2);
	put16(frame + 12, GUI_PROTOCOL_LAUNCH_EXECUTABLE);
	put16(frame + 14, path_bytes);
	memcpy(frame + 16, marker, path_bytes);
	write_full(fd, frame, 16 + path_bytes);
	/* Window-less peers cannot request even the diagnostics launch path. */
	read_error(fd, -EPERM, GUI_PROTOCOL_LAUNCH_REQUEST);
	assert(access(marker, F_OK) < 0 && errno == ENOENT);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_LAUNCH_REQUEST,
		       sizeof(gui_protocol_wire_launch_request) + sizeof(image) - 1) == 0);
	put32(frame + 8, 3);
	put16(frame + 12, GUI_PROTOCOL_LAUNCH_IMAGE_VIEWER);
	put16(frame + 14, sizeof(image) - 1);
	memcpy(frame + 16, image, sizeof(image) - 1);
	write_full(fd, frame, 16 + sizeof(image) - 1);
	read_error(fd, -EPERM, GUI_PROTOCOL_LAUNCH_REQUEST);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_LAUNCH_REQUEST,
		       sizeof(gui_protocol_wire_launch_request) + sizeof(directory) - 1) == 0);
	put32(frame + 8, 4);
	put16(frame + 12, GUI_PROTOCOL_LAUNCH_FILE_MANAGER);
	put16(frame + 14, sizeof(directory) - 1);
	memcpy(frame + 16, directory, sizeof(directory) - 1);
	write_full(fd, frame, 16 + sizeof(directory) - 1);
	read_error(fd, -EPERM, GUI_PROTOCOL_LAUNCH_REQUEST);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_LAUNCH_REQUEST,
		       sizeof(gui_protocol_wire_launch_request) + 1) == 0);
	put32(frame + 8, 5);
	put16(frame + 12, GUI_PROTOCOL_LAUNCH_TERMINAL);
	put16(frame + 14, 1);
	frame[16] = '/';
	write_full(fd, frame, 17);
	read_error(fd, -EPERM, GUI_PROTOCOL_LAUNCH_REQUEST);
	close(fd);

	fd = connect_client();
	(void)create_window(fd, 1, 64, 48);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_LAUNCH_REQUEST,
		       sizeof(gui_protocol_wire_launch_request)) == 0);
	put16(frame + 14, 0);
	for (size_t index = 0; index < sizeof(shell_apps) / sizeof(shell_apps[0]); index++) {
		put32(frame + 8, index + 1);
		put16(frame + 12, shell_apps[index]);
		write_full(fd, frame, 16);
		read_error(fd, -EPERM, GUI_PROTOCOL_LAUNCH_REQUEST);
	}
	close(fd);
}

static void patch_begin_wire(int descriptor, uint32_t window, uint64_t base,
			     uint64_t frame_serial, gui_rect rectangle)
{
	uint8_t bytes[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_patch_begin)] = { 0 };

	assert(gui_protocol_encode_header(bytes, sizeof(bytes), GUI_PROTOCOL_SURFACE_PATCH_BEGIN,
		sizeof(gui_protocol_wire_surface_patch_begin)) == 0);
	put32(bytes + 8, window);
	put64(bytes + 12, 1);
	put64(bytes + 20, base);
	put64(bytes + 28, frame_serial);
	put32(bytes + 36, rectangle.x);
	put32(bytes + 40, rectangle.y);
	put32(bytes + 44, rectangle.w);
	put32(bytes + 48, rectangle.h);
	write_full(descriptor, bytes, sizeof(bytes));
}

static void test_rectangular_upload(void)
{
	uint8_t frame[64];
	gui_rect rectangle = { 4, 3, 8, 5 };
	int descriptor = connect_client();
	uint32_t window;

	hello(descriptor);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_CAPABILITIES, 4) == 0);
	put32(frame + 8, GUI_PROTOCOL_CAP_SURFACE_PATCH);
	write_full(descriptor, frame, 12);
	read_frame(descriptor, frame, GUI_PROTOCOL_CAPABILITIES_REPLY);
	assert(get32(frame + 8) == GUI_PROTOCOL_CAP_SURFACE_PATCH);
	window = lifecycle_create(descriptor, 1, 64, 48);
	lifecycle_move(descriptor, window, 128, 96);
	begin_surface(descriptor, window, 64, 48, 1);
	budget_fill(descriptor, window, 64, 48, 1, 1, 0x00225688);
	patch_begin_wire(descriptor, window, 1, 2, rectangle);
	send_surface_data(descriptor, window, 0, 17, 2, 0x00cc8844);
	lifecycle_reply(descriptor, window, 1, 2, 0, 17);
	commit_surface(descriptor, window, 2);
	read_error(descriptor, -EINVAL, GUI_PROTOCOL_SURFACE_COMMIT);
	budget_marker("/tmp/gui-patch.partial");
	budget_wait("/tmp/gui-patch.complete");
	send_surface_data(descriptor, window, 17, 23, 2, 0x00cc8844);
	lifecycle_reply(descriptor, window, 1, 2, 17, 23);
	commit_surface(descriptor, window, 2);
	patch_begin_wire(descriptor, window, 1, 3, rectangle);
	read_error(descriptor, -ESTALE, GUI_PROTOCOL_SURFACE_PATCH_BEGIN);
	patch_begin_wire(descriptor, window, 2, 3, rectangle);
	send_surface_data(descriptor, window, 0, 7, 3, 0x00448866);
	lifecycle_reply(descriptor, window, 1, 3, 0, 7);
	assert(gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_SURFACE_CANCEL,
		sizeof(gui_protocol_wire_surface_cancel)) == 0);
	put32(frame + 8, window);
	put64(frame + 12, 1);
	put64(frame + 20, 3);
	write_full(descriptor, frame, GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_cancel));
	patch_begin_wire(descriptor, window, 2, 3, rectangle);
	read_error(descriptor, -ESTALE, GUI_PROTOCOL_SURFACE_PATCH_BEGIN);
	budget_marker("/tmp/gui-patch.committed");
	budget_wait("/tmp/gui-patch.finish");
	destroy_window(descriptor, window);
	close(descriptor);
	let_server_observe_disconnect();
	descriptor = connect_client();
	window = create_window(descriptor, 2, 64, 48);
	commit_color_surface(descriptor, window, 64, 48, 0x00225688);
	destroy_window(descriptor, window);
	close(descriptor);
	puts("wm-client: atomic patches, stale bases, cancellation and recovery pass");
}

static void send_window_state_request(int fd, uint32_t window_id, uint32_t state)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE +
		      sizeof(gui_protocol_wire_window_state_request)] = { 0 };

	assert(gui_protocol_encode_header(frame, sizeof(frame),
		GUI_PROTOCOL_WINDOW_STATE_REQUEST,
		sizeof(gui_protocol_wire_window_state_request)) == 0);
	put32(frame + 8, window_id);
	put32(frame + 12, state);
	write_full(fd, frame, sizeof(frame));
}

static void window_state_expect(int fd, uint32_t window_id, uint32_t expected,
				int *minimized_seen)
{
	uint8_t frame[64];
	uint64_t frame_serial = 2;

	for (;;) {
		gui_protocol_header header;

		read_full(fd, frame, GUI_PROTOCOL_HEADER_SIZE);
		assert(gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE, &header) == 0);
		assert(header.length <= sizeof(frame));
		read_full(fd, frame + GUI_PROTOCOL_HEADER_SIZE,
			header.length - GUI_PROTOCOL_HEADER_SIZE);
		if (header.type == GUI_PROTOCOL_WINDOW_STATE) {
			uint32_t state = get32(frame + 12);

			assert(get32(frame + 8) == window_id);
			assert(state <= GUI_PROTOCOL_WINDOW_STATE_MAXIMIZED);
			if (state == GUI_PROTOCOL_WINDOW_STATE_MINIMIZED)
				*minimized_seen = 1;
			else if (state == GUI_PROTOCOL_WINDOW_STATE_NORMAL)
				*minimized_seen = 0;
			if (state == expected)
				return;
			continue;
		}
		if (header.type == GUI_PROTOCOL_WINDOW_CONFIGURE) {
			uint64_t configure_serial = get64(frame + 12);
			uint32_t width = get32(frame + 20);
			uint32_t height = get32(frame + 24);
			uint32_t total = width * height;

			assert(get32(frame + 8) == window_id);
			assert(!*minimized_seen);
			begin_surface_configured(fd, window_id, width, height,
				configure_serial, frame_serial);
			for (uint32_t offset = 0; offset < total;) {
				uint32_t count = total - offset > SURFACE_PIXELS_PER_MESSAGE ?
					SURFACE_PIXELS_PER_MESSAGE : total - offset;

				send_surface_data_configured(fd, window_id, offset, count,
					configure_serial, frame_serial, 0x00ff0000);
				read_surface_data_reply_configured(fd, window_id,
					offset, count, configure_serial, frame_serial);
				offset += count;
			}
			commit_surface_configured(fd, window_id, configure_serial,
				frame_serial);
			frame_serial++;
			continue;
		}
		if (header.type == GUI_PROTOCOL_ERROR) {
			printf("wm-client: window-state error %d type %u\n",
				(int32_t)get32(frame + 8), get16(frame + 12));
			fflush(stdout);
			_exit(2);
		}
		assert(header.type == GUI_PROTOCOL_WINDOW_FOCUS ||
			header.type == GUI_PROTOCOL_INPUT_RESET ||
			header.type == GUI_PROTOCOL_INPUT_EVENT);
	}
}

static void test_window_state(void)
{
	int minimized_seen = 0;
	int fd = connect_client();
	uint32_t window_id = create_window(fd, 900, 200, 150);

	commit_color_surface(fd, window_id, 200, 150, 0x00ff0000);
	puts("wm-client: window-state ready");
	fflush(stdout);
	budget_wait("/tmp/gui-state.initial-checked");

	send_window_state_request(fd, window_id, GUI_PROTOCOL_WINDOW_STATE_MINIMIZED);
	window_state_expect(fd, window_id, GUI_PROTOCOL_WINDOW_STATE_MINIMIZED,
		&minimized_seen);
	budget_marker("/tmp/gui-state.minimized");
	budget_wait("/tmp/gui-state.minimized-checked");

	send_window_state_request(fd, window_id, GUI_PROTOCOL_WINDOW_STATE_NORMAL);
	window_state_expect(fd, window_id, GUI_PROTOCOL_WINDOW_STATE_NORMAL,
		&minimized_seen);
	budget_marker("/tmp/gui-state.restored");
	budget_wait("/tmp/gui-state.restored-checked");

	send_window_state_request(fd, window_id, GUI_PROTOCOL_WINDOW_STATE_MAXIMIZED);
	window_state_expect(fd, window_id, GUI_PROTOCOL_WINDOW_STATE_MAXIMIZED,
		&minimized_seen);
	budget_marker("/tmp/gui-state.maximized");
	budget_wait("/tmp/gui-state.maximized-checked");

	send_window_state_request(fd, window_id, GUI_PROTOCOL_WINDOW_STATE_NORMAL);
	window_state_expect(fd, window_id, GUI_PROTOCOL_WINDOW_STATE_NORMAL,
		&minimized_seen);
	budget_marker("/tmp/gui-state.unmaximized");
	budget_wait("/tmp/gui-state.done");

	destroy_window(fd, window_id);
	close(fd);
	puts("wm-client: window-state requests pass");
}

static void test_chrome(void)
{
	int minimized_seen = 0;
	int fd = connect_client();
	uint32_t window_id = create_window(fd, 901, 200, 150);

	commit_color_surface(fd, window_id, 200, 150, 0x00ff0000);
	puts("wm-client: chrome ready");
	fflush(stdout);
	budget_wait("/tmp/gui-chrome.initial-checked");

	window_state_expect(fd, window_id, GUI_PROTOCOL_WINDOW_STATE_MINIMIZED,
		&minimized_seen);
	budget_marker("/tmp/gui-chrome.minimized");
	budget_wait("/tmp/gui-chrome.minimized-checked");

	send_window_state_request(fd, window_id, GUI_PROTOCOL_WINDOW_STATE_NORMAL);
	window_state_expect(fd, window_id, GUI_PROTOCOL_WINDOW_STATE_NORMAL,
		&minimized_seen);
	budget_marker("/tmp/gui-chrome.restored");
	budget_wait("/tmp/gui-chrome.restored-checked");

	window_state_expect(fd, window_id, GUI_PROTOCOL_WINDOW_STATE_MAXIMIZED,
		&minimized_seen);
	budget_marker("/tmp/gui-chrome.maximized");
	budget_wait("/tmp/gui-chrome.maximized-checked");

	window_state_expect(fd, window_id, GUI_PROTOCOL_WINDOW_STATE_NORMAL,
		&minimized_seen);
	budget_marker("/tmp/gui-chrome.unmaximized");

	read_close_request(fd, window_id);
	puts("wm-client: chrome requests pass");
}

static void expect_no_configure(int fd, int timeout_ms)
{
	struct pollfd descriptor = { .fd = fd, .events = POLLIN };
	gui_protocol_header header;
	uint8_t frame[64];
	int ready;

	do {
		ready = poll(&descriptor, 1, timeout_ms);
		if (ready <= 0)
			break;
		assert(descriptor.revents == POLLIN);
		read_full(fd, frame, GUI_PROTOCOL_HEADER_SIZE);
		assert(gui_protocol_decode_header(frame, GUI_PROTOCOL_HEADER_SIZE,
			&header) == 0);
		assert(header.length <= sizeof(frame));
		read_full(fd, frame + GUI_PROTOCOL_HEADER_SIZE,
			header.length - GUI_PROTOCOL_HEADER_SIZE);
		if (header.type == GUI_PROTOCOL_INPUT_EVENT ||
		    header.type == GUI_PROTOCOL_INPUT_RESET)
			continue;
		printf("wm-client: no-configure saw type %u\n", header.type);
		fflush(stdout);
		_exit(2);
	} while (ready > 0);
	assert(ready == 0);
}

static void test_resize(void)
{
	int fd = connect_client();
	uint32_t window_id = create_window(fd, 902, 160, 100);

	lifecycle_move(fd, window_id, 64, 100);
	commit_color_surface(fd, window_id, 160, 100, 0x00ff0000);
	puts("wm-client: resize ready");
	fflush(stdout);
	budget_wait("/tmp/gui-resize.initial-checked");

	read_configure(fd, window_id, 2, 180, 100);
	puts("wm-client: left resize ready");
	fflush(stdout);
	budget_marker("/tmp/gui-resize.left");
	read_configure(fd, window_id, 3, 200, 100);
	puts("wm-client: right resize ready");
	fflush(stdout);
	budget_marker("/tmp/gui-resize.right");
	read_configure(fd, window_id, 4, 200, 110);
	puts("wm-client: top resize ready");
	fflush(stdout);
	budget_marker("/tmp/gui-resize.top");
	read_configure(fd, window_id, 5, 200, 120);
	puts("wm-client: bottom resize ready");
	fflush(stdout);
	budget_marker("/tmp/gui-resize.bottom");
	read_configure(fd, window_id, 6, 210, 130);
	puts("wm-client: top-left resize ready");
	fflush(stdout);
	budget_marker("/tmp/gui-resize.top-left");
	read_configure(fd, window_id, 7, 220, 140);
	puts("wm-client: top-right resize ready");
	fflush(stdout);
	budget_marker("/tmp/gui-resize.top-right");
	read_configure(fd, window_id, 8, 230, 150);
	puts("wm-client: bottom-left resize ready");
	fflush(stdout);
	budget_marker("/tmp/gui-resize.bottom-left");
	read_configure(fd, window_id, 9, 240, 160);
	puts("wm-client: bottom-right resize ready");
	fflush(stdout);
	budget_marker("/tmp/gui-resize.bottom-right");
	read_configure(fd, window_id, 10, 113, 48);
	puts("wm-client: minimum drag intermediate ready");
	fflush(stdout);
	read_configure(fd, window_id, 11, 64, 48);
	puts("wm-client: minimum resize ready");
	fflush(stdout);
	budget_marker("/tmp/gui-resize.minimum");

	budget_wait("/tmp/gui-resize.min-checked");
	budget_wait("/tmp/gui-resize.click-done");
	expect_no_configure(fd, 1000);
	puts("wm-client: resize no-configure pass");
}

int main(int argc, char **argv)
{
	uint8_t frame[64];
	int first;
	int second;
	uint32_t first_id;

	assert(argc == 1 || (argc == 2 && (!strcmp(argv[1], "--surface-only") ||
		!strcmp(argv[1], "--quota-only") || !strcmp(argv[1], "--surface-budget") ||
		!strcmp(argv[1], "--window-lifecycle") ||
		!strcmp(argv[1], "--many-windows") ||
		!strcmp(argv[1], "--occlusion") ||
		!strcmp(argv[1], "--fullscreen") ||
		!strcmp(argv[1], "--capacity-stress") ||
		!strcmp(argv[1], "--patch-only") ||
		!strcmp(argv[1], "--partial-only") ||
		!strcmp(argv[1], "--commit-only") ||
		!strcmp(argv[1], "--recovery-only") ||
		!strcmp(argv[1], "--public-shell-only") || !strcmp(argv[1], "--stack-only") ||
		!strcmp(argv[1], "--taskbar-only") || !strcmp(argv[1], "--shortcuts-only") ||
		!strcmp(argv[1], "--interact-only") || !strcmp(argv[1], "--launch-only") ||
		!strcmp(argv[1], "--launch-cleanup-only") ||
		!strcmp(argv[1], "--window-state-only") ||
		!strcmp(argv[1], "--chrome-only") ||
		!strcmp(argv[1], "--resize-only"))));
	if (argc == 2) {
		if (!strcmp(argv[1], "--patch-only")) {
			test_rectangular_upload();
		} else if (!strcmp(argv[1], "--surface-only")) {
			test_staging();
			puts("wm-client: staging ok");
		} else if (!strcmp(argv[1], "--surface-budget")) {
			test_surface_budget();
		} else if (!strcmp(argv[1], "--window-lifecycle")) {
			test_window_lifecycle();
		} else if (!strcmp(argv[1], "--many-windows")) {
			test_many_windows();
		} else if (!strcmp(argv[1], "--occlusion")) {
			test_occlusion();
		} else if (!strcmp(argv[1], "--capacity-stress")) {
			test_capacity_stress();
		} else if (!strcmp(argv[1], "--fullscreen")) {
			test_fullscreen();
		} else if (!strcmp(argv[1], "--quota-only")) {
			test_quotas();
			puts("wm-client: quotas ok");
		} else if (!strcmp(argv[1], "--partial-only")) {
			test_partial_disconnects();
			puts("wm-client: partial recovery ok");
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
		} else if (!strcmp(argv[1], "--launch-cleanup-only")) {
			test_launch_cleanup();
		} else if (!strcmp(argv[1], "--window-state-only")) {
			test_window_state();
		} else if (!strcmp(argv[1], "--chrome-only")) {
			test_chrome();
		} else if (!strcmp(argv[1], "--resize-only")) {
			test_resize();
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
