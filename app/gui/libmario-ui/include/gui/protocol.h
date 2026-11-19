#ifndef GUI_PROTOCOL_H
#define GUI_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define GUI_PROTOCOL_VERSION_MAJOR 1U
#define GUI_PROTOCOL_VERSION_MINOR 1U
#define GUI_PROTOCOL_CAP_SURFACE_PATCH 0x00000001U
#define GUI_PROTOCOL_CAP_WINDOW_OPACITY 0x00000002U
#define GUI_PROTOCOL_HEADER_SIZE 8U
#define GUI_PROTOCOL_MIN_MESSAGE_SIZE GUI_PROTOCOL_HEADER_SIZE
#define GUI_PROTOCOL_MAX_MESSAGE_SIZE 65536U
#define GUI_PROTOCOL_OUTPUT_LIMIT 65536U
#define GUI_PROTOCOL_MAX_TITLE_BYTES 256U
#define GUI_PROTOCOL_MAX_PATH_BYTES 1024U

typedef enum {
	GUI_PROTOCOL_ENDPOINT_ORDINARY,
	GUI_PROTOCOL_ENDPOINT_SHELL,
} gui_protocol_endpoint_role;

typedef enum {
	GUI_PROTOCOL_HELLO = 1,
	GUI_PROTOCOL_HELLO_REPLY,
	GUI_PROTOCOL_ERROR,
	GUI_PROTOCOL_WINDOW_CREATE,
	GUI_PROTOCOL_WINDOW_CREATE_REPLY,
	GUI_PROTOCOL_WINDOW_DESTROY,
	GUI_PROTOCOL_DESKTOP_CREATE,
	GUI_PROTOCOL_DESKTOP_CREATE_REPLY,
	GUI_PROTOCOL_WINDOW_MOVE,
	GUI_PROTOCOL_WINDOW_RESIZE,
	GUI_PROTOCOL_WINDOW_CONFIGURE,
	GUI_PROTOCOL_WINDOW_SET_TITLE,
	GUI_PROTOCOL_WINDOW_CLOSE_REQUEST,
	GUI_PROTOCOL_SURFACE_BEGIN,
	GUI_PROTOCOL_SURFACE_DATA,
	GUI_PROTOCOL_SURFACE_DATA_REPLY,
	GUI_PROTOCOL_SURFACE_COMMIT,
	GUI_PROTOCOL_INPUT_EVENT,
	GUI_PROTOCOL_INPUT_RESET,
	GUI_PROTOCOL_WINDOW_FOCUS,
	GUI_PROTOCOL_WINDOW_RAISE,
	GUI_PROTOCOL_WINDOW_LIST_SNAPSHOT,
	GUI_PROTOCOL_WINDOW_LIST_ADD,
	GUI_PROTOCOL_WINDOW_LIST_CHANGE,
	GUI_PROTOCOL_WINDOW_LIST_REMOVE,
	GUI_PROTOCOL_SHELL_ACTIVATE_WINDOW,
	GUI_PROTOCOL_LAUNCH_REQUEST,
	GUI_PROTOCOL_LAUNCH_RESULT,
	GUI_PROTOCOL_APP_READY,
	GUI_PROTOCOL_APP_EXIT,
	GUI_PROTOCOL_CAPABILITIES,
	GUI_PROTOCOL_CAPABILITIES_REPLY,
	GUI_PROTOCOL_SURFACE_PATCH_BEGIN,
	GUI_PROTOCOL_SURFACE_CANCEL,
	GUI_PROTOCOL_WINDOW_STATE,
	GUI_PROTOCOL_SHELL_WORK_AREA,
	GUI_PROTOCOL_WINDOW_STATE_REQUEST,
	GUI_PROTOCOL_WINDOW_OPACITY,
} gui_protocol_type;

typedef enum {
	GUI_PROTOCOL_WINDOW_STATE_NORMAL,
	GUI_PROTOCOL_WINDOW_STATE_MINIMIZED,
	GUI_PROTOCOL_WINDOW_STATE_MAXIMIZED,
} gui_protocol_window_state;

typedef enum {
	GUI_PROTOCOL_LAUNCH_TERMINAL,
	GUI_PROTOCOL_LAUNCH_FILE_MANAGER,
	GUI_PROTOCOL_LAUNCH_IMAGE_VIEWER,
	GUI_PROTOCOL_LAUNCH_OPEN_TEXT,
	GUI_PROTOCOL_LAUNCH_EXECUTABLE,
} gui_protocol_launch_type;

typedef struct __attribute__((packed)) {
	uint32_t length_le;
	uint16_t type_le;
	uint16_t flags_le;
} gui_protocol_wire_header;

typedef struct __attribute__((packed)) {
	uint16_t major_le;
	uint16_t minor_le;
} gui_protocol_wire_hello;

typedef struct __attribute__((packed)) {
	uint32_t capabilities_le;
} gui_protocol_wire_capabilities;

typedef struct __attribute__((packed)) {
	int32_t status_le;
	uint16_t failed_type_le;
	uint16_t reserved_le;
} gui_protocol_wire_error;

typedef struct __attribute__((packed)) {
	uint32_t request_id_le;
	uint32_t width_le;
	uint32_t height_le;
	uint16_t title_bytes_le;
	uint16_t reserved_le;
} gui_protocol_wire_window_create;

typedef struct __attribute__((packed)) {
	uint32_t request_id_le;
	uint32_t window_id_le;
	uint64_t configure_serial_le;
	uint32_t width_le;
	uint32_t height_le;
} gui_protocol_wire_window_create_reply;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
} gui_protocol_wire_window_id;

typedef struct __attribute__((packed)) {
	uint32_t request_id_le;
} gui_protocol_wire_desktop_create;

typedef gui_protocol_wire_window_create_reply gui_protocol_wire_desktop_create_reply;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	int32_t x_le;
	int32_t y_le;
} gui_protocol_wire_window_move;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	uint32_t width_le;
	uint32_t height_le;
} gui_protocol_wire_window_resize;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	uint64_t configure_serial_le;
	uint32_t width_le;
	uint32_t height_le;
} gui_protocol_wire_window_configure;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	uint16_t title_bytes_le;
	uint16_t reserved_le;
} gui_protocol_wire_window_set_title;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	uint64_t configure_serial_le;
	uint64_t frame_serial_le;
	int32_t x_le;
	int32_t y_le;
	uint32_t width_le;
	uint32_t height_le;
} gui_protocol_wire_surface_begin;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	uint64_t configure_serial_le;
	uint64_t base_frame_serial_le;
	uint64_t frame_serial_le;
	int32_t x_le;
	int32_t y_le;
	uint32_t width_le;
	uint32_t height_le;
} gui_protocol_wire_surface_patch_begin;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	uint64_t configure_serial_le;
	uint64_t frame_serial_le;
	uint32_t pixel_offset_le;
	uint32_t pixel_count_le;
} gui_protocol_wire_surface_data;

typedef gui_protocol_wire_surface_data gui_protocol_wire_surface_data_reply;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	uint64_t configure_serial_le;
	uint64_t frame_serial_le;
} gui_protocol_wire_surface_commit;

typedef gui_protocol_wire_surface_commit gui_protocol_wire_surface_cancel;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	uint32_t event_type_le;
	uint32_t keysym_le;
	uint32_t modifiers_le;
	int32_t x_le;
	int32_t y_le;
	uint8_t buttons;
	uint8_t reserved[3];
	uint32_t ticks_le;
} gui_protocol_wire_input_event;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	uint32_t focused_le;
} gui_protocol_wire_window_focus;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	int32_t x_le;
	int32_t y_le;
	uint32_t width_le;
	uint32_t height_le;
	uint32_t focused_le;
	uint32_t order_le;
	uint16_t title_bytes_le;
	uint16_t reserved_le;
} gui_protocol_wire_window_list;

typedef struct __attribute__((packed)) {
	uint32_t request_id_le;
	uint16_t launch_type_le;
	uint16_t path_bytes_le;
} gui_protocol_wire_launch_request;

typedef struct __attribute__((packed)) {
	uint32_t request_id_le;
	int32_t status_le;
} gui_protocol_wire_launch_result;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	uint32_t state_le;
	uint64_t configure_serial_le;
} gui_protocol_wire_window_state;

typedef struct __attribute__((packed)) {
	int32_t x_le;
	int32_t y_le;
	uint32_t width_le;
	uint32_t height_le;
} gui_protocol_wire_shell_work_area;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	uint32_t state_le;
	uint32_t reserved_le;
} gui_protocol_wire_window_state_request;

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	uint32_t opacity_le;
} gui_protocol_wire_window_opacity;

_Static_assert(sizeof(gui_protocol_wire_window_opacity) == 8, "window opacity wire size");

typedef struct __attribute__((packed)) {
	uint32_t window_id_le;
	int32_t status_le;
} gui_protocol_wire_app_exit;

typedef struct {
	uint32_t length;
	gui_protocol_type type;
} gui_protocol_header;

typedef struct {
	uint16_t major;
	uint16_t minor;
} gui_protocol_hello;

typedef struct gui_protocol_input gui_protocol_input;
typedef struct gui_protocol_output gui_protocol_output;

typedef void (*gui_protocol_message_fn)(void *context,
					const gui_protocol_header *header,
					const void *payload, size_t payload_bytes);

typedef ssize_t (*gui_protocol_write_fn)(void *context, const void *bytes,
					 size_t byte_count);

typedef enum {
	GUI_PROTOCOL_OUTPUT_ERROR = -1,
	GUI_PROTOCOL_OUTPUT_EMPTY,
	GUI_PROTOCOL_OUTPUT_BLOCKED,
} gui_protocol_output_result;

_Static_assert(sizeof(gui_protocol_wire_header) == GUI_PROTOCOL_HEADER_SIZE,
	       "GUI protocol header size");
_Static_assert(sizeof(gui_protocol_wire_hello) == 4,
	       "GUI protocol hello size");
_Static_assert(sizeof(gui_protocol_wire_surface_begin) == 36,
	       "GUI protocol surface begin size");
_Static_assert(sizeof(gui_protocol_wire_surface_data) == 28,
	       "GUI protocol surface data size");
_Static_assert(sizeof(gui_protocol_wire_surface_commit) == 20,
	       "GUI protocol surface commit size");
_Static_assert(sizeof(gui_protocol_wire_window_state) == 16,
	       "GUI protocol window state size");
_Static_assert(sizeof(gui_protocol_wire_shell_work_area) == 16,
	       "GUI protocol shell work area size");
_Static_assert(sizeof(gui_protocol_wire_window_state_request) == 12,
	       "GUI protocol window state request size");

int gui_protocol_message_length(size_t payload_bytes, uint32_t *length);
int gui_protocol_encode_header(void *buffer, size_t buffer_bytes,
			       gui_protocol_type type, size_t payload_bytes);
int gui_protocol_decode_header(const void *buffer, size_t buffer_bytes,
			       gui_protocol_header *header);
int gui_protocol_encode_hello(void *buffer, size_t buffer_bytes,
			      const gui_protocol_hello *hello);
int gui_protocol_decode_hello(const void *buffer, size_t buffer_bytes,
			      gui_protocol_hello *hello);
gui_protocol_input *gui_protocol_input_create(void);
void gui_protocol_input_destroy(gui_protocol_input *input);
int gui_protocol_input_append(gui_protocol_input *input, const void *bytes,
				      size_t byte_count);
void gui_protocol_input_finish(gui_protocol_input *input);
int gui_protocol_input_pending(const gui_protocol_input *input);
int gui_protocol_input_drain(gui_protocol_input *input,
				     gui_protocol_message_fn callback, void *context);
gui_protocol_output *gui_protocol_output_create(size_t byte_limit);
void gui_protocol_output_destroy(gui_protocol_output *output);
int gui_protocol_output_enqueue(gui_protocol_output *output,
				const void *message, size_t byte_count);
int gui_protocol_output_enqueue_motion(gui_protocol_output *output,
				       const void *message, size_t byte_count);
int gui_protocol_output_pending(const gui_protocol_output *output);
gui_protocol_output_result gui_protocol_output_flush(
	gui_protocol_output *output, gui_protocol_write_fn writer, void *context);

#endif
