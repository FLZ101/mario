#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <errno.h>

#include <app/util.h>

struct termios t;

void restore_tty() {
  HandleErr(tcsetattr(STDIN_FILENO, TCSAFLUSH, &t));
}

void init() {
  HandleErr(tcgetattr(STDIN_FILENO, &t));
  atexit(restore_tty);

  struct termios raw = t;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  HandleErr(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw));

  int flags = fcntl(STDIN_FILENO, F_GETFL);
  HandleErr(flags);

  flags |= O_NONBLOCK;
  HandleErr(fcntl(STDIN_FILENO, F_SETFL, flags));
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
