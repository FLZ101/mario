#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	int fb = open("/dev/fb0", O_RDWR);
	int gui;

	assert(fb >= 0);
	gui = open("/dev/gui", O_RDONLY);
	assert(gui >= 0);
	printf("gui-seat: ready\n");
	for (;;)
		pause();
}
