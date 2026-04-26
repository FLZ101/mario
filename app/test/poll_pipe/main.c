#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <app/util.h>

void poll_read(int fd) {
    int flags = fcntl(fd, F_GETFL);
    HandleErr(flags);

    flags |= O_NONBLOCK;
    HandleErr(fcntl(fd, F_SETFL, flags));

    char ch;
    while (1) {
        int err = read(fd, &ch, 1);
        if (err == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                struct pollfd fds[1];
                fds[0].fd = fd;
                fds[0].events = POLLIN | POLLPRI; // Data or Priority data

                HandleErr(poll(fds, 1, 3000));
                continue;
            }
            Exit();
        }
        if (ch == '\0') {
            break;
        }
        Putchar(ch);
    }
}

int main() {
    int pipefd[2];
    HandleErr(pipe(pipefd));

    int pid = fork();
    HandleErr(pid);

    if (pid) {
        close(pipefd[1]);

        poll_read(pipefd[0]);

        close(pipefd[0]);
    } else {
        close(pipefd[0]);

        for (char *p = "the quick brown fox\n"; ; ++p) {
            HandleErr(write(pipefd[1], p, 1));
            if (!*p)
                break;
            sleep(1);
        }

        close(pipefd[1]);
    }
    return 0;
}
