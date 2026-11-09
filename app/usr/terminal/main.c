#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gui/protocol.h>

#include "vt.h"

#define WM_SOCKET_PATH "/tmp/mario-wm.sock"
#define TERMINAL_WIDTH 320U
#define TERMINAL_HEIGHT 192U
#define TERMINAL_TITLE "Terminal"
#define SURFACE_PIXELS_PER_MESSAGE 1000U
#define TERMINAL_BACKGROUND 0x00111827U
#define TERMINAL_GRID 0x002563ebU
#define TERMINAL_CURSOR 0x00f8fafcU
#define TERMINAL_ROWS 24U
#define TERMINAL_COLUMNS 80U

typedef struct {
	int fd;
	uint32_t window_id;
	uint64_t configure_serial;
	uint64_t frame_serial;
	uint32_t width;
	uint32_t height;
	int close_requested;
	terminal_vt vt;
} terminal_state;

/* Keep the largest outbound wire frame out of Mario's small initial stack. */
static uint8_t terminal_frame[GUI_PROTOCOL_HEADER_SIZE +
	sizeof(gui_protocol_wire_surface_data) +
	SURFACE_PIXELS_PER_MESSAGE * sizeof(uint32_t)];
static terminal_vt_cell terminal_primary_cells[TERMINAL_ROWS * TERMINAL_COLUMNS];
static terminal_vt_cell terminal_alternate_cells[TERMINAL_ROWS * TERMINAL_COLUMNS];

static void put16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
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

static uint16_t get16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static uint64_t get64(const uint8_t *bytes)
{
	uint64_t value = 0;

	for (unsigned int index = 0; index < 8; index++)
		value |= (uint64_t)bytes[index] << (index * 8);
	return value;
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

static int read_full(int fd, void *buffer, size_t byte_count)
{
	uint8_t *bytes = buffer;

	while (byte_count != 0) {
		ssize_t count = read(fd, bytes, byte_count);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return -1;
		bytes += count;
		byte_count -= count;
	}
	return 0;
}

static int send_message(int fd, gui_protocol_type type, const void *payload,
			size_t payload_bytes)
{
	uint8_t header[GUI_PROTOCOL_HEADER_SIZE];

	return gui_protocol_encode_header(header, sizeof(header), type, payload_bytes) < 0 ||
		write_full(fd, header, sizeof(header)) < 0 ||
		(payload_bytes != 0 && write_full(fd, payload, payload_bytes) < 0) ? -1 : 0;
}

static int read_message(int fd, gui_protocol_header *header, uint8_t *payload,
			size_t payload_capacity)
{
	uint8_t wire_header[GUI_PROTOCOL_HEADER_SIZE];

	if (read_full(fd, wire_header, sizeof(wire_header)) < 0 ||
	    gui_protocol_decode_header(wire_header, sizeof(wire_header), header) < 0 ||
	    header->length < GUI_PROTOCOL_HEADER_SIZE ||
	    header->length - GUI_PROTOCOL_HEADER_SIZE > payload_capacity)
		return -1;
	return read_full(fd, payload, header->length - GUI_PROTOCOL_HEADER_SIZE);
}

static uint32_t terminal_pixel(const terminal_state *state, uint32_t offset)
{
	uint32_t x = offset % state->width;
	uint32_t y = offset / state->width;

	if (x == 24 && y >= 24 && y < 40)
		return TERMINAL_CURSOR;
	if (x >= 16 && y >= 16 && x < state->width - 16 && y < state->height - 16 &&
	    ((x - 16) % 16 == 0 || (y - 16) % 16 == 0))
		return TERMINAL_GRID;
	return TERMINAL_BACKGROUND;
}

static int terminal_handle_message(terminal_state *state,
				   const gui_protocol_header *header,
				   const uint8_t *payload, size_t payload_bytes)
{
	if (header->type == GUI_PROTOCOL_WINDOW_CONFIGURE) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_configure) ||
		    get32(payload) != state->window_id || get64(payload + 4) == 0 ||
		    get32(payload + 12) == 0 || get32(payload + 16) == 0)
			return -1;
		state->configure_serial = get64(payload + 4);
		state->width = get32(payload + 12);
		state->height = get32(payload + 16);
		return 1;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_CLOSE_REQUEST) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_id) ||
		    get32(payload) != state->window_id)
			return -1;
		state->close_requested = 1;
		return 0;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_FOCUS)
		return payload_bytes == sizeof(gui_protocol_wire_window_focus) &&
			get32(payload) == state->window_id ? 0 : -1;
	if (header->type == GUI_PROTOCOL_INPUT_RESET)
		return payload_bytes == 0 ? 0 : -1;
	if (header->type == GUI_PROTOCOL_INPUT_EVENT)
		return payload_bytes == sizeof(gui_protocol_wire_input_event) &&
			get32(payload) == state->window_id ? 0 : -1;
	return -1;
}

static int read_surface_data_reply(terminal_state *state, uint64_t frame_serial,
				   uint32_t pixel_offset, uint32_t pixel_count)
{
	uint8_t payload[sizeof(gui_protocol_wire_surface_data_reply)];

	for (;;) {
		gui_protocol_header header;
		int result;

		if (read_message(state->fd, &header, payload, sizeof(payload)) < 0)
		{
			fprintf(stderr, "terminal: surface reply read failed\n");
			return -1;
		}
		if (header.type == GUI_PROTOCOL_SURFACE_DATA_REPLY) {
			if (header.length != GUI_PROTOCOL_HEADER_SIZE +
			    sizeof(gui_protocol_wire_surface_data_reply) ||
			    get32(payload) != state->window_id ||
			    get64(payload + 4) != state->configure_serial ||
			    get64(payload + 12) != frame_serial ||
			    get32(payload + 20) != pixel_offset ||
			    get32(payload + 24) != pixel_count)
			{
				fprintf(stderr, "terminal: malformed surface reply\n");
				return -1;
			}
			return 0;
		}
		if (header.type == GUI_PROTOCOL_ERROR &&
		    header.length == GUI_PROTOCOL_HEADER_SIZE +
		    sizeof(gui_protocol_wire_error)) {
			fprintf(stderr, "terminal: protocol error type %u status %d\n",
				get16(payload + 4), (int32_t)get32(payload));
			return -1;
		}
		result = terminal_handle_message(state, &header, payload,
			header.length - GUI_PROTOCOL_HEADER_SIZE);
		if (result < 0)
			fprintf(stderr, "terminal: unexpected message %u\n", header.type);
		if (result < 0 || state->close_requested)
			return -1;
		if (result > 0)
			return 1;
	}
}

static int terminal_send_surface(terminal_state *state)
{
	uint8_t begin[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_begin)] = { 0 };
	uint8_t commit[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_commit)];
	uint64_t frame_serial = ++state->frame_serial;
	uint32_t total;

	if (frame_serial == 0 || state->height > UINT32_MAX / state->width)
		return -1;
	total = state->width * state->height;
	if (gui_protocol_encode_header(begin, sizeof(begin), GUI_PROTOCOL_SURFACE_BEGIN,
				       sizeof(gui_protocol_wire_surface_begin)) < 0)
		return -1;
	put32(begin + GUI_PROTOCOL_HEADER_SIZE, state->window_id);
	put64(begin + GUI_PROTOCOL_HEADER_SIZE + 4, state->configure_serial);
	put64(begin + GUI_PROTOCOL_HEADER_SIZE + 12, frame_serial);
	put32(begin + GUI_PROTOCOL_HEADER_SIZE + 28, state->width);
	put32(begin + GUI_PROTOCOL_HEADER_SIZE + 32, state->height);
	if (write_full(state->fd, begin, sizeof(begin)) < 0)
		return -1;
	for (uint32_t pixel_offset = 0; pixel_offset < total;) {
		uint32_t pixel_count = total - pixel_offset > SURFACE_PIXELS_PER_MESSAGE ?
			SURFACE_PIXELS_PER_MESSAGE : total - pixel_offset;
		size_t payload_bytes = sizeof(gui_protocol_wire_surface_data) +
			pixel_count * sizeof(uint32_t);
		int result;

		if (gui_protocol_encode_header(terminal_frame, sizeof(terminal_frame),
				GUI_PROTOCOL_SURFACE_DATA, payload_bytes) < 0)
			return -1;
		put32(terminal_frame + GUI_PROTOCOL_HEADER_SIZE, state->window_id);
		put64(terminal_frame + GUI_PROTOCOL_HEADER_SIZE + 4,
			state->configure_serial);
		put64(terminal_frame + GUI_PROTOCOL_HEADER_SIZE + 12, frame_serial);
		put32(terminal_frame + GUI_PROTOCOL_HEADER_SIZE + 20, pixel_offset);
		put32(terminal_frame + GUI_PROTOCOL_HEADER_SIZE + 24, pixel_count);
		for (uint32_t index = 0; index < pixel_count; index++)
			put32(terminal_frame + GUI_PROTOCOL_HEADER_SIZE +
				sizeof(gui_protocol_wire_surface_data) + index * sizeof(uint32_t),
				terminal_pixel(state, pixel_offset + index));
		if (write_full(state->fd, terminal_frame,
			GUI_PROTOCOL_HEADER_SIZE + payload_bytes) < 0)
			return -1;
		result = read_surface_data_reply(state, frame_serial, pixel_offset, pixel_count);
		if (result != 0)
			return result;
		pixel_offset += pixel_count;
	}
	if (gui_protocol_encode_header(commit, sizeof(commit), GUI_PROTOCOL_SURFACE_COMMIT,
				       sizeof(gui_protocol_wire_surface_commit)) < 0)
		return -1;
	put32(commit + GUI_PROTOCOL_HEADER_SIZE, state->window_id);
	put64(commit + GUI_PROTOCOL_HEADER_SIZE + 4, state->configure_serial);
	put64(commit + GUI_PROTOCOL_HEADER_SIZE + 12, frame_serial);
	return write_full(state->fd, commit, sizeof(commit));
}

static int terminal_connect(void)
{
	struct sockaddr_un address = { 0 };
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, WM_SOCKET_PATH, sizeof(WM_SOCKET_PATH));
	if (connect(fd, (const struct sockaddr *)&address,
		sizeof(address.sun_family) + sizeof(WM_SOCKET_PATH)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int terminal_create_window(terminal_state *state)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create) +
		sizeof(TERMINAL_TITLE) - 1] = { 0 };
	uint8_t payload[sizeof(gui_protocol_wire_window_create_reply)];
	gui_protocol_header header;
	size_t title_bytes = sizeof(TERMINAL_TITLE) - 1;

	if (gui_protocol_encode_hello(frame, sizeof(frame),
		&(gui_protocol_hello) { GUI_PROTOCOL_VERSION_MAJOR,
			GUI_PROTOCOL_VERSION_MINOR }) < 0 ||
	    write_full(state->fd, frame, GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_hello)) < 0 ||
	    read_message(state->fd, &header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_HELLO_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_hello))
		return -1;
	if (gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_CREATE,
			sizeof(gui_protocol_wire_window_create) + title_bytes) < 0)
		return -1;
	put32(frame + GUI_PROTOCOL_HEADER_SIZE, 1);
	put32(frame + GUI_PROTOCOL_HEADER_SIZE + 4, TERMINAL_WIDTH);
	put32(frame + GUI_PROTOCOL_HEADER_SIZE + 8, TERMINAL_HEIGHT);
	put16(frame + GUI_PROTOCOL_HEADER_SIZE + 12, title_bytes);
	memcpy(frame + GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create),
		TERMINAL_TITLE, title_bytes);
	if (write_full(state->fd, frame, GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_window_create) + title_bytes) < 0 ||
	    read_message(state->fd, &header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_WINDOW_CREATE_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_window_create_reply) || get32(payload) != 1)
		return -1;
	state->window_id = get32(payload + 4);
	state->configure_serial = get64(payload + 8);
	state->width = get32(payload + 16);
	state->height = get32(payload + 20);
	return state->window_id != 0 && state->configure_serial != 0 &&
		state->width != 0 && state->height != 0 ? 0 : -1;
}

static int terminal_spawn_pty(const char *command, int *master, pid_t *child)
{
	pid_t pid = forkpty(master, NULL, NULL, NULL);
	int flags;

	if (pid < 0)
		return -1;
	if (pid == 0) {
		if (command != NULL)
			execl("/bin/oksh", "oksh", "-c", command, NULL);
		else
			execl("/bin/oksh", "oksh", NULL);
		_exit(127);
	}
	flags = fcntl(*master, F_GETFL);
	if (flags < 0 || fcntl(*master, F_SETFL, flags | O_NONBLOCK) < 0) {
		close(*master);
		return -1;
	}
	*child = pid;
	return 0;
}

static int terminal_finish_child(int master, pid_t child)
{
	int status;

	if (close(master) < 0 || waitpid(child, &status, 0) != child)
		return -1;
	if (!WIFEXITED(status)) {
		fprintf(stderr, "terminal: child did not exit normally\n");
		return -1;
	}
	printf("terminal: child exit %d\n", WEXITSTATUS(status));
	fflush(stdout);
	return WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int terminal_drain_pty(int master, terminal_vt *vt)
{
	uint8_t buffer[512];

	for (;;) {
		ssize_t count = read(master, buffer, sizeof(buffer));

		if (count > 0) {
			terminal_vt_feed(vt, buffer, count);
			if (write_full(STDOUT_FILENO, buffer, count) < 0)
				return -1;
			continue;
		}
		if (count == 0 || (count < 0 && errno == EIO))
			return 1;
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		if (count < 0 && errno == EINTR)
			continue;
		return -1;
	}
}

int main(int argc, char **argv)
{
	terminal_state state = { .fd = -1 };
	uint8_t payload[sizeof(gui_protocol_wire_window_configure)];
	const char *command = NULL;
	pid_t child = -1;
	int master = -1;
	int status = 1;

	if (argc == 3 && !strcmp(argv[1], "--command"))
		command = argv[2];
	else if (argc != 1)
		return 1;
	state.fd = terminal_connect();
	if (state.fd < 0) {
		fprintf(stderr, "terminal: connect failed: %d\n", errno);
		goto out;
	}
	if (terminal_create_window(&state) < 0) {
		fprintf(stderr, "terminal: create failed\n");
		goto out;
	}
	if (terminal_send_surface(&state) != 0) {
		fprintf(stderr, "terminal: initial frame failed\n");
		goto out;
	}
	if (terminal_vt_init(&state.vt, terminal_primary_cells,
			     terminal_alternate_cells, TERMINAL_ROWS, TERMINAL_COLUMNS) < 0) {
		fprintf(stderr, "terminal: VT setup failed\n");
		goto out;
	}
	if (terminal_spawn_pty(command, &master, &child) < 0) {
		fprintf(stderr, "terminal: PTY setup failed: %d\n", errno);
		goto out;
	}
	puts("terminal: window ready");
	fflush(stdout);
	status = 0;
	for (;;) {
		gui_protocol_header header;
		struct pollfd pollfds[2];
		int result;

		pollfds[0] = (struct pollfd) { .fd = state.fd, .events = POLLIN };
		pollfds[1] = (struct pollfd) { .fd = master, .events = POLLIN };
		if (poll(pollfds, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pollfds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
			break;
		if (pollfds[0].revents & POLLIN) {
			if (read_message(state.fd, &header, payload, sizeof(payload)) < 0 ||
			    (result = terminal_handle_message(&state, &header, payload,
		     header.length - GUI_PROTOCOL_HEADER_SIZE)) < 0)
				break;
			if (result > 0 && terminal_send_surface(&state) != 0)
				break;
			if (state.close_requested)
				break;
		}
		if (pollfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			result = terminal_drain_pty(master, &state.vt);
			if (result < 0)
				break;
			if (result > 0) {
				status = terminal_finish_child(master, child) == 0 ? 0 : 1;
				master = -1;
				state.close_requested = 1;
				break;
			}
		}
	}
	if (state.close_requested) {
		uint8_t window_id[sizeof(gui_protocol_wire_window_id)];

		put32(window_id, state.window_id);
		(void)send_message(state.fd, GUI_PROTOCOL_WINDOW_DESTROY, window_id,
			sizeof(window_id));
	}
out:
	if (master >= 0)
		close(master);
	if (state.fd >= 0)
		close(state.fd);
	return status;
}
