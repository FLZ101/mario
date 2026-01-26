#include <syscall.h>
#include <errno.h>

_syscall1(int,_exit,int,status)

void exit(int status)
{
    _exit(status);
}
