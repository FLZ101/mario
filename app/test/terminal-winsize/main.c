#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct winsize initial;
	struct winsize size;
	FILE *report = NULL;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &initial) < 0) {
		perror("terminal-winsize: initial TIOCGWINSZ");
		return 1;
	}
	/* Optional report path lets GUI tests observe PTY resize without terminal stdout mirroring. */
	if (argc > 1) {
		report = fopen(argv[1], "w");
		if (report == NULL) {
			perror("terminal-winsize: report file");
			return 1;
		}
	}
	fputs("terminal-winsize: ready\n", stderr);
	if (report != NULL) {
		fputs("terminal-winsize: ready\n", report);
		fflush(report);
	}
	for (;;) {
		if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0) {
			perror("terminal-winsize: TIOCGWINSZ");
			return 1;
		}
		if (size.ws_row != initial.ws_row || size.ws_col != initial.ws_col) {
			printf("%u %u\n", size.ws_row, size.ws_col);
			fflush(stdout);
			if (report != NULL) {
				fprintf(report, "%u %u\n", size.ws_row, size.ws_col);
				fflush(report);
			}
			/* Keep the child alive until the terminal commits its resized frame. */
			for (;;)
				pause();
		}
		sleep(1);
	}
}
