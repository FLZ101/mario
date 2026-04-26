#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>

#include <app/util.h>

void select_tty(char *device) {
    printf("Select %s. Press q to exit\n", device);

    int fd = open(device, O_RDONLY);
    HandleErr(fd);

    struct termios t;
    MakeTtyRaw(fd, &t);

    char ch = '@';
    while (1) {
        int err = read(fd, &ch, 1);
        if (err == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(fd, &readfds);

                struct timeval tv = { .tv_sec = 3 };
                HandleErr(select(fd + 1, &readfds, NULL, NULL, &tv));
                continue;
            }
            Exit();
        }
        if (ch == 'q') {
            break;
        }
    }
    RestoreTty(fd, &t);

    HandleErr(close(fd));
}

int main() {
    select_tty("/dev/tty");
    // select_tty("/dev/tty2");
    // select_tty("/dev/ttyS0");
    return 0;
}
