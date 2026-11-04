#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mario/input.h>

int main(void)
{
	struct mario_gui_key_event event;
	int status;
	int fb = open("/dev/fb0", O_RDWR);
	int fb_dup;
	int gui;
	int gui_dup;
	pid_t pid;

	assert(fb >= 0);
	gui = open("/dev/gui", O_RDONLY);
	assert(gui >= 0);

	/* dup() shares each file description, so closing these names keeps the seat. */
	fb_dup = dup(fb);
	gui_dup = dup(gui);
	assert(fb_dup >= 0 && gui_dup >= 0);
	assert(close(fb) == 0);
	assert(close(gui) == 0);
	fb = fb_dup;
	gui = gui_dup;

	/* A fork creates child descriptions that must not release the parent's seat. */
	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		assert(close(gui) == 0);
		assert(close(fb) == 0);
		_exit(0);
	}
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	printf("gui-seat-lifetime: ready\n");
	fflush(stdout);
	/* Consume the whole QMP GUI-TYPE sequence before returning input to tty6. */
	for (unsigned int index = 0; index < 4; index++) {
		assert(read(gui, &event, sizeof(event)) == sizeof(event));
		if (index < 2)
			assert(event.scancode == 0x2d && event.pressed == !(index & 1));
		else
			assert(event.scancode == 0x1c && event.pressed == !(index & 1));
	}

	/* These are the final descriptions, so release returns to the saved text VT. */
	assert(close(gui) == 0);
	assert(close(fb) == 0);
	printf("gui-seat-lifetime: released\n");
	fflush(stdout);
	return 0;
}
