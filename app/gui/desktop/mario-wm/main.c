#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <mario/input.h>

#define WM_SOCKET_PATH "/tmp/mario-wm.sock"
#define WM_STARTED_PATH "/tmp/mario-wm.started"
#define WM_READY_PATH "/tmp/mario-wm.ready"
#define WM_FRAME_PATH "/tmp/mario-wm.frame"

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
	struct pollfd pollfd;
	struct mario_gui_key_event events[16];
	wm_state state = { .framebuffer = -1, .gui = -1, .listener = -1 };
	int status = 1;

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
	pollfd = (struct pollfd) { .fd = state.gui, .events = POLLIN };
	while (!stop_requested) {
		int ready = poll(&pollfd, 1, -1);

		if (ready < 0) {
			if (errno == EINTR)
				continue;
			goto out;
		}
		if (!(pollfd.revents & POLLIN))
			continue;
		/* Drain ownership input now; M3.2.4 assigns it to an endpoint. */
		while (read(state.gui, events, sizeof(events)) > 0)
			;
		if (errno != EAGAIN)
			goto out;
		pollfd.revents = 0;
	}
	status = 0;
out:
	if (status != 0)
		perror("mario-wm");
	wm_cleanup(&state);
	return status;
}
