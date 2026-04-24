#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <errno.h>

#include <app/util.h>

struct termios t;

void restore_tty() {
  RestoreTty(STDIN_FILENO, &t);
}

void init() {
  MakeTtyRaw(STDIN_FILENO, &t);
  atexit(restore_tty);
}

int main()
{
    init();

    printf("Press any key or wait 30s to exit.\n");
    int i = 0;
    while (i < 30) {
        printf("%c", 'A' + (i++ % 26));
        fflush(stdout);

        char c;
        int err = read(STDIN_FILENO, &c, 1);
        if (err == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                sleep(1);
                continue;
            }
            Exit();
        }
        break;
    }
    putchar('\n');
}
