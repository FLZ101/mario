#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <gui/image.h>
#include <gui/protocol.h>
#include <gui/surface.h>

#define WM_SOCKET_PATH "/tmp/mario-wm.sock"
#define IMAGEVIEWER_WIDTH 320U
#define IMAGEVIEWER_HEIGHT 256U
#define IMAGEVIEWER_TITLE "Image Viewer"
#define IMAGEVIEWER_BACKGROUND 0x00111827U
#define SURFACE_PIXELS_PER_MESSAGE 1000U
#define PNG_SIGNATURE_BYTES 8U

typedef struct {
	int fd;
	uint32_t window_id;
	uint64_t configure_serial;
	uint64_t frame_serial;
	uint32_t width;
	uint32_t height;
	int close_requested;
	gui_surface *image;
	gui_surface *surface;
} imageviewer_state;

/* Keep the largest outbound frame out of Mario's intentionally small initial stack. */
static uint8_t imageviewer_frame[GUI_PROTOCOL_HEADER_SIZE +
	sizeof(gui_protocol_wire_surface_data) +
	SURFACE_PIXELS_PER_MESSAGE * sizeof(uint32_t)];
static uint8_t imageviewer_input[GUI_PROTOCOL_MAX_MESSAGE_SIZE - GUI_PROTOCOL_HEADER_SIZE];

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

static int imageviewer_resize_surface(imageviewer_state *state, uint32_t width,
				      uint32_t height)
{
	gui_surface *surface;

	if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX)
		return -1;
	surface = gui_surface_create((int)width, (int)height);
	if (surface == NULL)
		return -1;
	gui_surface_destroy(state->surface);
	state->surface = surface;
	state->width = width;
	state->height = height;
	return 0;
}

static void imageviewer_render(imageviewer_state *state)
{
	uint32_t draw_width;
	uint32_t draw_height;
	uint32_t offset_x;
	uint32_t offset_y;

	gui_fill_rect(state->surface, (gui_rect) { .w = (int)state->width,
		.h = (int)state->height }, IMAGEVIEWER_BACKGROUND);
	/* Choose the limiting axis with 64-bit products before scaling the source. */
	if ((uint64_t)state->width * state->image->height <=
	    (uint64_t)state->height * state->image->width) {
		draw_width = state->width;
		draw_height = (uint64_t)state->image->height * draw_width /
			state->image->width;
	} else {
		draw_height = state->height;
		draw_width = (uint64_t)state->image->width * draw_height /
			state->image->height;
	}
	if (draw_width == 0 || draw_height == 0)
		return;
	offset_x = (state->width - draw_width) / 2;
	offset_y = (state->height - draw_height) / 2;
	for (uint32_t y = 0; y < draw_height; y++) {
		uint32_t source_y = (uint64_t)y * state->image->height / draw_height;
		gui_color *destination = state->surface->pixels +
			(size_t)(offset_y + y) * state->surface->stride + offset_x;
		const gui_color *source = state->image->pixels +
			(size_t)source_y * state->image->stride;

		for (uint32_t x = 0; x < draw_width; x++)
			destination[x] = source[(uint64_t)x * state->image->width / draw_width];
	}
}

static int imageviewer_handle_message(imageviewer_state *state,
				      const gui_protocol_header *header,
				      const uint8_t *payload, size_t payload_bytes)
{
	if (header->type == GUI_PROTOCOL_WINDOW_CONFIGURE) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_configure) ||
		    get32(payload) != state->window_id || get64(payload + 4) == 0 ||
		    get32(payload + 12) == 0 || get32(payload + 16) == 0 ||
		    imageviewer_resize_surface(state, get32(payload + 12), get32(payload + 16)) < 0)
			return -1;
		state->configure_serial = get64(payload + 4);
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

static int imageviewer_read_surface_reply(imageviewer_state *state,
					  uint64_t frame_serial, uint32_t pixel_offset,
					  uint32_t pixel_count)
{
	for (;;) {
		gui_protocol_header header;
		int result;

		if (read_message(state->fd, &header, imageviewer_input,
			 sizeof(imageviewer_input)) < 0)
			return -1;
		if (header.type == GUI_PROTOCOL_SURFACE_DATA_REPLY) {
			if (header.length != GUI_PROTOCOL_HEADER_SIZE +
			    sizeof(gui_protocol_wire_surface_data_reply) ||
			    get32(imageviewer_input) != state->window_id ||
			    get64(imageviewer_input + 4) != state->configure_serial ||
			    get64(imageviewer_input + 12) != frame_serial ||
			    get32(imageviewer_input + 20) != pixel_offset ||
			    get32(imageviewer_input + 24) != pixel_count)
				return -1;
			return 0;
		}
		if (header.type == GUI_PROTOCOL_ERROR &&
		    header.length == GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_error))
			return -1;
		result = imageviewer_handle_message(state, &header, imageviewer_input,
			header.length - GUI_PROTOCOL_HEADER_SIZE);
		if (result < 0 || state->close_requested)
			return -1;
		if (result > 0)
			return 1;
	}
}

static int imageviewer_send_surface(imageviewer_state *state)
{
	uint8_t begin[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_begin)] = { 0 };
	uint8_t commit[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_commit)];
	uint64_t frame_serial = ++state->frame_serial;
	uint32_t total;

	if (frame_serial == 0 || state->height > UINT32_MAX / state->width)
		return -1;
	imageviewer_render(state);
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

		if (gui_protocol_encode_header(imageviewer_frame, sizeof(imageviewer_frame),
				GUI_PROTOCOL_SURFACE_DATA, payload_bytes) < 0)
			return -1;
		put32(imageviewer_frame + GUI_PROTOCOL_HEADER_SIZE, state->window_id);
		put64(imageviewer_frame + GUI_PROTOCOL_HEADER_SIZE + 4,
			state->configure_serial);
		put64(imageviewer_frame + GUI_PROTOCOL_HEADER_SIZE + 12, frame_serial);
		put32(imageviewer_frame + GUI_PROTOCOL_HEADER_SIZE + 20, pixel_offset);
		put32(imageviewer_frame + GUI_PROTOCOL_HEADER_SIZE + 24, pixel_count);
		for (uint32_t index = 0; index < pixel_count; index++)
			put32(imageviewer_frame + GUI_PROTOCOL_HEADER_SIZE +
				sizeof(gui_protocol_wire_surface_data) + index * sizeof(uint32_t),
				state->surface->pixels[pixel_offset + index]);
		if (write_full(state->fd, imageviewer_frame,
			GUI_PROTOCOL_HEADER_SIZE + payload_bytes) < 0)
			return -1;
		result = imageviewer_read_surface_reply(state, frame_serial, pixel_offset,
			pixel_count);
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

static int imageviewer_send_current_surface(imageviewer_state *state)
{
	int result;

	/* A resize can arrive while the server acknowledges one surface chunk. */
	do {
		result = imageviewer_send_surface(state);
		if (result < 0)
			return -1;
	} while (result > 0);
	return 0;
}

static int imageviewer_connect(void)
{
	struct sockaddr_un address = { 0 };
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

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

static int imageviewer_create_window(imageviewer_state *state)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create) +
		sizeof(IMAGEVIEWER_TITLE) - 1] = { 0 };
	uint8_t payload[sizeof(gui_protocol_wire_window_create_reply)];
	gui_protocol_header header;
	size_t title_bytes = sizeof(IMAGEVIEWER_TITLE) - 1;

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
	put32(frame + GUI_PROTOCOL_HEADER_SIZE + 4, IMAGEVIEWER_WIDTH);
	put32(frame + GUI_PROTOCOL_HEADER_SIZE + 8, IMAGEVIEWER_HEIGHT);
	put16(frame + GUI_PROTOCOL_HEADER_SIZE + 12, title_bytes);
	memcpy(frame + GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create),
		IMAGEVIEWER_TITLE, title_bytes);
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

static int imageviewer_read_file(const char *path, uint8_t **contents,
				 size_t *byte_count)
{
	struct stat status;
	uint8_t *bytes;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	if (fstat(fd, &status) < 0) {
		close(fd);
		return -1;
	}
	if (status.st_size <= 0 || (uintmax_t)status.st_size > GUI_IMAGE_MAX_FILE_BYTES) {
		close(fd);
		errno = status.st_size <= 0 ? EINVAL : EFBIG;
		return -1;
	}
	bytes = malloc((size_t)status.st_size);
	if (bytes == NULL) {
		close(fd);
		return -1;
	}
	if (read_full(fd, bytes, (size_t)status.st_size) < 0) {
		int saved_errno = errno;

		close(fd);
		free(bytes);
		errno = saved_errno;
		return -1;
	}
	if (close(fd) < 0) {
		int saved_errno = errno;

		free(bytes);
		errno = saved_errno;
		return -1;
	}
	*contents = bytes;
	*byte_count = (size_t)status.st_size;
	return 0;
}

static int imageviewer_decode(const uint8_t *bytes, size_t byte_count,
			      gui_surface **image)
{
	static const uint8_t png_signature[PNG_SIGNATURE_BYTES] =
		{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

	if (byte_count >= PNG_SIGNATURE_BYTES &&
	    memcmp(bytes, png_signature, sizeof(png_signature)) == 0)
		return gui_png_decode(bytes, byte_count, image);
	return gui_bmp_decode(bytes, byte_count, image);
}

int main(int argc, char **argv)
{
	imageviewer_state state = { .fd = -1 };
	uint8_t *file_bytes = NULL;
	size_t file_byte_count = 0;
	int status = 1;

	if (argc != 2)
		return 1;
	if (imageviewer_read_file(argv[1], &file_bytes, &file_byte_count) < 0 ||
	    imageviewer_decode(file_bytes, file_byte_count, &state.image) < 0) {
		fprintf(stderr, "imageviewer: load failed: %d\n", errno);
		goto out;
	}
	free(file_bytes);
	file_bytes = NULL;
	state.fd = imageviewer_connect();
	if (state.fd < 0 || imageviewer_create_window(&state) < 0 ||
	    imageviewer_resize_surface(&state, state.width, state.height) < 0 ||
	    imageviewer_send_current_surface(&state) < 0) {
		fprintf(stderr, "imageviewer: window setup failed\n");
		goto out;
	}
	puts("imageviewer: window ready");
	fflush(stdout);
	status = 0;
	while (!state.close_requested) {
		gui_protocol_header header;
		int result;

		if (read_message(state.fd, &header, imageviewer_input,
			 sizeof(imageviewer_input)) < 0 ||
		    (result = imageviewer_handle_message(&state, &header, imageviewer_input,
			header.length - GUI_PROTOCOL_HEADER_SIZE)) < 0)
			break;
		if (result > 0 && imageviewer_send_current_surface(&state) < 0)
			break;
	}
	if (state.close_requested) {
		uint8_t window_id[sizeof(gui_protocol_wire_window_id)];

		put32(window_id, state.window_id);
		(void)send_message(state.fd, GUI_PROTOCOL_WINDOW_DESTROY, window_id,
			sizeof(window_id));
	}
out:
	free(file_bytes);
	gui_surface_destroy(state.surface);
	gui_surface_destroy(state.image);
	if (state.fd >= 0)
		close(state.fd);
	return status;
}
