#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

int main()
{
	int err;

	char ch;
	while (1) {
		int ret = read(0, &ch, 1);
		if (-1 == ret) {
			_perror();
			exit(EXIT_FAILURE);
		}
		// EOF
		if (ret == 0)
			break;
		putchar(ch);
	}
    return 0;
}
