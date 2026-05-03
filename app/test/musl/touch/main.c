#include <fcntl.h>
#include <sys/stat.h>

#include <app/util.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s FILE\n", argv[0]);
        exit(1);
    }
    HandleErr(utimensat(AT_FDCWD, argv[1], NULL, 0));
    return 0;
}
