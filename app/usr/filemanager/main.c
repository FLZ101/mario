#include "model.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gui/protocol.h>
#include <gui/font.h>
#include <gui/input.h>
#include <gui/surface.h>
#include <app/gui-limits.h>

#define WM_SOCKET_PATH "/tmp/mario-wm.sock"
#define FILEMANAGER_WIDTH 320U
#define FILEMANAGER_HEIGHT 192U
#define FILEMANAGER_TITLE "File Manager"
#define FILEMANAGER_BACKGROUND 0x00111827U
#define FILEMANAGER_FOREGROUND 0x00f8fafcU
#define FILEMANAGER_HEADER 0x0094a3b8U
#define FILEMANAGER_SELECTION 0x001d4ed8U
#define FILEMANAGER_FONT_PATH "/usr/share/fonts/DejaVuSansMono.ttf"
#define FILEMANAGER_FONT_HEIGHT 14
#define FILEMANAGER_MARGIN 6
#define FILEMANAGER_HEADER_Y 6
#define FILEMANAGER_COLUMNS_Y 24
#define FILEMANAGER_ROWS_Y 24
#define FILEMANAGER_PATH_ROWS_Y 42
#define FILEMANAGER_ROW_HEIGHT 16
#define FILEMANAGER_NAME_X 6
#define FILEMANAGER_TYPE_X 112
#define FILEMANAGER_SIZE_X 170
#define FILEMANAGER_TIME_X 228
#define FILEMANAGER_PATH_CHARS 35U
#define FILEMANAGER_INPUT_VALUE_X 64
#define FILEMANAGER_NAME_CHARS 13U
#define FILEMANAGER_TYPE_CHARS 7U
#define FILEMANAGER_SIZE_CHARS 7U
#define FILEMANAGER_TIME_CHARS 10U
#define SURFACE_PIXELS_PER_MESSAGE 1000U
#define FILEMANAGER_COPY_BYTES 4096U
#define FILEMANAGER_MESSAGE_RECONFIGURE 2

typedef enum {
	FILEMANAGER_INPUT_NONE,
	FILEMANAGER_INPUT_PATH,
	FILEMANAGER_INPUT_MKDIR,
	FILEMANAGER_INPUT_RENAME,
	FILEMANAGER_INPUT_CONFIRM_OVERWRITE,
	FILEMANAGER_INPUT_CONFIRM_DELETE_TREE,
} filemanager_input_mode;

typedef enum {
	FILEMANAGER_CLIPBOARD_NONE,
	FILEMANAGER_CLIPBOARD_COPY,
	FILEMANAGER_CLIPBOARD_MOVE,
} filemanager_clipboard_mode;

typedef struct {
	int fd;
	uint32_t window_id;
	uint64_t configure_serial;
	uint64_t frame_serial;
	uint32_t width;
	uint32_t height;
	int close_requested;
	app_gui_surface_flow surface_flow;
	size_t selected_entry;
	filemanager_input_mode input_mode;
	filemanager_clipboard_mode clipboard_mode;
	uint32_t launch_request_id;
	int redraw_pending;
	char *path;
	char *input;
	char *rename_source;
	char *clipboard_path;
	char status[32];
	gui_font *font;
	gui_surface *surface;
} filemanager_state;

/* Keep protocol frames off Mario's intentionally small initial stack. */
static uint8_t filemanager_frame[GUI_PROTOCOL_HEADER_SIZE +
	sizeof(gui_protocol_wire_surface_data) + SURFACE_PIXELS_PER_MESSAGE * sizeof(uint32_t)];
static uint8_t filemanager_input[GUI_PROTOCOL_MAX_MESSAGE_SIZE - GUI_PROTOCOL_HEADER_SIZE];
/* The bounded listing is larger than Mario's initial stack budget. */
static filemanager_directory filemanager_directory_model;
/* Path input is also bounded but should not inflate the initial application stack. */
static char filemanager_path[GUI_PROTOCOL_MAX_PATH_BYTES + 1];
static char filemanager_input_entry[GUI_PROTOCOL_MAX_PATH_BYTES + 1];
static char filemanager_rename_source[FILEMANAGER_NAME_BYTES + 1];
/* Mutations need two complete paths without growing Mario's initial app stack. */
static char filemanager_operation_source[GUI_PROTOCOL_MAX_PATH_BYTES + 1];
static char filemanager_operation_target[GUI_PROTOCOL_MAX_PATH_BYTES + 1];
static char filemanager_resolved_path[PATH_MAX];
static char filemanager_clipboard_path[GUI_PROTOCOL_MAX_PATH_BYTES + 1];
static uint8_t filemanager_copy_buffer[FILEMANAGER_COPY_BYTES];

static int filemanager_name_has_suffix(const char *name, const char *suffix)
{
	size_t name_bytes = strlen(name);
	size_t suffix_bytes = strlen(suffix);

	return name_bytes >= suffix_bytes &&
		!strcmp(name + name_bytes - suffix_bytes, suffix);
}

static void put16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
}

static uint16_t get16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
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

static int filemanager_resize_surface(filemanager_state *state, uint32_t width,
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

static const char *filemanager_type_name(filemanager_entry_type type)
{
	switch (type) {
	case FILEMANAGER_ENTRY_DIRECTORY: return "dir";
	case FILEMANAGER_ENTRY_FILE: return "file";
	case FILEMANAGER_ENTRY_SYMLINK: return "link";
	case FILEMANAGER_ENTRY_OTHER: return "other";
	case FILEMANAGER_ENTRY_UNKNOWN: return "unknown";
	}
	return "unknown";
}

static size_t filemanager_utf8_bytes(const char *text)
{
	unsigned char first = (unsigned char)text[0];
	size_t bytes = 1;

	if ((first & 0xe0) == 0xc0)
		bytes = 2;
	else if ((first & 0xf0) == 0xe0)
		bytes = 3;
	else if ((first & 0xf8) == 0xf0)
		bytes = 4;
	for (size_t index = 1; index < bytes; index++)
		if ((unsigned char)text[index] == 0 || ((unsigned char)text[index] & 0xc0) != 0x80)
			return 1;
	return bytes;
}

static size_t filemanager_utf8_count(const char *text, size_t limit)
{
	size_t count = 0;

	while (*text != '\0' && count < limit) {
		text += filemanager_utf8_bytes(text);
		count++;
	}
	return count;
}

static void filemanager_clip_text(char *destination, size_t destination_bytes,
				  const char *source, size_t character_limit)
{
	size_t source_count = filemanager_utf8_count(source, character_limit + 1);
	size_t kept = source_count > character_limit ? character_limit - 3 : source_count;
	size_t written = 0;

	while (*source != '\0' && kept != 0) {
		size_t bytes = filemanager_utf8_bytes(source);

		if (written + bytes + 1 > destination_bytes)
			break;
		memcpy(destination + written, source, bytes);
		written += bytes;
		source += bytes;
		kept--;
	}
	if (source_count > character_limit && written + 4 <= destination_bytes) {
		memcpy(destination + written, "...", 3);
		written += 3;
	}
	destination[written] = '\0';
}

static int filemanager_validate_path(const char *path)
{
	size_t path_bytes;

	if (path == NULL) {
		errno = EINVAL;
		return -1;
	}
	path_bytes = strnlen(path, GUI_PROTOCOL_MAX_PATH_BYTES + 1);
	if (path[0] != '/' || path_bytes == 0 || path_bytes > GUI_PROTOCOL_MAX_PATH_BYTES) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int filemanager_parent_path(char *destination, size_t destination_bytes,
				   const char *path)
{
	size_t path_bytes;

	if (destination == NULL || destination_bytes < GUI_PROTOCOL_MAX_PATH_BYTES + 1 ||
	    filemanager_validate_path(path) < 0)
		return -1;
	path_bytes = strlen(path);
	while (path_bytes > 1 && path[path_bytes - 1] == '/')
		path_bytes--;
	while (path_bytes > 1 && path[path_bytes - 1] != '/')
		path_bytes--;
	if (path_bytes > 1)
		path_bytes--;
	memcpy(destination, path, path_bytes);
	destination[path_bytes] = '\0';
	return 0;
}

static int filemanager_child_path(char *destination, size_t destination_bytes,
				  const char *path, const char *name)
{
	size_t path_bytes;
	size_t name_bytes;

	if (destination == NULL || name == NULL ||
	    destination_bytes < GUI_PROTOCOL_MAX_PATH_BYTES + 1 ||
	    filemanager_validate_path(path) < 0)
		return -1;
	path_bytes = strlen(path);
	while (path_bytes > 1 && path[path_bytes - 1] == '/')
		path_bytes--;
	name_bytes = strnlen(name, FILEMANAGER_NAME_BYTES + 1);
	if (name_bytes == 0 || name_bytes > FILEMANAGER_NAME_BYTES ||
	    path_bytes + (path_bytes == 1 ? 0 : 1) + name_bytes > GUI_PROTOCOL_MAX_PATH_BYTES) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(destination, path, path_bytes);
	if (path_bytes != 1)
		destination[path_bytes++] = '/';
	memcpy(destination + path_bytes, name, name_bytes);
	destination[path_bytes + name_bytes] = '\0';
	return 0;
}

static int filemanager_is_scratch_path(const char *path)
{
	if (realpath(path, filemanager_resolved_path) == NULL)
		return 0;
	return !strcmp(filemanager_resolved_path, "/tmp") ||
		!strncmp(filemanager_resolved_path, "/tmp/", sizeof("/tmp/") - 1);
}

static int filemanager_validate_entry_name(const char *name)
{
	size_t name_bytes;

	if (name == NULL) {
		errno = EINVAL;
		return -1;
	}
	name_bytes = strnlen(name, FILEMANAGER_NAME_BYTES + 1);
	if (name_bytes == 0 || name_bytes > FILEMANAGER_NAME_BYTES || strchr(name, '/') != NULL ||
	    !strcmp(name, ".") || !strcmp(name, "..")) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static void filemanager_set_error(filemanager_state *state, const char *operation,
				  const char *path, int error)
{
	(void)snprintf(state->status, sizeof(state->status), "%s error: %d", operation, error);
	fprintf(stderr, "filemanager: %s failed: %s: %d\n", operation, path, error);
	fflush(stderr);
}

static int filemanager_copy_write_all(int fd, const uint8_t *bytes, size_t byte_count)
{
	while (byte_count != 0) {
		ssize_t count = write(fd, bytes, byte_count);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (count == 0)
				errno = EIO;
			return -1;
		}
		bytes += count;
		byte_count -= count;
	}
	return 0;
}

static int filemanager_copy_file(const char *source, const char *target, int overwrite)
{
	struct stat status;
	int source_fd = -1;
	int target_fd = -1;
	int saved_errno = 0;

	if ((source_fd = open(source, O_RDONLY)) < 0 || fstat(source_fd, &status) < 0 ||
	    !S_ISREG(status.st_mode)) {
		saved_errno = errno == 0 ? EINVAL : errno;
		goto out;
	}
	target_fd = open(target, O_WRONLY | O_CREAT | (overwrite ? O_TRUNC : O_EXCL),
		status.st_mode & 0777);
	if (target_fd < 0) {
		saved_errno = errno;
		goto out;
	}
	for (;;) {
		ssize_t count = read(source_fd, filemanager_copy_buffer, sizeof(filemanager_copy_buffer));

		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			saved_errno = errno;
			goto out;
		}
		if (count == 0)
			break;
		if (filemanager_copy_write_all(target_fd, filemanager_copy_buffer, count) < 0) {
			saved_errno = errno;
			goto out;
		}
	}
	if (fsync(target_fd) < 0) {
		saved_errno = errno;
		goto out;
	}
out:
	if (target_fd >= 0 && close(target_fd) < 0 && saved_errno == 0)
		saved_errno = errno;
	if (source_fd >= 0)
		(void)close(source_fd);
	if (saved_errno != 0 && !overwrite)
		(void)unlink(target);
	if (saved_errno != 0) {
		errno = saved_errno;
		return -1;
	}
	return 0;
}

static int filemanager_remove_tree(const char *path)
{
	DIR *stream;
	char *child_path;
	int directory_fd;
	int saved_errno = 0;

	directory_fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (directory_fd < 0)
		return -1;
	stream = fdopendir(directory_fd);
	if (stream == NULL) {
		saved_errno = errno;
		close(directory_fd);
		errno = saved_errno;
		return -1;
	}
	child_path = malloc(GUI_PROTOCOL_MAX_PATH_BYTES + 1);
	if (child_path == NULL) {
		saved_errno = errno;
		closedir(stream);
		errno = saved_errno;
		return -1;
	}
	for (;;) {
		struct dirent *entry;
		struct stat status;

		errno = 0;
		entry = readdir(stream);
		if (entry == NULL) {
			if (errno != 0)
				saved_errno = errno;
			break;
		}
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (filemanager_child_path(child_path, GUI_PROTOCOL_MAX_PATH_BYTES + 1, path,
			entry->d_name) < 0 || lstat(child_path, &status) < 0) {
			saved_errno = errno;
			break;
		}
		if (S_ISDIR(status.st_mode)) {
			if (filemanager_remove_tree(child_path) < 0) {
				saved_errno = errno;
				break;
			}
		} else if (unlink(child_path) < 0) {
			saved_errno = errno;
			break;
		}
	}
	free(child_path);
	if (closedir(stream) < 0 && saved_errno == 0)
		saved_errno = errno;
	if (saved_errno != 0) {
		errno = saved_errno;
		return -1;
	}
	return rmdir(path);
}

static int filemanager_navigate(filemanager_state *state, const char *path)
{
	int saved_errno;

	if (filemanager_validate_path(path) < 0 ||
	    filemanager_directory_load(&filemanager_directory_model, path) < 0) {
		saved_errno = errno;
		(void)snprintf(state->status, sizeof(state->status), "Path error: %d", saved_errno);
		fprintf(stderr, "filemanager: navigation failed: %s: %d\n", path, saved_errno);
		fflush(stderr);
		return 0;
	}
	/* The model commits only after a complete load, so path and rows change together. */
	strcpy(state->path, path);
	state->selected_entry = 0;
	state->input_mode = FILEMANAGER_INPUT_NONE;
	state->input[0] = '\0';
	state->status[0] = '\0';
	printf("filemanager: navigated: %s (%zu entries%s)\n", state->path,
		filemanager_directory_model.count,
		filemanager_directory_model.omitted_count == 0 ? "" : ", truncated");
	fflush(stdout);
	return 1;
}

static int filemanager_refresh(filemanager_state *state)
{
	if (filemanager_directory_load(&filemanager_directory_model, state->path) < 0) {
		int saved_errno = errno;

		filemanager_set_error(state, "refresh", state->path, saved_errno);
		return 0;
	}
	state->selected_entry = 0;
	state->input_mode = FILEMANAGER_INPUT_NONE;
	state->input[0] = '\0';
	state->status[0] = '\0';
	return 1;
}

static int filemanager_mutate(filemanager_state *state)
{
	int saved_errno;

	if (!filemanager_is_scratch_path(state->path)) {
		(void)snprintf(state->status, sizeof(state->status), "Scratch only: /tmp");
		fprintf(stderr, "filemanager: mutation rejected outside /tmp: %s\n", state->path);
		fflush(stderr);
		return 1;
	}
	if (filemanager_validate_entry_name(state->input) < 0) {
		filemanager_set_error(state, "name", state->input, errno);
		return 1;
	}
	if (filemanager_child_path(filemanager_operation_target,
		sizeof(filemanager_operation_target), state->path, state->input) < 0) {
		filemanager_set_error(state, "path", state->input, errno);
		return 1;
	}
	if (state->input_mode == FILEMANAGER_INPUT_MKDIR) {
		if (mkdir(filemanager_operation_target, 0777) < 0) {
			saved_errno = errno;
			filemanager_set_error(state, "mkdir", filemanager_operation_target, saved_errno);
			return 1;
		}
		printf("filemanager: created directory: %s\n", filemanager_operation_target);
	} else {
		struct stat status;

		if (filemanager_child_path(filemanager_operation_source,
			sizeof(filemanager_operation_source), state->path, state->rename_source) < 0) {
			filemanager_set_error(state, "rename", state->rename_source, errno);
			return 1;
		}
		/* This UI does not offer overwrite confirmation for rename targets. */
		if (lstat(filemanager_operation_target, &status) == 0) {
			filemanager_set_error(state, "rename", filemanager_operation_target, EEXIST);
			return 1;
		}
		if (errno != ENOENT) {
			saved_errno = errno;
			filemanager_set_error(state, "rename", filemanager_operation_target, saved_errno);
			return 1;
		}
		if (rename(filemanager_operation_source, filemanager_operation_target) < 0) {
			saved_errno = errno;
			filemanager_set_error(state, "rename", filemanager_operation_target, saved_errno);
			return 1;
		}
		printf("filemanager: renamed: %s -> %s\n", filemanager_operation_source,
			filemanager_operation_target);
	}
	fflush(stdout);
	/* Do not leave a completed mutation active if its post-operation refresh fails. */
	state->input_mode = FILEMANAGER_INPUT_NONE;
	state->input[0] = '\0';
	return filemanager_refresh(state);
}

static int filemanager_stage_clipboard(filemanager_state *state,
				       filemanager_clipboard_mode clipboard_mode)
{
	struct stat status;

	if (state->selected_entry >= filemanager_directory_model.count)
		return 0;
	if (!filemanager_is_scratch_path(state->path)) {
		(void)snprintf(state->status, sizeof(state->status), "Scratch only: /tmp");
		return 1;
	}
	if (filemanager_child_path(state->clipboard_path, GUI_PROTOCOL_MAX_PATH_BYTES + 1,
		state->path, filemanager_directory_model.entries[state->selected_entry].name) < 0 ||
	    lstat(state->clipboard_path, &status) < 0 || !S_ISREG(status.st_mode)) {
		filemanager_set_error(state, clipboard_mode == FILEMANAGER_CLIPBOARD_COPY ? "copy" : "move",
			state->clipboard_path, errno == 0 ? EINVAL : errno);
		return 1;
	}
	state->clipboard_mode = clipboard_mode;
	(void)snprintf(state->status, sizeof(state->status), "%s ready",
		clipboard_mode == FILEMANAGER_CLIPBOARD_COPY ? "Copy" : "Move");
	printf("filemanager: %s staged: %s\n",
		clipboard_mode == FILEMANAGER_CLIPBOARD_COPY ? "copy" : "move",
		state->clipboard_path);
	fflush(stdout);
	return 1;
}

static int filemanager_paste(filemanager_state *state, int overwrite)
{
	struct stat status;
	int saved_errno;

	if (state->clipboard_mode == FILEMANAGER_CLIPBOARD_NONE) {
		filemanager_set_error(state, "paste", "clipboard", EINVAL);
		return 1;
	}
	if (!filemanager_is_scratch_path(state->path)) {
		(void)snprintf(state->status, sizeof(state->status), "Scratch only: /tmp");
		return 1;
	}
	if (filemanager_child_path(filemanager_operation_target,
		sizeof(filemanager_operation_target), state->path,
		strrchr(state->clipboard_path, '/') + 1) < 0) {
		filemanager_set_error(state, "paste", state->clipboard_path, errno);
		return 1;
	}
	if (!strcmp(state->clipboard_path, filemanager_operation_target)) {
		filemanager_set_error(state, "paste", filemanager_operation_target, EINVAL);
		return 1;
	}
	if (!overwrite) {
		if (lstat(filemanager_operation_target, &status) == 0) {
			state->input_mode = FILEMANAGER_INPUT_CONFIRM_OVERWRITE;
			state->status[0] = '\0';
			return 1;
		}
		if (errno != ENOENT) {
			filemanager_set_error(state, "paste", filemanager_operation_target, errno);
			return 1;
		}
	}
	if (state->clipboard_mode == FILEMANAGER_CLIPBOARD_COPY) {
		if (filemanager_copy_file(state->clipboard_path, filemanager_operation_target,
			overwrite) < 0) {
			filemanager_set_error(state, "copy", filemanager_operation_target, errno);
			return 1;
		}
		printf("filemanager: copied: %s -> %s\n", state->clipboard_path,
			filemanager_operation_target);
	} else if (rename(state->clipboard_path, filemanager_operation_target) < 0) {
		saved_errno = errno;
		if (saved_errno != EXDEV || filemanager_copy_file(state->clipboard_path,
			filemanager_operation_target, overwrite) < 0) {
			filemanager_set_error(state, "move", filemanager_operation_target, errno);
			return 1;
		}
		if (unlink(state->clipboard_path) < 0) {
			saved_errno = errno;
			if (!overwrite)
				(void)unlink(filemanager_operation_target);
			filemanager_set_error(state, "move", state->clipboard_path, saved_errno);
			return 1;
		}
		printf("filemanager: moved across filesystems: %s -> %s\n",
			state->clipboard_path, filemanager_operation_target);
	} else {
		printf("filemanager: moved: %s -> %s\n", state->clipboard_path,
			filemanager_operation_target);
	}
	fflush(stdout);
	if (state->clipboard_mode == FILEMANAGER_CLIPBOARD_MOVE) {
		state->clipboard_mode = FILEMANAGER_CLIPBOARD_NONE;
		state->clipboard_path[0] = '\0';
	}
	state->input_mode = FILEMANAGER_INPUT_NONE;
	return filemanager_refresh(state);
}

static int filemanager_delete_selected(filemanager_state *state)
{
	struct stat status;

	if (state->selected_entry >= filemanager_directory_model.count)
		return 0;
	if (!filemanager_is_scratch_path(state->path)) {
		(void)snprintf(state->status, sizeof(state->status), "Scratch only: /tmp");
		return 1;
	}
	if (filemanager_child_path(filemanager_operation_target,
		sizeof(filemanager_operation_target), state->path,
		filemanager_directory_model.entries[state->selected_entry].name) < 0 ||
	    lstat(filemanager_operation_target, &status) < 0) {
		filemanager_set_error(state, "delete", filemanager_operation_target, errno);
		return 1;
	}
	if (S_ISDIR(status.st_mode)) {
		if (rmdir(filemanager_operation_target) == 0) {
			printf("filemanager: deleted directory: %s\n", filemanager_operation_target);
			fflush(stdout);
			return filemanager_refresh(state);
		}
		if (errno == ENOTEMPTY) {
			state->input_mode = FILEMANAGER_INPUT_CONFIRM_DELETE_TREE;
			state->status[0] = '\0';
			return 1;
		}
		filemanager_set_error(state, "delete", filemanager_operation_target, errno);
		return 1;
	}
	if (unlink(filemanager_operation_target) < 0) {
		filemanager_set_error(state, "delete", filemanager_operation_target, errno);
		return 1;
	}
	printf("filemanager: deleted: %s\n", filemanager_operation_target);
	fflush(stdout);
	return filemanager_refresh(state);
}

static int filemanager_delete_tree(filemanager_state *state)
{
	if (filemanager_remove_tree(filemanager_operation_target) < 0) {
		filemanager_set_error(state, "delete", filemanager_operation_target, errno);
		return 1;
	}
	printf("filemanager: deleted tree: %s\n", filemanager_operation_target);
	fflush(stdout);
	state->input_mode = FILEMANAGER_INPUT_NONE;
	return filemanager_refresh(state);
}

static int filemanager_open_selected(filemanager_state *state)
{
	const filemanager_entry *entry;
	struct stat status;
	uint8_t request[sizeof(gui_protocol_wire_launch_request) + GUI_PROTOCOL_MAX_PATH_BYTES];
	uint16_t launch_type;
	size_t path_bytes;

	if (state->selected_entry >= filemanager_directory_model.count)
		return 0;
	entry = &filemanager_directory_model.entries[state->selected_entry];
	if (filemanager_child_path(filemanager_operation_target,
		sizeof(filemanager_operation_target), state->path, entry->name) < 0 ||
		lstat(filemanager_operation_target, &status) < 0) {
		filemanager_set_error(state, "open", entry->name, errno);
		return 1;
	}
	if (!S_ISREG(status.st_mode)) {
		(void)snprintf(state->status, sizeof(state->status), "Open regular files only");
		return 1;
	}
	path_bytes = strlen(filemanager_operation_target);
	launch_type = filemanager_name_has_suffix(entry->name, ".bmp") ||
		filemanager_name_has_suffix(entry->name, ".png") ?
		GUI_PROTOCOL_LAUNCH_IMAGE_VIEWER : status.st_mode & 0111 ?
		GUI_PROTOCOL_LAUNCH_EXECUTABLE : GUI_PROTOCOL_LAUNCH_OPEN_TEXT;
	/* The server echoes this identifier so delayed launch results stay attributable. */
	if (++state->launch_request_id == 0)
		state->launch_request_id++;
	put32(request, state->launch_request_id);
	put16(request + 4, launch_type);
	put16(request + 6, path_bytes);
	memcpy(request + sizeof(gui_protocol_wire_launch_request), filemanager_operation_target,
		path_bytes);
	if (send_message(state->fd, GUI_PROTOCOL_LAUNCH_REQUEST, request,
		sizeof(gui_protocol_wire_launch_request) + path_bytes) < 0) {
		filemanager_set_error(state, "launch", filemanager_operation_target, errno);
		return 1;
	}
	printf("filemanager: launch requested: %s\n", filemanager_operation_target);
	fflush(stdout);
	return 0;
}

static const char *filemanager_input_label(filemanager_input_mode input_mode)
{
	switch (input_mode) {
	case FILEMANAGER_INPUT_PATH: return "Open:";
	case FILEMANAGER_INPUT_MKDIR: return "Mkdir:";
	case FILEMANAGER_INPUT_RENAME: return "Rename:";
	case FILEMANAGER_INPUT_CONFIRM_OVERWRITE: return "Overwrite? y/N";
	case FILEMANAGER_INPUT_CONFIRM_DELETE_TREE: return "Delete tree? y/N";
	default: return "";
	}
}

static int filemanager_handle_input(filemanager_state *state, uint32_t keysym)
{
	size_t input_bytes;

	if (state->input_mode == FILEMANAGER_INPUT_CONFIRM_OVERWRITE ||
	    state->input_mode == FILEMANAGER_INPUT_CONFIRM_DELETE_TREE) {
		if (keysym == 'y')
			return state->input_mode == FILEMANAGER_INPUT_CONFIRM_OVERWRITE ?
				filemanager_paste(state, 1) : filemanager_delete_tree(state);
		if (keysym == GUI_KEYSYM_ESCAPE || keysym == 'n') {
			filemanager_input_mode input_mode = state->input_mode;

			state->input_mode = FILEMANAGER_INPUT_NONE;
			printf("filemanager: %s cancelled\n",
				input_mode == FILEMANAGER_INPUT_CONFIRM_OVERWRITE ? "paste" : "delete");
			fflush(stdout);
			return 1;
		}
		return 0;
	}
	if (keysym == GUI_KEYSYM_ESCAPE) {
		state->input_mode = FILEMANAGER_INPUT_NONE;
		state->input[0] = '\0';
		state->status[0] = '\0';
		return 1;
	}
	input_bytes = strlen(state->input);
	if (keysym == GUI_KEYSYM_BACKSPACE) {
		if (input_bytes == 0)
			return 0;
		state->input[input_bytes - 1] = '\0';
		state->status[0] = '\0';
		return 1;
	}
	if (keysym == '\n') {
		if (state->input_mode == FILEMANAGER_INPUT_PATH)
			(void)filemanager_navigate(state, state->input);
		else
			(void)filemanager_mutate(state);
		return 1;
	}
	if (keysym >= 0x20 && keysym <= 0x7e && input_bytes < GUI_PROTOCOL_MAX_PATH_BYTES) {
		state->input[input_bytes] = (char)keysym;
		state->input[input_bytes + 1] = '\0';
		state->status[0] = '\0';
		return 1;
	}
	return 0;
}

static int filemanager_handle_key(filemanager_state *state, uint32_t keysym)
{
	if (state->input_mode != FILEMANAGER_INPUT_NONE)
		return filemanager_handle_input(state, keysym);
	if (keysym == '/') {
		state->input_mode = FILEMANAGER_INPUT_PATH;
		state->input[0] = '\0';
		state->status[0] = '\0';
		return 1;
	}
	if (keysym == GUI_KEYSYM_BACKSPACE || keysym == GUI_KEYSYM_LEFT || keysym == 'h') {
		if (filemanager_parent_path(state->input, sizeof(filemanager_input_entry),
			state->path) < 0 || !strcmp(state->input, state->path))
			return 0;
		(void)filemanager_navigate(state, state->input);
		return 1;
	}
	if ((keysym == 'm' || keysym == 'r' || keysym == 'y' || keysym == 'd' || keysym == 'p' ||
	     keysym == 'x') &&
	    !filemanager_is_scratch_path(state->path)) {
		(void)snprintf(state->status, sizeof(state->status), "Scratch only: /tmp");
		fprintf(stderr, "filemanager: mutation rejected outside /tmp: %s\n", state->path);
		fflush(stderr);
		return 1;
	}
	if (keysym == 'm') {
		state->input_mode = FILEMANAGER_INPUT_MKDIR;
		state->input[0] = '\0';
		state->status[0] = '\0';
		return 1;
	}
	if (keysym == 'r' && state->selected_entry < filemanager_directory_model.count) {
		strcpy(state->rename_source,
			filemanager_directory_model.entries[state->selected_entry].name);
		state->input_mode = FILEMANAGER_INPUT_RENAME;
		state->input[0] = '\0';
		state->status[0] = '\0';
		return 1;
	}
	if (keysym == 'y')
		return filemanager_stage_clipboard(state, FILEMANAGER_CLIPBOARD_COPY);
	if (keysym == 'd')
		return filemanager_stage_clipboard(state, FILEMANAGER_CLIPBOARD_MOVE);
	if (keysym == 'p')
		return filemanager_paste(state, 0);
	if (keysym == 'x')
		return filemanager_delete_selected(state);
	if (keysym == 'o')
		return filemanager_open_selected(state);
	if (keysym == '\n' && state->selected_entry < filemanager_directory_model.count) {
		/* The transactional load is the final directory check and retains rows on error. */
		if (filemanager_child_path(state->input, sizeof(filemanager_input_entry), state->path,
			filemanager_directory_model.entries[state->selected_entry].name) < 0)
			return 0;
		(void)filemanager_navigate(state, state->input);
		return 1;
	}
	if (filemanager_directory_model.count == 0)
		return 0;
	if ((keysym == GUI_KEYSYM_UP || keysym == 'k') && state->selected_entry != 0) {
		state->selected_entry--;
		return 1;
	}
	if ((keysym == GUI_KEYSYM_DOWN || keysym == 'j') &&
	    state->selected_entry + 1 < filemanager_directory_model.count) {
		state->selected_entry++;
		return 1;
	}
	return 0;
}

static void filemanager_render(filemanager_state *state)
{
	char clipped_path[FILEMANAGER_PATH_CHARS * 4 + 1];
	int columns_y = FILEMANAGER_HEADER_Y;
	uint32_t rows_y = FILEMANAGER_ROWS_Y;
	uint32_t visible_rows;

	gui_fill_rect(state->surface, (gui_rect) { .w = (int)state->width,
		.h = (int)state->height }, FILEMANAGER_BACKGROUND);
	if (state->font == NULL)
		return;
	if (state->status[0] != '\0') {
		gui_font_draw_text(state->surface, FILEMANAGER_NAME_X,
			FILEMANAGER_HEADER_Y + FILEMANAGER_FONT_HEIGHT, state->font, state->status,
			FILEMANAGER_FOREGROUND);
		columns_y = FILEMANAGER_COLUMNS_Y;
		rows_y = FILEMANAGER_PATH_ROWS_Y;
	} else if (state->input_mode == FILEMANAGER_INPUT_CONFIRM_OVERWRITE) {
		gui_font_draw_text(state->surface, FILEMANAGER_NAME_X,
			FILEMANAGER_HEADER_Y + FILEMANAGER_FONT_HEIGHT, state->font,
			filemanager_input_label(state->input_mode), FILEMANAGER_HEADER);
		columns_y = FILEMANAGER_COLUMNS_Y;
		rows_y = FILEMANAGER_PATH_ROWS_Y;
	} else if (state->input_mode != FILEMANAGER_INPUT_NONE) {
		filemanager_clip_text(clipped_path, sizeof(clipped_path), state->input,
			FILEMANAGER_PATH_CHARS - 8);
		gui_font_draw_text(state->surface, FILEMANAGER_NAME_X,
			FILEMANAGER_HEADER_Y + FILEMANAGER_FONT_HEIGHT, state->font,
			filemanager_input_label(state->input_mode),
			FILEMANAGER_HEADER);
		gui_font_draw_text(state->surface, FILEMANAGER_INPUT_VALUE_X,
			FILEMANAGER_HEADER_Y + FILEMANAGER_FONT_HEIGHT, state->font, clipped_path,
			FILEMANAGER_FOREGROUND);
		gui_font_draw_text(state->surface, FILEMANAGER_INPUT_VALUE_X +
			(int)strlen(clipped_path) * 8, FILEMANAGER_HEADER_Y + FILEMANAGER_FONT_HEIGHT,
			state->font, "_", FILEMANAGER_FOREGROUND);
		columns_y = FILEMANAGER_COLUMNS_Y;
		rows_y = FILEMANAGER_PATH_ROWS_Y;
	}
	gui_font_draw_text(state->surface, FILEMANAGER_NAME_X,
		columns_y + FILEMANAGER_FONT_HEIGHT, state->font, "Name",
		FILEMANAGER_HEADER);
	gui_font_draw_text(state->surface, FILEMANAGER_TYPE_X,
		columns_y + FILEMANAGER_FONT_HEIGHT, state->font, "Type",
		FILEMANAGER_HEADER);
	gui_font_draw_text(state->surface, FILEMANAGER_SIZE_X,
		columns_y + FILEMANAGER_FONT_HEIGHT, state->font, "Size",
		FILEMANAGER_HEADER);
	gui_font_draw_text(state->surface, FILEMANAGER_TIME_X,
		columns_y + FILEMANAGER_FONT_HEIGHT, state->font, "Time",
		FILEMANAGER_HEADER);
	visible_rows = state->height <= rows_y ? 0 :
		(state->height - rows_y) / FILEMANAGER_ROW_HEIGHT;
	if (visible_rows > filemanager_directory_model.count)
		visible_rows = filemanager_directory_model.count;
	for (uint32_t row = 0; row < visible_rows; row++) {
		const filemanager_entry *entry = &filemanager_directory_model.entries[row];
		char name[FILEMANAGER_NAME_CHARS * 4 + 1];
		char type[FILEMANAGER_TYPE_CHARS * 4 + 1];
		char size[FILEMANAGER_SIZE_CHARS * 4 + 1];
		char time[FILEMANAGER_TIME_CHARS * 4 + 1];
		char size_text[32];
		char time_text[32];
		int y = (int)rows_y + (int)row * FILEMANAGER_ROW_HEIGHT;

		if (row == state->selected_entry)
			gui_fill_rect(state->surface, (gui_rect) { .x = FILEMANAGER_MARGIN - 2,
				.y = y, .w = (int)state->width - 2 * (FILEMANAGER_MARGIN - 2),
				.h = FILEMANAGER_ROW_HEIGHT }, FILEMANAGER_SELECTION);
		filemanager_clip_text(name, sizeof(name), entry->name, FILEMANAGER_NAME_CHARS);
		filemanager_clip_text(type, sizeof(type), filemanager_type_name(entry->type),
			FILEMANAGER_TYPE_CHARS);
		if (entry->metadata_available) {
			(void)snprintf(size_text, sizeof(size_text), "%llu",
				(unsigned long long)entry->size);
			(void)snprintf(time_text, sizeof(time_text), "%lld",
				(long long)entry->modified_time);
		} else {
			strcpy(size_text, "-");
			strcpy(time_text, "-");
		}
		filemanager_clip_text(size, sizeof(size), size_text, FILEMANAGER_SIZE_CHARS);
		filemanager_clip_text(time, sizeof(time), time_text, FILEMANAGER_TIME_CHARS);
		gui_font_draw_text(state->surface, FILEMANAGER_NAME_X, y + FILEMANAGER_FONT_HEIGHT,
			state->font, name, FILEMANAGER_FOREGROUND);
		gui_font_draw_text(state->surface, FILEMANAGER_TYPE_X, y + FILEMANAGER_FONT_HEIGHT,
			state->font, type, FILEMANAGER_FOREGROUND);
		gui_font_draw_text(state->surface, FILEMANAGER_SIZE_X, y + FILEMANAGER_FONT_HEIGHT,
			state->font, size, FILEMANAGER_FOREGROUND);
		gui_font_draw_text(state->surface, FILEMANAGER_TIME_X, y + FILEMANAGER_FONT_HEIGHT,
			state->font, time, FILEMANAGER_FOREGROUND);
	}
}

static int filemanager_handle_message(filemanager_state *state,
				      const gui_protocol_header *header,
				      const uint8_t *payload, size_t payload_bytes)
{
	int flow_result = app_gui_surface_flow_message(&state->surface_flow,
		state->window_id, header->type, payload, payload_bytes);

	if (flow_result != 0)
		return flow_result < 0 ? -1 : 0;
	if (header->type == GUI_PROTOCOL_WINDOW_CONFIGURE) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_configure) ||
		    get32(payload) != state->window_id ||
		    get64(payload + 4) <= state->configure_serial ||
		    get32(payload + 12) == 0 || get32(payload + 16) == 0 ||
		    filemanager_resize_surface(state, get32(payload + 12), get32(payload + 16)) < 0)
			return -1;
		state->configure_serial = get64(payload + 4);
		app_gui_surface_defer(&state->surface_flow);
		return FILEMANAGER_MESSAGE_RECONFIGURE;
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
	if (header->type == GUI_PROTOCOL_LAUNCH_RESULT) {
		int32_t status;

		if (payload_bytes != sizeof(gui_protocol_wire_launch_result) ||
		    get32(payload) != state->launch_request_id)
			return -1;
		status = (int32_t)get32(payload + 4);
		if (status != 0) {
			(void)snprintf(state->status, sizeof(state->status), "Launch error: %d", -status);
			fprintf(stderr, "filemanager: launch failed: %d\n", -status);
		} else {
			state->status[0] = '\0';
			printf("filemanager: launch accepted\n");
		}
		fflush(stdout);
		return 1;
	}
	if (header->type == GUI_PROTOCOL_APP_EXIT) {
		int refreshed;

		if (payload_bytes != sizeof(gui_protocol_wire_app_exit) ||
		    get32(payload) != state->window_id)
			return -1;
		/* The server sends this only for the executable terminal it launched for us. */
		refreshed = filemanager_refresh(state);
		if (refreshed) {
			printf("filemanager: refreshed after executable exit: %s\n", state->path);
			fflush(stdout);
		}
		return refreshed;
	}
	if (header->type == GUI_PROTOCOL_INPUT_EVENT) {
		if (payload_bytes != sizeof(gui_protocol_wire_input_event) ||
		    get32(payload) != state->window_id)
			return -1;
		return get32(payload + 4) == GUI_INPUT_EVENT_KEY ?
			filemanager_handle_key(state, get32(payload + 8)) : 0;
	}
	return -1;
}

static int filemanager_read_surface_reply(filemanager_state *state,
					  uint64_t frame_serial, uint32_t pixel_offset,
					  uint32_t pixel_count)
{
	for (;;) {
		gui_protocol_header header;
		int result;
		uint64_t configure_serial = state->configure_serial;

		if (read_message(state->fd, &header, filemanager_input,
				 sizeof(filemanager_input)) < 0)
			return -1;
		if (header.type == GUI_PROTOCOL_SURFACE_DATA_REPLY &&
		    !state->surface_flow.discard_reply) {
			if (header.length != GUI_PROTOCOL_HEADER_SIZE +
			    sizeof(gui_protocol_wire_surface_data_reply) ||
			    get32(filemanager_input) != state->window_id ||
			    get64(filemanager_input + 4) != state->configure_serial ||
			    get64(filemanager_input + 12) != frame_serial ||
			    get32(filemanager_input + 20) != pixel_offset ||
			    get32(filemanager_input + 24) != pixel_count)
				return -1;
			return 0;
		}
		result = filemanager_handle_message(state, &header, filemanager_input,
			header.length - GUI_PROTOCOL_HEADER_SIZE);
		if (result < 0 || state->close_requested)
			return -1;
		if (result == FILEMANAGER_MESSAGE_RECONFIGURE ||
		    (header.type == GUI_PROTOCOL_ERROR &&
		     get16(filemanager_input + 4) == GUI_PROTOCOL_SURFACE_BEGIN &&
		     state->surface_flow.retry_pending)) {
			app_gui_surface_discard_reply(&state->surface_flow, configure_serial,
				frame_serial, pixel_offset, pixel_count);
			return FILEMANAGER_MESSAGE_RECONFIGURE;
		}
		if (result > 0) {
			/* Complete this transaction before redrawing changed application state. */
			state->redraw_pending = 1;
		}
	}
}

static int filemanager_send_surface(filemanager_state *state)
{
	uint8_t begin[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_begin)] = { 0 };
	uint8_t commit[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_commit)];
	uint64_t frame_serial = ++state->frame_serial;
	uint32_t total;

	if (frame_serial == 0 || state->height > UINT32_MAX / state->width)
		return -1;
	filemanager_render(state);
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

		if (gui_protocol_encode_header(filemanager_frame, sizeof(filemanager_frame),
				GUI_PROTOCOL_SURFACE_DATA, payload_bytes) < 0)
			return -1;
		put32(filemanager_frame + GUI_PROTOCOL_HEADER_SIZE, state->window_id);
		put64(filemanager_frame + GUI_PROTOCOL_HEADER_SIZE + 4,
			state->configure_serial);
		put64(filemanager_frame + GUI_PROTOCOL_HEADER_SIZE + 12, frame_serial);
		put32(filemanager_frame + GUI_PROTOCOL_HEADER_SIZE + 20, pixel_offset);
		put32(filemanager_frame + GUI_PROTOCOL_HEADER_SIZE + 24, pixel_count);
		for (uint32_t index = 0; index < pixel_count; index++)
			put32(filemanager_frame + GUI_PROTOCOL_HEADER_SIZE +
				sizeof(gui_protocol_wire_surface_data) + index * sizeof(uint32_t),
				state->surface->pixels[pixel_offset + index]);
		if (write_full(state->fd, filemanager_frame,
			GUI_PROTOCOL_HEADER_SIZE + payload_bytes) < 0)
			return -1;
		result = filemanager_read_surface_reply(state, frame_serial, pixel_offset,
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

static int filemanager_send_current_surface(filemanager_state *state)
{
	int result;

	if (state->surface_flow.retry_pending)
		return 0;

	/* A resize may arrive while the server acknowledges a surface chunk. */
	do {
		state->redraw_pending = 0;
		result = filemanager_send_surface(state);
		if (result < 0)
			return -1;
	} while (!state->surface_flow.retry_pending &&
		 (result == FILEMANAGER_MESSAGE_RECONFIGURE || state->redraw_pending));
	return 0;
}

static int filemanager_connect(void)
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

static int filemanager_create_window(filemanager_state *state)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create) +
		sizeof(FILEMANAGER_TITLE) - 1] = { 0 };
	uint8_t payload[sizeof(gui_protocol_wire_window_create_reply)];
	gui_protocol_header header;
	size_t title_bytes = sizeof(FILEMANAGER_TITLE) - 1;

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
	put32(frame + GUI_PROTOCOL_HEADER_SIZE + 4, FILEMANAGER_WIDTH);
	put32(frame + GUI_PROTOCOL_HEADER_SIZE + 8, FILEMANAGER_HEIGHT);
	put16(frame + GUI_PROTOCOL_HEADER_SIZE + 12, title_bytes);
	memcpy(frame + GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create),
		FILEMANAGER_TITLE, title_bytes);
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

int main(int argc, char **argv)
{
	filemanager_state state = { .fd = -1, .path = filemanager_path,
		.input = filemanager_input_entry, .rename_source = filemanager_rename_source,
		.clipboard_path = filemanager_clipboard_path };
	const char *path = argc == 2 ? argv[1] : "/";
	const char *setup_step = "connect";
	int status = 1;

	if ((argc != 1 && argc != 2) || filemanager_validate_path(path) < 0 ||
	    filemanager_directory_load(&filemanager_directory_model, path) < 0) {
		fprintf(stderr, "filemanager: directory load failed: %d\n", errno);
		goto out;
	}
	strcpy(state.path, path);
	state.fd = filemanager_connect();
	if (state.fd < 0) {
		fprintf(stderr, "filemanager: %s failed: %d\n", setup_step, errno);
		goto out;
	}
	setup_step = "create window";
	if (filemanager_create_window(&state) < 0) {
		fprintf(stderr, "filemanager: %s failed: %d\n", setup_step, errno);
		goto out;
	}
	setup_step = "allocate surface";
	if (filemanager_resize_surface(&state, state.width, state.height) < 0) {
		fprintf(stderr, "filemanager: %s failed: %d\n", setup_step, errno);
		goto out;
	}
	setup_step = "load font";
	state.font = gui_font_load(FILEMANAGER_FONT_PATH, FILEMANAGER_FONT_HEIGHT);
	if (state.font == NULL) {
		fprintf(stderr, "filemanager: %s failed: %d\n", setup_step, errno);
		goto out;
	}
	setup_step = "submit surface";
	if (filemanager_send_current_surface(&state) < 0) {
		fprintf(stderr, "filemanager: %s failed: %d\n", setup_step, errno);
		goto out;
	}
	printf("filemanager: window ready: %s (%zu entries%s)\n", path,
		filemanager_directory_model.count,
		filemanager_directory_model.omitted_count == 0 ? "" : ", truncated");
	fflush(stdout);
	status = 0;
	while (!state.close_requested) {
		gui_protocol_header header;
		int result;
		struct pollfd descriptor = { .fd = state.fd, .events = POLLIN };

		result = poll(&descriptor, 1, state.surface_flow.retry_pending ?
			APP_GUI_SURFACE_RETRY_MS : -1);
		if (result < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (result == 0) {
			if (app_gui_surface_retry_ready(&state.surface_flow) &&
			    filemanager_send_current_surface(&state) < 0)
				break;
			continue;
		}

		if (read_message(state.fd, &header, filemanager_input,
			 sizeof(filemanager_input)) < 0 ||
		    (result = filemanager_handle_message(&state, &header, filemanager_input,
			header.length - GUI_PROTOCOL_HEADER_SIZE)) < 0)
			break;
		if (result > 0 && filemanager_send_current_surface(&state) < 0)
			break;
		if (app_gui_surface_retry_ready(&state.surface_flow) &&
		    filemanager_send_current_surface(&state) < 0)
			break;
	}
	if (state.close_requested) {
		uint8_t window_id[sizeof(gui_protocol_wire_window_id)];

		put32(window_id, state.window_id);
		(void)send_message(state.fd, GUI_PROTOCOL_WINDOW_DESTROY, window_id,
			sizeof(window_id));
	}
out:
	gui_font_destroy(state.font);
	gui_surface_destroy(state.surface);
	if (state.fd >= 0)
		close(state.fd);
	return status;
}
