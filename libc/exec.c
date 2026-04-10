#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include <mario/util.h>

char **environ = NULL;

_syscall3(int,execve,const char *,pathname,char **,argv,char **,envp)
_syscall4(int,execveat,int,dirfd,const char *,pathname,char **,argv,char **,envp)

static int vexecl(int p, int e, const char *file, char *arg, va_list ap)
{
    if (p)
        p = !strstr(file, "/");
    if (p) {
        file = Sprintf("/bin/%s", file);
        if (!file)
            return -1;
    }

    size_t n_arg = 1;
    va_list ap_copy;
    va_copy(ap_copy, ap);
    while (va_arg(ap_copy, char *) != NULL) {
        ++n_arg;
    }
    va_end(ap_copy);

    char **argv = (char **)calloc(n_arg + 1, sizeof(char *));
    if (!argv) {
        goto fail;
    }

    argv[0] = (char *)arg;
    for (int i = 1; i < n_arg; i++) {
        argv[i] = va_arg(ap, char *);
    }

    char **envp = environ;
    if (e)
        envp = va_arg(ap, char **);

    execve(file, argv, envp);

fail:
    if (p)
        free((void *)file);
    return -1;
}

// l: command arguments are passed with **variadic arguments**
// v: command arguments are passed with an array
// e: use specified **envp** rather than **environ**
// p: search the program in PATH if its name does not conatin '/'

int execl(const char *pathname, char *arg, ...)
{
    int err;
    va_list ap;

	va_start(ap, arg);
	err = vexecl(0, 0, pathname, arg, ap);
	va_end(ap);

    return err;
}

int execlp(const char *file, char *arg, ...)
{
    int err;
    va_list ap;

	va_start(ap, arg);
	err = vexecl(1, 0, file, arg, ap);
	va_end(ap);

    return err;
}

int execle(const char *pathname, char *arg, ...)
{
    int err;
    va_list ap;

	va_start(ap, arg);
	err = vexecl(0, 1, pathname, arg, ap);
	va_end(ap);

    return err;
}

int execv(const char *pathname, char *argv[])
{
    return execve(pathname, argv, environ);
}

int execvpe(const char *file, char *argv[], char *envp[])
{
    if (strstr(file, "/"))
        return execve(file, argv, envp);

    char *pathname = Sprintf("/bin/%s", file);
    if (!pathname)
        return -1;

    int err = execve(pathname, argv, envp);
    free(pathname);
    return err;
}

int execvp(const char *file, char *argv[])
{
    return execvpe(file, argv, environ);
}
