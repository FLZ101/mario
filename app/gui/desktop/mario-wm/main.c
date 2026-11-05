#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <mario/input.h>
#include <gui/protocol.h>

#define WM_SOCKET_PATH "/tmp/mario-wm.sock"
#define WM_STARTED_PATH "/tmp/mario-wm.started"
#define WM_READY_PATH "/tmp/mario-wm.ready"
#define WM_FRAME_PATH "/tmp/mario-wm.frame"
#define WM_CLIENT_LIMIT 8

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static void remove_status_files(void)
{
	/* These are server-owned diagnostics, never client-controlled state. */
	unlink(WM_SOCKET_PATH);
	unlink(WM_STARTED_PATH);
	unlink(WM_READY_PATH);
	unlink(WM_FRAME_PATH);
}

typedef struct {
	int framebuffer;
	int gui;
	int listener;
	int owns_status;
} wm_state;

typedef struct {
	int fd;
	int hello;
	uint32_t window_id;
	gui_protocol_input *input;
	gui_protocol_output *output;
} wm_client;

static uint16_t get_u16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static uint32_t get_u32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	       (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void put_u16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
	for (unsigned int index = 0; index < 4; index++)
		bytes[index] = value >> (index * 8);
}

static void put_u64(uint8_t *bytes, uint64_t value)
{
	for (unsigned int index = 0; index < 8; index++)
		bytes[index] = value >> (index * 8);
}

static void client_destroy(wm_client *client)
{
	if (client->fd >= 0)
		close(client->fd);
	gui_protocol_input_destroy(client->input);
	gui_protocol_output_destroy(client->output);
	*client = (wm_client) { .fd = -1 };
}

static int client_enqueue(wm_client *client, gui_protocol_type type,
			  const void *payload, size_t payload_bytes)
{
	uint8_t *frame;
	uint32_t length;
	int result;

	if (gui_protocol_message_length(payload_bytes, &length) < 0 ||
	    (frame = malloc(length)) == NULL)
		return -1;
	if (gui_protocol_encode_header(frame, length, type, payload_bytes) < 0) {
		free(frame);
		return -1;
}
	if (payload_bytes != 0)
		memcpy(frame + GUI_PROTOCOL_HEADER_SIZE, payload, payload_bytes);
	result = gui_protocol_output_enqueue(client->output, frame, length);
	free(frame);
	return result;
}

static ssize_t client_write(void *context, const void *bytes, size_t byte_count)
{
	wm_client *client = context;

	return write(client->fd, bytes, byte_count);
}

static int client_flush(wm_client *client)
{
	return gui_protocol_output_flush(client->output, client_write, client) !=
	       GUI_PROTOCOL_OUTPUT_ERROR ? 0 : -1;
}

static void client_message(void *context, const gui_protocol_header *header,
			   const void *payload, size_t payload_bytes)
{
	wm_client *client = context;
	const uint8_t *bytes = payload;
	uint8_t reply[sizeof(gui_protocol_wire_window_create_reply)];
	uint8_t focus[sizeof(gui_protocol_wire_window_focus)];

	if (!client->hello) {
		if (header->type != GUI_PROTOCOL_HELLO || payload_bytes != 4 ||
		    get_u16(bytes) != GUI_PROTOCOL_VERSION_MAJOR ||
		    get_u16(bytes + 2) > GUI_PROTOCOL_VERSION_MINOR) {
			client->hello = -1;
			return;
		}
		put_u16(reply, GUI_PROTOCOL_VERSION_MAJOR);
		put_u16(reply + 2, GUI_PROTOCOL_VERSION_MINOR);
		client->hello = client_enqueue(client, GUI_PROTOCOL_HELLO_REPLY, reply, 4) == 0;
		if (!client->hello)
			client->hello = -1;
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_CREATE && client->window_id == 0 &&
	    payload_bytes >= sizeof(gui_protocol_wire_window_create) &&
	    payload_bytes == sizeof(gui_protocol_wire_window_create) + get_u16(bytes + 12) &&
	    get_u32(bytes + 4) != 0 && get_u32(bytes + 8) != 0) {
		client->window_id = 1;
		put_u32(reply, get_u32(bytes));
		put_u32(reply + 4, client->window_id);
		put_u64(reply + 8, 1);
		put_u32(reply + 16, get_u32(bytes + 4));
		put_u32(reply + 20, get_u32(bytes + 8));
		put_u32(focus, client->window_id);
		put_u32(focus + 4, 1);
		if (client_enqueue(client, GUI_PROTOCOL_WINDOW_CREATE_REPLY, reply,
				   sizeof(reply)) < 0 ||
		    client_enqueue(client, GUI_PROTOCOL_WINDOW_FOCUS, focus,
				   sizeof(focus)) < 0)
			client->hello = -1;
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_DESTROY && payload_bytes == 4 &&
	    get_u32(bytes) == client->window_id) {
		client->window_id = 0;
		return;
	}
	client->hello = -1;
}

static void wm_cleanup(wm_state *state)
{
	/* Close the listener before releasing the descriptors that own the seat. */
	if (state->listener >= 0)
		close(state->listener);
	if (state->gui >= 0)
		close(state->gui);
	if (state->framebuffer >= 0)
		close(state->framebuffer);
	if (state->owns_status)
		remove_status_files();
}

static int create_started_marker(void)
{
	static const char marker[] = "listening\n";
	int fd = open(WM_STARTED_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0)
		return -1;
	if (write(fd, marker, sizeof(marker) - 1) != sizeof(marker) - 1) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}
	return close(fd);
}

static int bind_listener(void)
{
	struct sockaddr_un address;
	int listener;

	_Static_assert(sizeof(WM_SOCKET_PATH) <= sizeof(address.sun_path),
		       "window manager socket path fits sockaddr_un");
	listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (listener < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, WM_SOCKET_PATH, sizeof(WM_SOCKET_PATH));
	if (bind(listener, (const struct sockaddr *)&address,
		 sizeof(address.sun_family) + sizeof(WM_SOCKET_PATH)) < 0 ||
	    listen(listener, 16) < 0) {
		int saved_errno = errno;

		close(listener);
		errno = saved_errno;
		return -1;
	}
	return listener;
}

int main(void)
{
	struct sigaction action = { .sa_handler = request_stop };
	struct pollfd pollfds[WM_CLIENT_LIMIT + 2];
	struct mario_gui_key_event events[16];
	wm_state state = { .framebuffer = -1, .gui = -1, .listener = -1 };
	wm_client clients[WM_CLIENT_LIMIT];
	int status = 1;

	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++)
		clients[index].fd = -1;

	/* Acquire both descriptors before F8 may expose the graphics seat. */
	state.framebuffer = open("/dev/fb0", O_RDWR);
	if (state.framebuffer < 0)
		goto out;
	state.gui = open("/dev/gui", O_RDONLY | O_NONBLOCK);
	if (state.gui < 0)
		goto out;
	state.owns_status = 1;
	/* Only the exclusive owner may discard a predecessor's stale status. */
	remove_status_files();
	state.listener = bind_listener();
	if (state.listener < 0)
		goto out;
	if (create_started_marker() < 0)
		goto out;
	if (sigaction(SIGTERM, &action, NULL) < 0 ||
	    sigaction(SIGINT, &action, NULL) < 0)
		goto out;
	printf("mario-wm: listening\n");
	fflush(stdout);
	while (!stop_requested) {
		int ready;
		size_t poll_count = 2;

		pollfds[0] = (struct pollfd) { .fd = state.gui, .events = POLLIN };
		pollfds[1] = (struct pollfd) { .fd = state.listener, .events = POLLIN };
		for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
			if (clients[index].fd < 0)
				continue;
			pollfds[poll_count++] = (struct pollfd) {
				.fd = clients[index].fd,
				.events = POLLIN | POLLOUT,
			};
		}
		ready = poll(pollfds, poll_count, -1);

		if (ready < 0) {
			if (errno == EINTR)
				continue;
			goto out;
		}
		if (pollfds[0].revents & POLLIN) {
			/* Drain ownership input now; M3.2.4 assigns it to an endpoint. */
			while (read(state.gui, events, sizeof(events)) > 0)
				;
			if (errno != EAGAIN)
				goto out;
		}
		if (pollfds[1].revents & POLLIN) {
			int fd = accept4(state.listener, NULL, NULL, SOCK_NONBLOCK);

			if (fd >= 0) {
				for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
					if (clients[index].fd >= 0)
						continue;
					clients[index] = (wm_client) { .fd = fd,
						.input = gui_protocol_input_create(),
						.output = gui_protocol_output_create(GUI_PROTOCOL_OUTPUT_LIMIT) };
					if (clients[index].input == NULL || clients[index].output == NULL)
						client_destroy(&clients[index]);
					fd = -1;
					break;
				}
				if (fd >= 0)
					close(fd);
			}
		}
		for (size_t index = 0, poll_index = 2; index < WM_CLIENT_LIMIT; index++) {
			uint8_t bytes[4096];
			ssize_t count = -1;

			if (clients[index].fd < 0)
				continue;
			if (pollfds[poll_index].revents & POLLIN) {
				while ((count = recv(clients[index].fd, bytes, sizeof(bytes), 0)) > 0) {
					if (gui_protocol_input_append(clients[index].input, bytes, count) < 0 ||
					    gui_protocol_input_drain(clients[index].input, client_message,
							     &clients[index]) < 0 ||
					    client_flush(&clients[index]) < 0)
						clients[index].hello = -1;
				}
				if (count == 0 ||
				    (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
					clients[index].hello = -1;
			}
			if (clients[index].fd >= 0 && (pollfds[poll_index].revents & POLLOUT) &&
			    client_flush(&clients[index]) < 0)
				clients[index].hello = -1;
			if (clients[index].hello < 0)
				client_destroy(&clients[index]);
			poll_index++;
		}
	}
	status = 0;
out:
	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++)
		client_destroy(&clients[index]);
	if (status != 0)
		perror("mario-wm");
	wm_cleanup(&state);
	return status;
}
