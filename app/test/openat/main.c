#include <fcntl.h>
#include <unistd.h>
#include <app/util.h>

int main(void)
{
    int dirfd = open("/etc", O_RDONLY | O_DIRECTORY);
    HandleErr(dirfd);

    printf("openat) /etc, passwd\n");
    int fd = openat(dirfd, "passwd", O_RDONLY);
    HandleErr(fd);
    PrintFd(fd);
    HandleErr(close(fd));

    HandleErr(close(dirfd));

    printf("openat) CWD, etc/passwd\n");
    fd = openat(AT_FDCWD, "etc/passwd", O_RDONLY);
    HandleErr(fd);
    PrintFd(fd);
    HandleErr(close(fd));
}
