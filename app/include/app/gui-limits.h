#ifndef APP_GUI_LIMITS_H
#define APP_GUI_LIMITS_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <gui/protocol.h>

#define APP_GUI_SESSION_SURFACE_FRAMES 16U
#define APP_GUI_SESSION_SURFACE_BYTES (64U * 1024U * 1024U)
#define APP_GUI_PEER_SURFACE_FRAMES 8U
#define APP_GUI_PEER_SURFACE_BYTES (32U * 1024U * 1024U)
#define APP_GUI_WM_RECORD_BYTES (1024U * 1024U)
#define APP_GUI_PEER_RECORD_BYTES (256U * 1024U)
#define APP_GUI_DESKTOP_RECORD_BYTES (1024U * 1024U)
#define APP_GUI_SURFACE_RETRY_MS 100

typedef struct {
	int retry_pending;
	uint64_t retry_at_ms;
	int discard_reply;
	uint64_t discarded_configure_serial;
	uint64_t discarded_frame_serial;
	uint32_t discarded_offset;
	uint32_t discarded_count;
} app_gui_surface_flow;

static inline size_t app_gui_surface_budget(size_t frame_bytes)
{
	return frame_bytes > APP_GUI_SESSION_SURFACE_BYTES /
		APP_GUI_SESSION_SURFACE_FRAMES ? APP_GUI_SESSION_SURFACE_BYTES :
		frame_bytes * APP_GUI_SESSION_SURFACE_FRAMES;
}

static inline size_t app_gui_peer_surface_budget(size_t frame_bytes)
{
	return frame_bytes > APP_GUI_PEER_SURFACE_BYTES /
		APP_GUI_PEER_SURFACE_FRAMES ? APP_GUI_PEER_SURFACE_BYTES :
		frame_bytes * APP_GUI_PEER_SURFACE_FRAMES;
}

static inline uint32_t app_gui_reply_u32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
		(uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static inline uint64_t app_gui_reply_u64(const uint8_t *bytes)
{
	return (uint64_t)app_gui_reply_u32(bytes) |
		(uint64_t)app_gui_reply_u32(bytes + 4) << 32;
}

static inline void app_gui_surface_defer(app_gui_surface_flow *flow)
{
	struct timespec now;

	if (flow->retry_pending)
		return;
	flow->retry_pending = 1;
	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
		flow->retry_at_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 +
			APP_GUI_SURFACE_RETRY_MS;
}

static inline int app_gui_surface_retry_ready(app_gui_surface_flow *flow)
{
	struct timespec now;
	uint64_t now_ms;

	if (!flow->retry_pending || flow->discard_reply ||
	    clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	now_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
	if (now_ms < flow->retry_at_ms)
		return 0;
	flow->retry_pending = 0;
	return 1;
}

static inline void app_gui_surface_discard_reply(app_gui_surface_flow *flow,
		uint64_t configure_serial, uint64_t frame_serial,
		uint32_t offset, uint32_t count)
{
	flow->discard_reply = 1;
	flow->discarded_configure_serial = configure_serial;
	flow->discarded_frame_serial = frame_serial;
	flow->discarded_offset = offset;
	flow->discarded_count = count;
}

static inline int app_gui_surface_flow_message(app_gui_surface_flow *flow,
		uint32_t window_id, gui_protocol_type type, const uint8_t *payload,
		size_t payload_bytes)
{
	if (type == GUI_PROTOCOL_SURFACE_DATA_REPLY && flow->discard_reply) {
		if (payload_bytes != sizeof(gui_protocol_wire_surface_data_reply) ||
		    app_gui_reply_u32(payload) != window_id ||
		    app_gui_reply_u64(payload + 4) != flow->discarded_configure_serial ||
		    app_gui_reply_u64(payload + 12) != flow->discarded_frame_serial ||
		    app_gui_reply_u32(payload + 20) != flow->discarded_offset ||
		    app_gui_reply_u32(payload + 24) != flow->discarded_count)
			return -1;
		flow->discard_reply = 0;
		return 1;
	}
	if (type == GUI_PROTOCOL_ERROR) {
		int32_t status;
		uint16_t failed_type;

		if (payload_bytes != sizeof(gui_protocol_wire_error))
			return -1;
		status = (int32_t)app_gui_reply_u32(payload);
		failed_type = (uint16_t)payload[4] | (uint16_t)payload[5] << 8;
		if ((failed_type == GUI_PROTOCOL_SURFACE_BEGIN &&
		     (status == -ENOMEM || status == -EIO)) ||
		    (failed_type == GUI_PROTOCOL_SURFACE_COMMIT && status == -EIO)) {
			app_gui_surface_defer(flow);
			return 1;
		}
		if (failed_type == GUI_PROTOCOL_WINDOW_RESIZE &&
		    (status == -ENOMEM || status == -EOVERFLOW || status == -EIO)) {
			if (status == -EIO)
				app_gui_surface_defer(flow);
			return 1;
		}
		if (status == -EINVAL && flow->discard_reply &&
		    (failed_type == GUI_PROTOCOL_SURFACE_BEGIN ||
		     failed_type == GUI_PROTOCOL_SURFACE_DATA)) {
			if (failed_type == GUI_PROTOCOL_SURFACE_DATA)
				flow->discard_reply = 0;
			return 1;
		}
		return -1;
	}
	return 0;
}

#endif
