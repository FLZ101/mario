#ifndef GUI_SURFACE_UPLOAD_H
#define GUI_SURFACE_UPLOAD_H

#include <stdint.h>
#include <gui/protocol.h>
#include <gui/surface.h>

#define GUI_SURFACE_UPLOAD_WINDOW_BYTES (16U * 1024U)
#define GUI_SURFACE_UPLOAD_BATCH_BYTES (64U * 1024U)
#define GUI_SURFACE_UPLOAD_CHUNKS 4U
#define GUI_SURFACE_UPLOAD_PIXELS 1000U
#define GUI_SURFACE_UPLOAD_BATCH_PIXELS 4000U
#define GUI_SURFACE_UPLOAD_FRAME_BYTES (GUI_PROTOCOL_HEADER_SIZE + \
	sizeof(gui_protocol_wire_surface_data) + GUI_SURFACE_UPLOAD_BATCH_PIXELS * sizeof(gui_color))

typedef struct {
	uint32_t offset;
	uint32_t count;
	size_t bytes;
	int sent;
} gui_surface_upload_chunk;

typedef struct {
	int batch;
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
	gui_surface_upload_chunk chunks[GUI_SURFACE_UPLOAD_CHUNKS];
	unsigned int chunk_head;
	unsigned int chunk_count;
	size_t outstanding_bytes;
	size_t peak_outstanding_bytes;
	uint8_t frame[GUI_SURFACE_UPLOAD_FRAME_BYTES];
	size_t frame_bytes;
	size_t frame_written;
	gui_protocol_type frame_type;
} gui_surface_upload;

typedef ssize_t (*gui_surface_upload_writer)(void *context, const void *bytes, size_t count);

int gui_surface_upload_start(gui_surface_upload *upload, uint32_t window, uint64_t configure,
	uint64_t base, uint64_t serial, const gui_surface *source, gui_rect rectangle, int patch);
int gui_surface_upload_ready(const gui_surface_upload *upload);
int gui_surface_upload_writable(const gui_surface_upload *upload);
int gui_surface_upload_write(gui_surface_upload *upload, gui_surface_upload_writer writer, void *context);
int gui_surface_upload_reply(gui_surface_upload *upload, const uint8_t *payload, size_t bytes);
int gui_surface_upload_data_error(gui_surface_upload *upload);
void gui_surface_upload_cancel(gui_surface_upload *upload, int send_cancel);
int gui_surface_upload_control(gui_surface_upload *upload, gui_protocol_type type,
	const void *payload, size_t bytes);

#endif
