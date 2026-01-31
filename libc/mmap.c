#include <sys/mman.h>
#include <syscall.h>

_syscall1(int,mmap_,struct mmap_arg_struct*,arg)
_syscall2(int,munmap,void *,addr,size_t,length)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    struct mmap_arg_struct arg = {
        .addr = (unsigned long)addr,
        .len = length,
        .prot = prot,
        .flags = flags,
        .fd = fd,
        .offset = offset
    };
    return (void *)mmap_(&arg);
}
