#include <dirent.h>
#include <syscall.h>
#include <sys/types.h>

_syscall3(ssize_t,getdents,int,fd,void *,dirp, size_t,count)
