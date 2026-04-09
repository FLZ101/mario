#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <app/util.h>

void print_statx(struct statx *stx)
{
    printf("%x %s %d\n", (long) stx->stx_ino,
        GetModeName(stx->stx_mode), (long) stx->stx_size);
}

int main()
{
    int err;
    struct statx stx;

    int dirfd = open("/etc", O_RDONLY | O_DIRECTORY);
    HandleErr(dirfd);

    printf("statx: /etc\n");
    err = statx(dirfd, "", 0, STATX_BASIC_STATS, &stx);
    HandleErr(err);
    print_statx(&stx);

    printf("statx: /etc, welcome.txt\n");
    err = statx(dirfd, "welcome.txt", 0, STATX_BASIC_STATS, &stx);
    HandleErr(err);
    print_statx(&stx);

    printf("statx: /etc, /etc/welcome.txt\n");
    err = statx(dirfd, "/etc/welcome.txt", 0, STATX_BASIC_STATS, &stx);
    HandleErr(err);
    print_statx(&stx);

    HandleErr(close(dirfd));

    printf("statx: CWD, etc/welcome.txt\n");
    err = statx(AT_FDCWD, "etc/welcome.txt", 0, STATX_BASIC_STATS, &stx);
    HandleErr(err);
    print_statx(&stx);

    printf("statx: CWD, /etc/welcome.txt\n");
    err = statx(AT_FDCWD, "/etc/welcome.txt", 0, STATX_BASIC_STATS, &stx);
    HandleErr(err);
    print_statx(&stx);

    int fd = open("etc/welcome.txt", O_RDONLY);
    HandleErr(fd);

    printf("statx: etc/welcome.txt\n");
    err = statx(fd, "", 0, STATX_BASIC_STATS, &stx);
    HandleErr(err);
    print_statx(&stx);

    HandleErr(close(fd));

    return 0;
}
