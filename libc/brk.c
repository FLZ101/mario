#include <syscall.h>
#include <stddef.h>

_syscall1(int,brk_,void *,addr)

int brk(void *addr)
{
    return -1 == brk_(addr) ? -1 : 0;
}

void *sbrk(int increment)
{
    int addr = brk_(NULL); // get current program break
    if (-1 == addr)
        return (void *) -1;
    if (!increment)
        return (void *) addr;
    return (void *) brk_((void *) (addr + increment));
}
