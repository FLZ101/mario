#ifndef _SOCKET_H
#define _SOCKET_H

#include <fs/fs.h>

#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOCK_NONBLOCK O_NONBLOCK
#define SOCK_CLOEXEC O_CLOEXEC

int do_socketpair(int domain, int type, int protocol, int fd[2]);
int sys_socketcall(int call, unsigned long *args);
int sys_socketpair(int domain, int type, int protocol, int *fd);

#endif
