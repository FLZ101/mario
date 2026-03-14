#include <app/util.h>

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <dirent.h>
#include <string.h>
#include <stdarg.h>

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

void Run(char *filename, char **argv, char **envp)
{
    int err;
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
        int status = 0;
        err = waitpid(pid, &status, 0);
        if (-1 == err)
            Exit();
        if (WIFEXITED(status)) {
            printf("[run] status = %d\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("[run] signal = %d\n", WTERMSIG(status));
        }
    }

    if (p)
        free(filename);
}

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

void PrintFile(char *filename)
{
    int err;

    int fd = open(filename, O_RDONLY);
    if (-1 == fd)
        Exit();

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

    err = close(fd);
    if (-1 == err)
        Exit();
}

#define N 1024

void ListDir(char *pathname)
{
    char buf[N];

    printf("[list] %s\n", pathname);

    int fd = open(pathname, O_RDONLY | O_DIRECTORY);
    if (-1 == fd)
        Exit();

    while (1) {
        int count = getdents(fd, buf, N);
        if (-1 == count)
            Exit();
        // end of directory
        if (0 == count)
            break;

        int off = 0;
        while (off < count) {
            struct mario_dirent *dirent = (struct mario_dirent *) (buf + off);
            printf("%x %x %s\n", dirent->d_ino, dirent->d_off, dirent->d_name);
            off += dirent->d_reclen;
        }
    }

    HandleErr(close(fd));
}

#undef N
