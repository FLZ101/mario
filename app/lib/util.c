#include <app/util.h>

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <dirent.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <assert.h>

char *Sprintf(const char *fmt, ...)
{
    size_t len;
    va_list ap;

    va_start(ap, fmt);
    len = vsprintf(NULL, fmt, ap);
    va_end(ap);

    char *buf = malloc(len + 1);
    if (!buf)
        return NULL;

    va_start(ap, fmt);
    (void) vsprintf(buf, fmt, ap);
    va_end(ap);

    return buf;
}

void Wait(pid_t pid)
{
    int status = 0;
    int err = waitpid(pid, &status, 0);
    if (-1 == err)
        Exit();
    if (WIFEXITED(status)) {
        printf("[wait] %d, status = %d\n", pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        printf("[wait] %d, signal = %d\n", pid, WTERMSIG(status));
    }
}

void Run(char *filename, char **argv, char **envp)
{
    pid_t pid;

    printf("[run] %s\n", filename);

    int p = !strstr(filename, "/");
    if (p) {
        filename = Sprintf("/bin/%s", filename);
        if (!filename)
            Exit();
    }

    pid = fork();
    if (-1 == pid)
        Exit();

    if (!pid) {
        execve(filename, argv, envp);
        Exit();
    } else {
        Wait(pid);
    }

    if (p)
        free(filename);
    sleep(1);
}

extern char **environ;

static void VRunL(char *filename, va_list ap)
{
    size_t n_arg = 1;
    va_list ap_copy;
    va_copy(ap_copy, ap);
    while (va_arg(ap_copy, const char *) != NULL) {
        ++n_arg;
    }
    va_end(ap_copy);

    char **argv = (char **)calloc(n_arg + 1, sizeof(char *));
    if (!argv)
        Exit();

    argv[0] = filename;
    for (int i = 1; i < n_arg; i++) {
        argv[i] = va_arg(ap, char *);
    }

    Run(filename, argv, environ);
}

// must end with NULL
void RunL(char *filename, ...)
{
    va_list ap;
    va_start(ap, filename);
    VRunL(filename, ap);
    va_end(ap);
}

void PrintFd(int fd)
{
    char ch;
    while (1) {
        int ret = read(fd, &ch, 1);
        if (-1 == ret)
            Exit();
        // EOF
        if (ret == 0)
            break;
        putchar(ch);
    }
}

void PrintFile(char *filename)
{
    printf("[print] %s\n", filename);

    int fd = open(filename, O_RDONLY);
    if (-1 == fd)
        Exit();

    PrintFd(fd);

    int err = close(fd);
    if (-1 == err)
        Exit();
}

void WriteFile(char *filename, char *content)
{
    printf("[write] %s\n", filename);

    int fd = open(filename, O_WRONLY | O_CREAT);
    HandleErr(fd);

    HandleErr(write(fd, content, strlen(content)));

    HandleErr(close(fd));
}

#define N 1024

char *GetDirentTypeName(unsigned char d_type)
{
    switch (d_type) {
    case DT_BLK:
        return "Block";
    case DT_CHR:
        return "Char";
    case DT_DIR:
        return "Dir";
    case DT_FIFO:
        return "Fifo";
    case DT_LNK:
        return "Link";
    case DT_REG:
        return "Reg";
    case DT_SOCK:
        return "Sock";
    default:
        return "?";
    }
}

char *GetModeName(mode_t mode)
{
    return GetDirentTypeName(IFTODT(mode));
}

void ListDir(char *pathname)
{
    char buf[N];

    printf("[list] %s\n", pathname);

    int fd = open(pathname, O_RDONLY | O_DIRECTORY);
    if (-1 == fd)
        Exit();

    while (1) {
        int count = getdents(fd, (struct dirent *) buf, N);
        if (-1 == count)
            Exit();
        // end of directory
        if (0 == count)
            break;

        int off = 0;
        while (off < count) {
            struct dirent *ent = (struct dirent *) (buf + off);
            // Size of d_ino and d_off might be 8 bytes, we must cast them to type of 4 bytes to match %x
            printf("%x %s %s", (long) ent->d_ino, GetDirentTypeName(ent->d_type), ent->d_name);

            if (ent->d_type == DT_LNK) {
                char buf[512];
                int n = readlinkat(fd, ent->d_name, buf, 512);
                HandleErr(n);
                assert(n < 512);
                buf[n] = '\0';
                printf(" -> %s", buf);
            }

            putchar('\n');
            off += ent->d_reclen;
        }
    }

    HandleErr(close(fd));
}

#undef N

void RestoreTty(int fd, struct termios *t)
{
    HandleErr(tcsetattr(fd, TCSAFLUSH, t));
}

void MakeTtyRaw(int fd, struct termios *t)
{
  HandleErr(tcgetattr(fd, t));

  struct termios raw = *t;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  HandleErr(tcsetattr(fd, TCSAFLUSH, &raw));

  int flags = fcntl(fd, F_GETFL);
  HandleErr(flags);

  flags |= O_NONBLOCK;
  HandleErr(fcntl(fd, F_SETFL, flags));
}

int Poll(struct pollfd *fds, unsigned int nfds, int timeout)
{
    int n = poll(fds, nfds, timeout);
    HandleErr(n);

    for (unsigned int i = 0; i < nfds; i++) {
        if (!fds[i].revents)
            continue;

        if (fds[i].revents & POLLIN)
            printf("FD %d: POLLIN (Data ready)\n", fds[i].fd);
        if (fds[i].revents & POLLOUT)
            printf("FD %d: POLLOUT (Space ready)\n", fds[i].fd);

        if (fds[i].revents & POLLHUP)
            printf("FD %d: POLLHUP (Hung up/Disconnected)\n", fds[i].fd);
        if (fds[i].revents & POLLERR)
            printf("FD %d: POLLERR (Device error)\n", fds[i].fd);
        if (fds[i].revents & POLLNVAL)
            printf("FD %d: POLLNVAL (Invalid FD)\n", fds[i].fd);
    }
    return n;
}
