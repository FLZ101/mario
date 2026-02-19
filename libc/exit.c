#include <syscall.h>
#include <errno.h>

_syscall1(int,_exit,int,status)

#define MAX_EXIT_FN 32

static int n_exit_fn = 0;
static void (*exit_functions[MAX_EXIT_FN])(void);

int atexit(void (*f)(void))
{
    if (n_exit_fn >= MAX_EXIT_FN)
        return 1;
    exit_functions[n_exit_fn++] = f;
    return 0;
}

void exit(int status)
{
    for (int i = n_exit_fn - 1; i >= 0; --i)
        exit_functions[i]();
    _exit(status);
}
