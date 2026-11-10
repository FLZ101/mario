#include <errno.h>
#include <dirent.h>
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
#include <gui/image_cache.h>
#include <gui/image_view.h>
#include <gui/input.h>
#include <gui/protocol.h>
#include <gui/surface.h>

#define WM_SOCKET_PATH "/tmp/mario-wm.sock"
#define IMAGEVIEWER_WIDTH 320U
#define IMAGEVIEWER_HEIGHT 256U
#define IMAGEVIEWER_TITLE "Image Viewer"
#define IMAGEVIEWER_BACKGROUND 0x00111827U
#define SURFACE_PIXELS_PER_MESSAGE 1000U
#define PNG_SIGNATURE_BYTES 8U
#define IMAGEVIEWER_PAN_STEP 256

static char imageviewer_directory_path[PATH_MAX];
static char imageviewer_candidate_path[PATH_MAX];
static char imageviewer_previous_path[PATH_MAX];

typedef struct {
	int fd;
	uint32_t window_id;
	uint64_t configure_serial;
	uint64_t frame_serial;
	uint32_t width;
	uint32_t height;
	int close_requested;
	gui_image_cache image_cache;
	gui_image_view view;
	int dragging;
	int32_t drag_x;
	int32_t drag_y;
	gui_surface *surface;
} imageviewer_state;

/* Keep the largest outbound frame out of Mario's intentionally small initial stack. */
static uint8_t imageviewer_frame[GUI_PROTOCOL_HEADER_SIZE +
	sizeof(gui_protocol_wire_surface_data) +
	SURFACE_PIXELS_PER_MESSAGE * sizeof(uint32_t)];
static uint8_t imageviewer_input[GUI_PROTOCOL_MAX_MESSAGE_SIZE - GUI_PROTOCOL_HEADER_SIZE];

static int imageviewer_navigate(imageviewer_state *state, int direction);

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
	gui_surface *image = gui_image_cache_current(&state->image_cache);
	gui_image_view_layout layout;

	gui_fill_rect(state->surface, (gui_rect) { .w = (int)state->width,
		.h = (int)state->height }, IMAGEVIEWER_BACKGROUND);
	if (gui_image_view_prepare_layout(&state->view, (uint32_t)image->width,
		(uint32_t)image->height, state->width, state->height, &layout) < 0)
		return;
	for (uint32_t y = 0; y < layout.height; y++) {
		uint32_t source_y = ((uint64_t)layout.scaled_y + y) *
			layout.scale_denominator / layout.scale_numerator;
		gui_color *destination = state->surface->pixels +
			(size_t)(layout.destination_y + y) * state->surface->stride +
			layout.destination_x;
		const gui_color *source = image->pixels + (size_t)source_y * image->stride;

		for (uint32_t x = 0; x < layout.width; x++) {
			uint32_t source_x = ((uint64_t)layout.scaled_x + x) *
				layout.scale_denominator / layout.scale_numerator;

			destination[x] = source[source_x];
		}
	}
}

static int imageviewer_handle_key(imageviewer_state *state, uint32_t keysym)
{
	gui_surface *image = gui_image_cache_current(&state->image_cache);
	int32_t pan_x = 0;
	int32_t pan_y = 0;

	if (keysym == 'f') {
		gui_image_view_fit(&state->view);
		return 1;
	}
	if (keysym == '1')
		return gui_image_view_set_zoom(&state->view, GUI_IMAGE_VIEW_ZOOM_ONE) < 0 ?
			-1 : 1;
	if (keysym == '+' || keysym == '=')
		return gui_image_view_zoom(&state->view, 1);
	if (keysym == '-')
		return gui_image_view_zoom(&state->view, -1);
	if (keysym == 'n' || keysym == GUI_KEYSYM_PAGE_DOWN)
		return imageviewer_navigate(state, 1);
	if (keysym == 'p' || keysym == GUI_KEYSYM_PAGE_UP)
		return imageviewer_navigate(state, -1);
	if (keysym == GUI_KEYSYM_LEFT || keysym == 'h')
		pan_x = IMAGEVIEWER_PAN_STEP;
	else if (keysym == GUI_KEYSYM_RIGHT || keysym == 'l')
		pan_x = -IMAGEVIEWER_PAN_STEP;
	else if (keysym == GUI_KEYSYM_UP || keysym == 'k')
		pan_y = IMAGEVIEWER_PAN_STEP;
	else if (keysym == GUI_KEYSYM_DOWN || keysym == 'j')
		pan_y = -IMAGEVIEWER_PAN_STEP;
	else
		return 0;
	return gui_image_view_pan(&state->view, (uint32_t)image->width,
		(uint32_t)image->height, state->width, state->height, pan_x, pan_y);
}

static int imageviewer_handle_mouse(imageviewer_state *state, const uint8_t *payload)
{
	gui_surface *image = gui_image_cache_current(&state->image_cache);
	int32_t x = (int32_t)get32(payload + 16);
	int32_t y = (int32_t)get32(payload + 20);
	int result;

	if ((payload[24] & 1) == 0) {
		state->dragging = 0;
		return 0;
	}
	if (!state->dragging) {
		state->dragging = 1;
		state->drag_x = x;
		state->drag_y = y;
		return 0;
	}
	/* Dragging the image right/down moves the source viewport left/up. */
	result = gui_image_view_pan(&state->view, (uint32_t)image->width,
		(uint32_t)image->height, state->width, state->height,
		state->drag_x - x, state->drag_y - y);
	state->drag_x = x;
	state->drag_y = y;
	return result;
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
	if (header->type == GUI_PROTOCOL_INPUT_RESET) {
		if (payload_bytes != 0)
			return -1;
		state->dragging = 0;
		return 0;
	}
	if (header->type == GUI_PROTOCOL_INPUT_EVENT) {
		if (payload_bytes != sizeof(gui_protocol_wire_input_event) ||
		    get32(payload) != state->window_id)
			return -1;
		if (get32(payload + 4) == GUI_INPUT_EVENT_KEY)
			return imageviewer_handle_key(state, get32(payload + 8));
		return get32(payload + 4) == GUI_INPUT_EVENT_MOUSE ?
			imageviewer_handle_mouse(state, payload) : 0;
	}
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

static int imageviewer_extension_matches(const char *path, const char *extension)
{
	const char *suffix = strrchr(path, '.');

	return suffix != NULL && strcmp(suffix, extension) == 0;
}

static int imageviewer_path_is_supported(const char *path)
{
	return imageviewer_extension_matches(path, ".bmp") ||
		imageviewer_extension_matches(path, ".png");
}

static int imageviewer_decode(const char *path, const uint8_t *bytes, size_t byte_count,
			      gui_surface **image)
{
	static const uint8_t png_signature[PNG_SIGNATURE_BYTES] =
		{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	int is_png = byte_count >= PNG_SIGNATURE_BYTES &&
		memcmp(bytes, png_signature, sizeof(png_signature)) == 0;
	int is_bmp = byte_count >= 2 && bytes[0] == 'B' && bytes[1] == 'M';
	int result;

	/* Prefer the filename type, but recover when a valid image has a wrong suffix. */
	if (imageviewer_extension_matches(path, ".png")) {
		result = gui_png_decode(bytes, byte_count, image);
		return result == 0 || !is_bmp ? result : gui_bmp_decode(bytes, byte_count, image);
	}
	if (imageviewer_extension_matches(path, ".bmp")) {
		result = gui_bmp_decode(bytes, byte_count, image);
		return result == 0 || !is_png ? result : gui_png_decode(bytes, byte_count, image);
	}
	return is_png ? gui_png_decode(bytes, byte_count, image) : gui_bmp_decode(bytes,
		byte_count, image);
}

static int imageviewer_load(const char *path, gui_surface **image)
{
	uint8_t *bytes = NULL;
	size_t byte_count = 0;
	int result;

	if (imageviewer_read_file(path, &bytes, &byte_count) < 0)
		return -1;
	result = imageviewer_decode(path, bytes, byte_count, image);
	free(bytes);
	return result;
}

static const char *imageviewer_basename(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash == NULL ? path : slash + 1;
}

static int imageviewer_directory(const char *path)
{
	const char *slash = strrchr(path, '/');
	size_t length;

	if (slash == NULL) {
		strcpy(imageviewer_directory_path, ".");
		return 0;
	}
	length = slash == path ? 1 : (size_t)(slash - path);
	if (length >= sizeof(imageviewer_directory_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(imageviewer_directory_path, path, length);
	imageviewer_directory_path[length] = '\0';
	return 0;
}

static int imageviewer_join_path(const char *name, char path[PATH_MAX])
{
	size_t directory_bytes = strlen(imageviewer_directory_path);
	size_t name_bytes = strlen(name);
	int separator = directory_bytes != 0 && imageviewer_directory_path[directory_bytes - 1] != '/';

	if (directory_bytes + separator + name_bytes >= PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(path, imageviewer_directory_path, directory_bytes);
	if (separator)
		path[directory_bytes++] = '/';
	memcpy(path + directory_bytes, name, name_bytes + 1);
	return 0;
}

static int imageviewer_find_sibling(const char *current_path, int direction)
{
	DIR *directory;
	const char *current_name = imageviewer_basename(current_path);
	int found_current = 0;

	if (imageviewer_directory(current_path) < 0)
		return -1;
	directory = opendir(imageviewer_directory_path);
	if (directory == NULL)
		return -1;
	imageviewer_candidate_path[0] = '\0';
	imageviewer_previous_path[0] = '\0';
	for (;;) {
		struct dirent *entry = readdir(directory);
		struct stat status;

		if (entry == NULL)
			break;
		if (!imageviewer_path_is_supported(entry->d_name) ||
		    imageviewer_join_path(entry->d_name, imageviewer_candidate_path) < 0 ||
		    stat(imageviewer_candidate_path, &status) < 0 || !S_ISREG(status.st_mode))
			continue;
		if (direction > 0 && found_current && strcmp(entry->d_name, current_name) != 0)
			break;
		if (strcmp(entry->d_name, current_name) == 0) {
			found_current = 1;
			if (direction < 0) {
				if (imageviewer_previous_path[0] != '\0')
					strcpy(imageviewer_candidate_path, imageviewer_previous_path);
				else
					imageviewer_candidate_path[0] = '\0';
				break;
			}
			continue;
		}
		if (direction < 0)
			strcpy(imageviewer_previous_path, imageviewer_candidate_path);
	}
	if (closedir(directory) < 0)
		return -1;
	if (!found_current || imageviewer_candidate_path[0] == '\0') {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

static void imageviewer_preload_next(imageviewer_state *state)
{
	gui_surface *image = NULL;
	const char *current_path = gui_image_cache_current_path(&state->image_cache);

	if (imageviewer_find_sibling(current_path, 1) < 0 ||
	    (gui_image_cache_next_path(&state->image_cache) != NULL &&
	     strcmp(gui_image_cache_next_path(&state->image_cache), imageviewer_candidate_path) == 0) ||
	    imageviewer_load(imageviewer_candidate_path, &image) < 0)
		return;
	if (gui_image_cache_set_next(&state->image_cache, imageviewer_candidate_path, image) == 0)
		image = NULL;
	gui_surface_destroy(image);
}

static int imageviewer_navigate(imageviewer_state *state, int direction)
{
	gui_surface *image = NULL;
	const char *current_path = gui_image_cache_current_path(&state->image_cache);

	if (imageviewer_find_sibling(current_path, direction) < 0)
		return 0;
	if (direction > 0 && gui_image_cache_next_path(&state->image_cache) != NULL &&
	    strcmp(gui_image_cache_next_path(&state->image_cache), imageviewer_candidate_path) == 0) {
		if (gui_image_cache_promote_next(&state->image_cache) < 0)
			return -1;
	} else if (imageviewer_load(imageviewer_candidate_path, &image) < 0 ||
		   gui_image_cache_set_current(&state->image_cache, imageviewer_candidate_path,
			image) < 0) {
		gui_surface_destroy(image);
		return 0;
	} else
		image = NULL;
	gui_image_view_fit(&state->view);
	imageviewer_preload_next(state);
	return 1;
}

int main(int argc, char **argv)
{
	imageviewer_state state = { .fd = -1 };
	uint8_t *file_bytes = NULL;
	size_t file_byte_count = 0;
	gui_surface *image = NULL;
	int status = 1;

	if (argc != 2)
		return 1;
	gui_image_cache_init(&state.image_cache);
	gui_image_view_init(&state.view);
	if (imageviewer_read_file(argv[1], &file_bytes, &file_byte_count) < 0 ||
	    imageviewer_decode(argv[1], file_bytes, file_byte_count, &image) < 0 ||
	    gui_image_cache_set_current(&state.image_cache, argv[1], image) < 0) {
		fprintf(stderr, "imageviewer: load failed: %d\n", errno);
		goto out;
	}
	image = NULL;
	free(file_bytes);
	file_bytes = NULL;
	imageviewer_preload_next(&state);
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
	gui_surface_destroy(image);
	gui_surface_destroy(state.surface);
	gui_image_cache_destroy(&state.image_cache);
	if (state.fd >= 0)
		close(state.fd);
	return status;
}
