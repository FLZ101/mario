#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

int main(int argc, char *argv[])
{
	if (argc > 1) {
		FILE *f = fopen(argv[1], "r");
		if (!f) {
			_perror();
			exit(EXIT_FAILURE);
		}

		char *line = NULL;
		size_t len = 0;
		size_t n_read = 0;
		while ((n_read = getline(&line, &len, f)) != -1) {
			fwrite(line, 1, n_read, stdout);
		}

		if (line)
			free(line);
		return 0;
	}

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
