#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main(int argc, char *argv[])
{
    if (argc < 3) {
        return EXIT_FAILURE;
    }

    FILE *src_f = fopen(argv[1], "r");
    if (!src_f) {
        _perror();
    }
    FILE *dst_f = fopen(argv[2], "w");
    if (!dst_f) {
        _perror();
    }

    char c;
    size_t n_read = 0;
    size_t n_write = 0;

    while (1) {
        errno = 0;
        if (fread(&c, 1, 1, src_f)) {
            ++n_read;
        } else {
            perror("fread");
            break;
        }

        errno = 0;
        if (fwrite(&c, 1, 1, dst_f)) {
            ++n_write;
        } else {
            perror("fwrite");
            break;
        }
    }

    if (fclose(src_f))
        perror("fclose src_f");
    if (fclose(dst_f))
        perror("fclose dst_f");

    printf("n_read = %d, n_write = %d\n", n_read, n_write);
    return 0;
}
