#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char *argv[], char *envp[])
{
	int fd = open("/etc/welcome.txt", O_RDONLY);
	char ch;
	while (1) {
		int ret = read(fd, &ch, 1);
		if (ret == 0 || ret == -1) {
			break;
		}
		putchar(ch);
	}
	return 0;
}
