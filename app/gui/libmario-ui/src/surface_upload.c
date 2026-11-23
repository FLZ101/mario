#include <errno.h>
#include <limits.h>
#include <string.h>

#include <gui/surface_upload.h>

static void upload_put32(uint8_t *bytes, uint32_t value)
{
	for (unsigned int index = 0; index < 4; index++)
		bytes[index] = value >> (index * 8);
}

static void upload_put64(uint8_t *bytes, uint64_t value)
{
	upload_put32(bytes, value);
	upload_put32(bytes + 4, value >> 32);
}

static uint32_t upload_get32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
		(uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint64_t upload_get64(const uint8_t *bytes)
{
	return upload_get32(bytes) | (uint64_t)upload_get32(bytes + 4) << 32;
}

int gui_surface_upload_ready(const gui_surface_upload *upload)
{
	return !upload->active && upload->chunk_count == 0 && upload->frame_bytes == 0;
}

int gui_surface_upload_start(gui_surface_upload *upload, uint32_t window, uint64_t configure,
	uint64_t base, uint64_t serial, const gui_surface *source, gui_rect rectangle, int patch)
{
	if (!gui_surface_upload_ready(upload) || source == NULL || source->pixels == NULL ||
	    source->width <= 0 || source->height <= 0 || source->stride < source->width ||
	    rectangle.x < 0 || rectangle.y < 0 || rectangle.w <= 0 || rectangle.h <= 0 ||
	    rectangle.x > source->width || rectangle.y > source->height ||
	    rectangle.w > source->width - rectangle.x || rectangle.h > source->height - rectangle.y ||
	    (!patch && (rectangle.x != 0 || rectangle.y != 0 || rectangle.w != source->width || rectangle.h != source->height)) ||
	    (uint32_t)rectangle.w > UINT32_MAX / (uint32_t)rectangle.h ||
	    (size_t)source->height > SIZE_MAX / sizeof(gui_color) / (size_t)source->stride ||
	    window == 0 || configure == 0 || serial == 0 || (patch && base == 0))
		return -1;
	upload->active = 1;
	upload->begun = 0;
	upload->canceled = 0;
	upload->send_cancel = 0;
	upload->completed = 0;
	upload->patch = patch;
	upload->window = window;
	upload->configure = configure;
	upload->base = base;
	upload->serial = serial;
	upload->source = *source;
	upload->rectangle = rectangle;
	upload->total = (uint32_t)rectangle.w * rectangle.h;
	upload->next_pixel = 0;
	return 0;
}

int gui_surface_upload_control(gui_surface_upload *upload, gui_protocol_type type,
	const void *payload, size_t bytes)
{
	if (upload->frame_bytes != 0 || bytes > sizeof(upload->frame) - GUI_PROTOCOL_HEADER_SIZE ||
	    (payload == NULL && bytes != 0) ||
	    gui_protocol_encode_header(upload->frame, sizeof(upload->frame), type, bytes) < 0)
		return -1;
	if (bytes != 0)
		memcpy(upload->frame + GUI_PROTOCOL_HEADER_SIZE, payload, bytes);
	upload->frame_type = type;
	upload->frame_written = 0;
	upload->frame_bytes = GUI_PROTOCOL_HEADER_SIZE + bytes;
	return 0;
}

int gui_surface_upload_writable(const gui_surface_upload *upload)
{
	return upload->frame_bytes != 0 || (upload->active &&
		(upload->canceled || !upload->begun ||
		 (upload->next_pixel < upload->total && upload->chunk_count < GUI_SURFACE_UPLOAD_CHUNKS) ||
		 (upload->next_pixel == upload->total && upload->chunk_count == 0)));
}

static int upload_prepare(gui_surface_upload *upload)
{
	uint8_t payload[sizeof(gui_protocol_wire_surface_patch_begin)] = { 0 };

	if (upload->frame_bytes != 0)
		return 1;
	if (!upload->active)
		return 0;
	upload_put32(payload, upload->window);
	upload_put64(payload + 4, upload->configure);
	upload_put64(payload + 12, upload->serial);
	if (upload->canceled) {
		if (upload->send_cancel && upload->begun) {
			upload->send_cancel = 0;
			return gui_surface_upload_control(upload, GUI_PROTOCOL_SURFACE_CANCEL, payload,
				sizeof(gui_protocol_wire_surface_cancel)) < 0 ? -1 : 1;
		}
		upload->active = 0;
		return 0;
	}
	if (!upload->begun) {
		if (upload->patch) {
			upload_put64(payload + 12, upload->base);
			upload_put64(payload + 20, upload->serial);
			upload_put32(payload + 28, upload->rectangle.x);
			upload_put32(payload + 32, upload->rectangle.y);
			upload_put32(payload + 36, upload->rectangle.w);
			upload_put32(payload + 40, upload->rectangle.h);
			return gui_surface_upload_control(upload, GUI_PROTOCOL_SURFACE_PATCH_BEGIN, payload,
				sizeof(gui_protocol_wire_surface_patch_begin)) < 0 ? -1 : 1;
		}
		upload_put32(payload + 28, upload->rectangle.w);
		upload_put32(payload + 32, upload->rectangle.h);
		return gui_surface_upload_control(upload, GUI_PROTOCOL_SURFACE_BEGIN, payload,
			sizeof(gui_protocol_wire_surface_begin)) < 0 ? -1 : 1;
	}
	if (upload->next_pixel < upload->total) {
		uint32_t count = upload->total - upload->next_pixel;
		size_t bytes;
		gui_surface_upload_chunk *chunk;

		if (upload->chunk_count == GUI_SURFACE_UPLOAD_CHUNKS)
			return 0;
		if (count > GUI_SURFACE_UPLOAD_PIXELS)
			count = GUI_SURFACE_UPLOAD_PIXELS;
		bytes = GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_data) + count * sizeof(gui_color);
		if (bytes > GUI_SURFACE_UPLOAD_WINDOW_BYTES - upload->outstanding_bytes)
			return 0;
		if (gui_protocol_encode_header(upload->frame, sizeof(upload->frame), GUI_PROTOCOL_SURFACE_DATA,
			bytes - GUI_PROTOCOL_HEADER_SIZE) < 0)
			return -1;
		memcpy(upload->frame + GUI_PROTOCOL_HEADER_SIZE, payload, 20);
		upload_put32(upload->frame + GUI_PROTOCOL_HEADER_SIZE + 20, upload->next_pixel);
		upload_put32(upload->frame + GUI_PROTOCOL_HEADER_SIZE + 24, count);
		for (uint32_t index = 0; index < count; index++) {
			uint32_t offset = upload->next_pixel + index;
			size_t row = upload->rectangle.y + offset / upload->rectangle.w;
			size_t column = upload->rectangle.x + offset % upload->rectangle.w;

			upload_put32(upload->frame + GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_data) +
				index * sizeof(gui_color), upload->source.pixels[row * upload->source.stride + column]);
		}
		chunk = &upload->chunks[(upload->chunk_head + upload->chunk_count) % GUI_SURFACE_UPLOAD_CHUNKS];
		*chunk = (gui_surface_upload_chunk) { upload->next_pixel, count, bytes };
		upload->chunk_count++;
		upload->outstanding_bytes += bytes;
		if (upload->peak_outstanding_bytes < upload->outstanding_bytes)
			upload->peak_outstanding_bytes = upload->outstanding_bytes;
		upload->next_pixel += count;
		upload->frame_written = 0;
		upload->frame_bytes = bytes;
		upload->frame_type = GUI_PROTOCOL_SURFACE_DATA;
		return 1;
	}
	if (upload->chunk_count != 0)
		return 0;
	return gui_surface_upload_control(upload, GUI_PROTOCOL_SURFACE_COMMIT, payload,
		sizeof(gui_protocol_wire_surface_commit)) < 0 ? -1 : 1;
}

int gui_surface_upload_write(gui_surface_upload *upload, gui_surface_upload_writer writer, void *context)
{
	if (writer == NULL)
		return -1;
	for (unsigned int attempt = 0; attempt < 4; attempt++) {
		int ready = upload_prepare(upload);
		ssize_t count;

		if (ready <= 0)
			return ready;
		count = writer(context, upload->frame + upload->frame_written,
			upload->frame_bytes - upload->frame_written);
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			return 0;
		if (count <= 0 || (size_t)count > upload->frame_bytes - upload->frame_written)
			return -1;
		upload->frame_written += count;
		if (upload->frame_written != upload->frame_bytes)
			continue;
		if (upload->frame_type == GUI_PROTOCOL_SURFACE_BEGIN || upload->frame_type == GUI_PROTOCOL_SURFACE_PATCH_BEGIN)
			upload->begun = 1;
		if (upload->frame_type == GUI_PROTOCOL_SURFACE_DATA) {
			unsigned int tail = (upload->chunk_head + upload->chunk_count - 1) % GUI_SURFACE_UPLOAD_CHUNKS;

			upload->chunks[tail].sent = 1;
		}
		if (upload->frame_type == GUI_PROTOCOL_SURFACE_COMMIT) {
			upload->active = 0;
			if (!upload->canceled) {
				upload->completed = 1;
				upload->completed_serial = upload->serial;
				upload->completed_configure = upload->configure;
			}
		}
		if (upload->frame_type == GUI_PROTOCOL_SURFACE_CANCEL)
			upload->active = 0;
		upload->frame_bytes = upload->frame_written = 0;
	}
	return 0;
}

static void upload_retire_chunk(gui_surface_upload *upload)
{
	upload->outstanding_bytes -= upload->chunks[upload->chunk_head].bytes;
	upload->chunk_head = (upload->chunk_head + 1) % GUI_SURFACE_UPLOAD_CHUNKS;
	upload->chunk_count--;
}

int gui_surface_upload_reply(gui_surface_upload *upload, const uint8_t *payload, size_t bytes)
{
	uint64_t configure;
	uint64_t serial;
	uint32_t offset;
	uint32_t count;
	gui_surface_upload_chunk *chunk;

	if (payload == NULL || bytes != sizeof(gui_protocol_wire_surface_data_reply) || upload_get32(payload) != upload->window)
		return -1;
	configure = upload_get64(payload + 4);
	serial = upload_get64(payload + 12);
	offset = upload_get32(payload + 20);
	count = upload_get32(payload + 24);
	if (configure == 0 || serial == 0 || count == 0 || count > GUI_SURFACE_UPLOAD_PIXELS || offset > UINT32_MAX - count)
		return -1;
	if (configure < upload->configure || (configure == upload->configure && serial < upload->serial))
		return 0;
	if (configure != upload->configure || serial != upload->serial || count == 0)
		return -1;
	if (upload->chunk_count == 0)
		return !upload->active && offset <= upload->total && count <= upload->total - offset ? 0 : -1;
	chunk = &upload->chunks[upload->chunk_head];
	if (offset < chunk->offset && count <= chunk->offset - offset)
		return 0;
	if (offset != chunk->offset || count != chunk->count || !chunk->sent)
		return -1;
	upload_retire_chunk(upload);
	return 0;
}

int gui_surface_upload_data_error(gui_surface_upload *upload)
{
	if (upload->chunk_count == 0 || !upload->chunks[upload->chunk_head].sent)
		return -1;
	upload_retire_chunk(upload);
	return 0;
}

void gui_surface_upload_cancel(gui_surface_upload *upload, int send_cancel)
{
	if (!upload->active)
		return;
	upload->canceled = 1;
	upload->send_cancel = send_cancel;
	upload->source.pixels = NULL;
	if (upload->frame_bytes != 0 && upload->frame_written == 0) {
		if (upload->frame_type == GUI_PROTOCOL_SURFACE_DATA) {
			unsigned int tail = (upload->chunk_head + upload->chunk_count - 1) % GUI_SURFACE_UPLOAD_CHUNKS;

			upload->outstanding_bytes -= upload->chunks[tail].bytes;
			upload->chunk_count--;
		}
		if (upload->frame_type == GUI_PROTOCOL_SURFACE_DATA || upload->frame_type == GUI_PROTOCOL_SURFACE_BEGIN ||
		    upload->frame_type == GUI_PROTOCOL_SURFACE_PATCH_BEGIN || upload->frame_type == GUI_PROTOCOL_SURFACE_COMMIT)
			upload->frame_bytes = 0;
	}
}
