#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
	struct winsize initial;
	struct winsize size;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &initial) < 0) {
		perror("terminal-winsize: initial TIOCGWINSZ");
		return 1;
	}
	fputs("terminal-winsize: ready\n", stderr);
	for (;;) {
		if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0) {
			perror("terminal-winsize: TIOCGWINSZ");
			return 1;
		}
		if (size.ws_row != initial.ws_row || size.ws_col != initial.ws_col) {
			printf("%u %u\n", size.ws_row, size.ws_col);
			fflush(stdout);
			/* Keep the child alive until the terminal commits its resized frame. */
			for (;;)
				pause();
		}
		sleep(1);
	}
}
