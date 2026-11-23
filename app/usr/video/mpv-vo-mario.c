#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <app/gui-limits.h>
#include <gui/input.h>
#include <gui/surface_upload.h>

#include "common/msg.h"
#include "input/input.h"
#include "input/keycodes.h"
#include "osdep/io.h"
#include "osdep/poll_wrapper.h"
#include "osdep/timer.h"
#include "video/sws_utils.h"
#include "vo.h"

#define WM_SOCKET_PATH "/tmp/mario-wm.sock"
#define MARIO_MAX_SURFACE_PIXELS (APP_GUI_PEER_SURFACE_BYTES / 2 / sizeof(gui_color))
#define MARIO_UPLOAD_TIMEOUT MP_TIME_S_TO_NS(10)

struct priv {
    int fd;
    int wakeup_pipe[2];
    bool stopped;
    bool dirty;
    bool can_drop;
    uint32_t window;
    uint64_t configure;
    uint64_t serial;
    uint64_t frame_id;
    int64_t upload_start;
    int64_t conversion_end;
    int width, height;
    gui_protocol_type expected;
    gui_protocol_input *input;
    uint8_t rx[4096];
    gui_surface_upload upload;
    app_gui_surface_flow flow;
    gui_surface *surface;
    struct mp_sws_context *sws;
    struct mp_image *next;
    struct mp_image *latest;
};

static void stop(struct vo *vo)
{
    struct priv *priv = vo->priv;
    priv->stopped = true;
    if (priv->fd >= 0) {
        close(priv->fd);
        priv->fd = -1;
    }
    if (priv->window)
        mp_input_put_key(vo->input_ctx, MP_KEY_CLOSE_WIN);
}

static bool configure(struct vo *vo, uint64_t serial, uint32_t width, uint32_t height)
{
    struct priv *priv = vo->priv;
    if (serial <= priv->configure || !width || !height ||
        width > MARIO_MAX_SURFACE_PIXELS / height)
        return false;
    gui_surface_upload_cancel(&priv->upload, false);
    priv->flow = (app_gui_surface_flow){0};
    priv->configure = serial;
    priv->width = vo->dwidth = width;
    priv->height = vo->dheight = height;
    priv->dirty = priv->latest != NULL;
    vo_event(vo, VO_EVENT_RESIZE);
    return true;
}

static bool handshake(struct vo *vo, gui_protocol_type type,
                      const void *payload, size_t bytes)
{
    struct priv *priv = vo->priv;
    if (type != priv->expected)
        return false;
    if (type == GUI_PROTOCOL_HELLO_REPLY) {
        gui_protocol_wire_hello hello;
        if (bytes != sizeof(hello))
            return false;
        memcpy(&hello, payload, bytes);
        uint32_t capabilities = GUI_PROTOCOL_CAP_SURFACE_PATCH;
        priv->expected = GUI_PROTOCOL_CAPABILITIES_REPLY;
        return hello.major_le == GUI_PROTOCOL_VERSION_MAJOR && hello.minor_le >= 1 &&
            gui_surface_upload_control(&priv->upload, GUI_PROTOCOL_CAPABILITIES,
                                       &capabilities, sizeof(capabilities)) == 0;
    }
    if (type == GUI_PROTOCOL_CAPABILITIES_REPLY) {
        if (bytes != sizeof(gui_protocol_wire_capabilities) ||
            !(app_gui_reply_u32(payload) & GUI_PROTOCOL_CAP_SURFACE_PATCH))
            return false;
        struct __attribute__((packed)) {
            gui_protocol_wire_window_create create;
            char title[3];
        } message = {{.request_id_le = 1, .width_le = 320, .height_le = 180,
                      .title_bytes_le = 3}, {'m', 'priv', 'v'}};
        priv->expected = GUI_PROTOCOL_WINDOW_CREATE_REPLY;
        return gui_surface_upload_control(&priv->upload, GUI_PROTOCOL_WINDOW_CREATE,
                                          &message, sizeof(message)) == 0;
    }
    gui_protocol_wire_window_create_reply reply;
    if (bytes != sizeof(reply))
        return false;
    memcpy(&reply, payload, bytes);
    if (reply.request_id_le != 1 || !reply.window_id_le ||
        !configure(vo, reply.configure_serial_le, reply.width_le, reply.height_le))
        return false;
    priv->window = reply.window_id_le;
    return true;
}

static void handle_key(struct vo *vo, const gui_protocol_wire_input_event *event)
{
    static const int special[] = {
        [GUI_KEYSYM_ESCAPE - GUI_KEYSYM_SPECIAL_BASE] = MP_KEY_ESC,
        [GUI_KEYSYM_BACKSPACE - GUI_KEYSYM_SPECIAL_BASE] = MP_KEY_BS,
        [GUI_KEYSYM_LEFT - GUI_KEYSYM_SPECIAL_BASE] = MP_KEY_LEFT,
        [GUI_KEYSYM_RIGHT - GUI_KEYSYM_SPECIAL_BASE] = MP_KEY_RIGHT,
        [GUI_KEYSYM_UP - GUI_KEYSYM_SPECIAL_BASE] = MP_KEY_UP,
        [GUI_KEYSYM_DOWN - GUI_KEYSYM_SPECIAL_BASE] = MP_KEY_DOWN,
        [GUI_KEYSYM_HOME - GUI_KEYSYM_SPECIAL_BASE] = MP_KEY_HOME,
        [GUI_KEYSYM_END - GUI_KEYSYM_SPECIAL_BASE] = MP_KEY_END,
        [GUI_KEYSYM_PAGE_UP - GUI_KEYSYM_SPECIAL_BASE] = MP_KEY_PGUP,
        [GUI_KEYSYM_PAGE_DOWN - GUI_KEYSYM_SPECIAL_BASE] = MP_KEY_PGDOWN,
        [GUI_KEYSYM_INSERT - GUI_KEYSYM_SPECIAL_BASE] = MP_KEY_INS,
        [GUI_KEYSYM_DELETE - GUI_KEYSYM_SPECIAL_BASE] = MP_KEY_DEL,
    };
    uint32_t keysym = event->keysym_le;
    uint32_t modifiers = event->modifiers_le;
    int key = keysym < GUI_KEYSYM_SPECIAL_BASE ? keysym :
        keysym - GUI_KEYSYM_SPECIAL_BASE < MP_ARRAY_SIZE(special) ?
        special[keysym - GUI_KEYSYM_SPECIAL_BASE] : 0;
    if (!key)
        return;
    if (modifiers & GUI_MOD_SHIFT && (keysym <= ' ' || keysym >= GUI_KEYSYM_SPECIAL_BASE))
        key |= MP_KEY_MODIFIER_SHIFT;
    if (modifiers & GUI_MOD_CTRL)
        key |= MP_KEY_MODIFIER_CTRL;
    if (modifiers & GUI_MOD_ALT)
        key |= MP_KEY_MODIFIER_ALT;
    if (modifiers & GUI_MOD_SUPER)
        key |= MP_KEY_MODIFIER_META;
    mp_input_put_key(vo->input_ctx, key);
}

static bool surface_error(struct vo *vo, const void *payload, size_t bytes)
{
    struct priv *priv = vo->priv;
    gui_protocol_wire_error error;
    if (bytes != sizeof(error))
        return false;
    memcpy(&error, payload, bytes);
    switch (error.failed_type_le) {
    case GUI_PROTOCOL_WINDOW_RESIZE:
    case GUI_PROTOCOL_SURFACE_CANCEL:
        return error.status_le == -EINVAL || error.status_le == -ENOMEM ||
               error.status_le == -EOVERFLOW || error.status_le == -EIO;
    case GUI_PROTOCOL_SURFACE_DATA:
        if (gui_surface_upload_data_error(&priv->upload) < 0)
            return false;
        break;
    case GUI_PROTOCOL_SURFACE_BEGIN:
    case GUI_PROTOCOL_SURFACE_COMMIT:
        break;
    default:
        return false;
    }
    if (error.status_le != -ENOMEM && error.status_le != -EBUSY &&
        error.status_le != -EINVAL && error.status_le != -ESTALE && error.status_le != -EIO)
        return false;
    if (!priv->upload.canceled || error.failed_type_le == GUI_PROTOCOL_SURFACE_COMMIT) {
        gui_surface_upload_cancel(&priv->upload, true);
        priv->dirty = priv->latest != NULL;
        app_gui_surface_defer(&priv->flow);
        MP_DBG(vo, "Deferred surface error %u: %d\n", error.failed_type_le, error.status_le);
    }
    return true;
}

static bool handle_message(struct vo *vo, gui_protocol_type type,
                           const void *payload, size_t bytes)
{
    struct priv *priv = vo->priv;
    if (!priv->window)
        return handshake(vo, type, payload, bytes);
    if (type == GUI_PROTOCOL_ERROR)
        return surface_error(vo, payload, bytes);
    if (type == GUI_PROTOCOL_INPUT_RESET)
        return bytes == 0;
    if (bytes < sizeof(uint32_t) || app_gui_reply_u32(payload) != priv->window)
        return false;
    switch (type) {
    case GUI_PROTOCOL_SURFACE_DATA_REPLY:
        return gui_surface_upload_reply(&priv->upload, payload, bytes) == 0;
    case GUI_PROTOCOL_WINDOW_CONFIGURE: {
        gui_protocol_wire_window_configure event;
        if (bytes != sizeof(event))
            return false;
        memcpy(&event, payload, bytes);
        return configure(vo, event.configure_serial_le, event.width_le, event.height_le);
    }
    case GUI_PROTOCOL_WINDOW_CLOSE_REQUEST:
        if (bytes != sizeof(gui_protocol_wire_window_id))
            return false;
        stop(vo);
        return true;
    case GUI_PROTOCOL_INPUT_EVENT: {
        gui_protocol_wire_input_event event;
        if (bytes != sizeof(event))
            return false;
        memcpy(&event, payload, bytes);
        if (event.event_type_le == GUI_INPUT_EVENT_KEY)
            handle_key(vo, &event);
        return true;
    }
    case GUI_PROTOCOL_WINDOW_FOCUS:
        return bytes == sizeof(gui_protocol_wire_window_focus);
    case GUI_PROTOCOL_WINDOW_STATE:
        return bytes == sizeof(gui_protocol_wire_window_state);
    default:
        return false;
    }
}

static void receive_message(void *context, const gui_protocol_header *header,
                            const void *payload, size_t bytes)
{
    struct vo *vo = context;
    struct priv *priv = vo->priv;
    if (!priv->stopped && !handle_message(vo, header->type, payload, bytes)) {
        MP_ERR(vo, "Unexpected window manager message %u\n", header->type);
        stop(vo);
    }
}

static ssize_t socket_write(void *context, const void *bytes, size_t count)
{
    return write(*(int *)context, bytes, count);
}

static bool start_upload(struct vo *vo)
{
    struct priv *priv = vo->priv;
    if (!priv->dirty || !gui_surface_upload_ready(&priv->upload) ||
        (priv->flow.retry_pending && !app_gui_surface_retry_ready(&priv->flow)))
        return true;
    priv->upload_start = mp_time_ns();
    if (!priv->surface || priv->surface->width != priv->width || priv->surface->height != priv->height) {
        gui_surface_destroy(priv->surface);
        priv->surface = gui_surface_create(priv->width, priv->height);
        if (!priv->surface)
            return false;
    }
    int display_width, display_height;
    mp_image_params_get_dsize(&priv->latest->params, &display_width, &display_height);
    if (display_width <= 0 || display_height <= 0)
        return false;
    int width = priv->width;
    int height = priv->height;
    if ((int64_t)width * display_height > (int64_t)height * display_width)
        width = MPMAX(1, (int64_t)height * display_width / display_height);
    else
        height = MPMAX(1, (int64_t)width * display_height / display_width);
    int left = (priv->width - width) / 2;
    int top = (priv->height - height) / 2;
    gui_rect full = {0, 0, priv->width, priv->height};
    if (width != priv->width || height != priv->height)
        gui_fill_rect(priv->surface, full, 0);
    struct mp_image target = {0};
    mp_image_setfmt(&target, IMGFMT_BGR0);
    mp_image_set_size(&target, width, height);
    target.stride[0] = priv->surface->stride * sizeof(gui_color);
    target.planes[0] = (uint8_t *)(priv->surface->pixels + top * priv->surface->stride + left);
    if (mp_sws_scale(priv->sws, &target, priv->latest) < 0)
        return false;
    priv->conversion_end = mp_time_ns();
    priv->dirty = false;
    return gui_surface_upload_start(&priv->upload, priv->window, priv->configure, 0,
                                    ++priv->serial, priv->surface, full, false) == 0;
}

static void pump_events(struct vo *vo, int64_t deadline, bool interruptible)
{
    struct priv *priv = vo->priv;
    int64_t timeout = MPCLAMP(deadline - mp_time_ns(), 0, MP_TIME_S_TO_NS(10));
    if (priv->flow.retry_pending)
        timeout = MPMIN(timeout, MP_TIME_MS_TO_NS(APP_GUI_SURFACE_RETRY_MS));
    if (!priv->stopped && !start_upload(vo)) {
        MP_ERR(vo, "Failed to render the video surface\n");
        stop(vo);
    }
    struct pollfd fds[] = {
        {.fd = priv->fd, .events = POLLIN | (gui_surface_upload_writable(&priv->upload) ? POLLOUT : 0)},
        {.fd = interruptible ? priv->wakeup_pipe[0] : -1, .events = POLLIN},
    };
    int result = mp_poll(fds, MP_ARRAY_SIZE(fds), timeout);
    if (result < 0 && errno == EINTR)
        return;
    if (fds[1].revents & POLLIN)
        mp_flush_wakeup_pipe(priv->wakeup_pipe[0]);
    if (priv->stopped)
        return;
    if (result < 0 || (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
        goto failed;
    if (fds[0].revents & POLLIN) {
        ssize_t count = read(priv->fd, priv->rx, sizeof(priv->rx));
        if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR))
            goto failed;
        if (count > 0) {
            if (gui_protocol_input_append(priv->input, priv->rx, count) < 0)
                goto failed;
            do {
                result = gui_protocol_input_drain(priv->input, receive_message, vo);
                if (result < 0)
                    goto failed;
            } while (result && !priv->stopped);
        }
    }
    if (!priv->stopped && gui_surface_upload_writable(&priv->upload)) {
        bool completed = priv->upload.completed;
        if (gui_surface_upload_write(&priv->upload, socket_write, &priv->fd) < 0)
            goto failed;
        if (!completed && priv->upload.completed)
            MP_TRACE(vo, "Submitted surface %" PRIu64 " in %.1f ms (conversion %.1f ms)\n",
                     priv->upload.completed_serial, (mp_time_ns() - priv->upload_start) / 1e6,
                     (priv->conversion_end - priv->upload_start) / 1e6);
    }
    return;

failed:
    MP_ERR(vo, "Lost connection to the window manager\n");
    stop(vo);
}

static void wait_events(struct vo *vo, int64_t deadline)
{
    pump_events(vo, deadline, true);
}

static bool drain_upload(struct vo *vo)
{
    struct priv *priv = vo->priv;
    int64_t deadline = mp_time_ns() + MARIO_UPLOAD_TIMEOUT;
    while (!priv->stopped && (priv->dirty || !gui_surface_upload_ready(&priv->upload))) {
        if (mp_time_ns() >= deadline) {
            MP_ERR(vo, "Timed out uploading a video frame\n");
            stop(vo);
            break;
        }
        pump_events(vo, deadline, false);
    }
    return !priv->stopped;
}

static void wakeup(struct vo *vo)
{
    struct priv *priv = vo->priv;
    if (priv->wakeup_pipe[1] >= 0)
        (void)write(priv->wakeup_pipe[1], &(char){0}, 1);
}

static void uninit(struct vo *vo)
{
    struct priv *priv = vo->priv;
    if (priv->fd >= 0)
        close(priv->fd);
    if (priv->wakeup_pipe[0] >= 0)
        close(priv->wakeup_pipe[0]);
    if (priv->wakeup_pipe[1] >= 0)
        close(priv->wakeup_pipe[1]);
    gui_protocol_input_destroy(priv->input);
    gui_surface_destroy(priv->surface);
    talloc_free(priv->latest);
    talloc_free(priv->next);
}

static int preinit(struct vo *vo)
{
    struct priv *priv = vo->priv;
    struct sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = WM_SOCKET_PATH};
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &blocked, NULL) != 0)
        goto failed;
    priv->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (priv->fd < 0 || connect(priv->fd, (const struct sockaddr *)&address,
                            offsetof(struct sockaddr_un, sun_path) + sizeof(WM_SOCKET_PATH)) < 0 ||
        mp_make_wakeup_pipe(priv->wakeup_pipe) < 0)
        goto failed;
    int send_buffer = GUI_SURFACE_UPLOAD_BATCH_BYTES;
    if (setsockopt(priv->fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0)
        priv->upload.batch = true;
    priv->input = gui_protocol_input_create();
    priv->sws = mp_sws_alloc(priv);
    priv->sws->log = vo->log;
    const gui_protocol_wire_hello hello = {GUI_PROTOCOL_VERSION_MAJOR, GUI_PROTOCOL_VERSION_MINOR};
    priv->expected = GUI_PROTOCOL_HELLO_REPLY;
    if (!priv->input || gui_surface_upload_control(&priv->upload, GUI_PROTOCOL_HELLO,
                                                &hello, sizeof(hello)) < 0)
        goto failed;
    int64_t deadline = mp_time_ns() + MP_TIME_S_TO_NS(3);
    while (!priv->stopped && !priv->window && mp_time_ns() < deadline)
        wait_events(vo, deadline);
    if (priv->window && !priv->stopped)
        return 0;

failed:
    MP_ERR(vo, "Could not create a window on " WM_SOCKET_PATH "\n");
    uninit(vo);
    return -1;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *priv = vo->priv;
    vo->dwidth = priv->width;
    vo->dheight = priv->height;
    return priv->stopped ? -1 : 0;
}

static int query_format(struct vo *vo, int format)
{
    struct priv *priv = vo->priv;
    return mp_sws_supports_formats(priv->sws, IMGFMT_BGR0, format);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *priv = vo->priv;
    if (!priv->stopped && frame->current && frame->frame_id != priv->frame_id) {
        priv->next = mp_image_new_ref(frame->current);
        priv->frame_id = frame->frame_id;
        priv->can_drop = frame->can_drop;
    }
    return !priv->stopped;
}

static void flip_page(struct vo *vo)
{
    struct priv *priv = vo->priv;
    if (priv->next) {
        if (priv->dirty) {
            if (priv->can_drop)
                vo_increment_drop_count(vo, 1);
            else if (!drain_upload(vo))
                return;
        }
        talloc_free(priv->latest);
        priv->latest = priv->next;
        priv->next = NULL;
        priv->dirty = true;
    }
    if (priv->can_drop)
        wait_events(vo, 0);
    else
        drain_upload(vo);
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *priv = vo->priv;
    switch (request) {
    case VOCTRL_CHECK_EVENTS:
        wait_events(vo, 0);
        return VO_TRUE;
    case VOCTRL_RESET:
        gui_surface_upload_cancel(&priv->upload, true);
        TA_FREEP(&priv->latest);
        priv->frame_id = 0;
        priv->dirty = false;
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

const struct vo_driver video_out_mario = {
    .description = "Mario window manager",
    .name = "mario",
    .preinit = preinit,
    .reconfig = reconfig,
    .query_format = query_format,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .control = control,
    .wakeup = wakeup,
    .wait_events = wait_events,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv){.fd = -1, .wakeup_pipe = {-1, -1}},
};
