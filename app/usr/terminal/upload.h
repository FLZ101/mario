#ifndef TERMINAL_UPLOAD_H
#define TERMINAL_UPLOAD_H

#include <stdint.h>
#include <gui/protocol.h>
#include <gui/surface.h>

#define TERMINAL_UPLOAD_WINDOW_BYTES (16U * 1024U)
#define TERMINAL_UPLOAD_CHUNKS 4U
#define TERMINAL_UPLOAD_PIXELS 1000U
#define TERMINAL_UPLOAD_FRAME_BYTES (GUI_PROTOCOL_HEADER_SIZE + \
	sizeof(gui_protocol_wire_surface_data) + TERMINAL_UPLOAD_PIXELS * sizeof(gui_color))

typedef struct {
	uint32_t offset;
	uint32_t count;
	size_t bytes;
	int sent;
} terminal_upload_chunk;

typedef struct {
	int active;
	int begun;
	int canceled;
	int send_cancel;
	int patch;
	int completed;
	uint32_t window;
	uint64_t configure;
	uint64_t serial;
	uint64_t base;
	uint64_t completed_serial;
	uint64_t completed_configure;
	gui_surface source;
	gui_rect rectangle;
	uint32_t total;
	uint32_t next_pixel;
	terminal_upload_chunk chunks[TERMINAL_UPLOAD_CHUNKS];
	unsigned int chunk_head;
	unsigned int chunk_count;
	size_t outstanding_bytes;
	size_t peak_outstanding_bytes;
	uint8_t frame[TERMINAL_UPLOAD_FRAME_BYTES];
	size_t frame_bytes;
	size_t frame_written;
	gui_protocol_type frame_type;
} terminal_upload;

typedef ssize_t (*terminal_upload_writer)(void *context, const void *bytes, size_t count);

int terminal_upload_start(terminal_upload *upload, uint32_t window, uint64_t configure,
	uint64_t base, uint64_t serial, const gui_surface *source, gui_rect rectangle, int patch);
int terminal_upload_ready(const terminal_upload *upload);
int terminal_upload_writable(const terminal_upload *upload);
int terminal_upload_write(terminal_upload *upload, terminal_upload_writer writer, void *context);
int terminal_upload_reply(terminal_upload *upload, const uint8_t *payload, size_t bytes);
int terminal_upload_data_error(terminal_upload *upload);
void terminal_upload_cancel(terminal_upload *upload, int send_cancel);
int terminal_upload_control(terminal_upload *upload, gui_protocol_type type,
	const void *payload, size_t bytes);

#endif
