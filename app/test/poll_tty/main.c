#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <app/util.h>

void poll_tty(char *device) {
    printf("Poll %s. Press q to exit\n", device);

    int fd = open(device, O_RDONLY);
    HandleErr(fd);

    struct termios t;
    MakeTtyRaw(fd, &t);

    char ch = '@';
    while (1) {
        int err = read(fd, &ch, 1);
        if (err == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                struct pollfd fds[1];
                fds[0].fd = fd;
                fds[0].events = POLLIN | POLLPRI; // Data or Priority data

                Poll(fds, 1, 3000);
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
    poll_tty("/dev/tty");
    // poll_tty("/dev/tty2");
    // poll_tty("/dev/ttyS0");
    return 0;
}
