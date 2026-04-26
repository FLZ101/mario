#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>

#include <app/util.h>

void select_read(int fd) {
    int flags = fcntl(fd, F_GETFL);
    HandleErr(flags);

    flags |= O_NONBLOCK;
    HandleErr(fcntl(fd, F_SETFL, flags));

    char ch;
    while (1) {
        int err = read(fd, &ch, 1);
        if (err == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(fd, &readfds);

                struct timeval tv = {.tv_sec = 3};
                HandleErr(select(fd + 1, &readfds, NULL, NULL, &tv));
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

        select_read(pipefd[0]);

        close(pipefd[0]);
    } else {
        close(pipefd[0]);

        for (char *p = "the quick brown fox\n";; ++p) {
            HandleErr(write(pipefd[1], p, 1));
            if (!*p)
                break;
            sleep(1);
        }

        close(pipefd[1]);
    }
    return 0;
}
