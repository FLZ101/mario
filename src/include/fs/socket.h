#ifndef _SOCKET_H
#define _SOCKET_H

#include <fs/fs.h>

#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOCK_NONBLOCK O_NONBLOCK
#define SOCK_CLOEXEC O_CLOEXEC
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

int do_socketpair(int domain, int type, int protocol, int fd[2]);
void socket_init(void);
int sys_socketcall(int call, unsigned long *args);
int sys_socket(int domain, int type, int protocol);
int sys_socketpair(int domain, int type, int protocol, int *fd);
int sys_bind(int fd, const void *address, unsigned int length);
int sys_connect(int fd, const void *address, unsigned int length);
int sys_listen(int fd, int backlog);
int sys_accept4(int fd, void *address, unsigned int *length, int flags);
int sys_getsockopt(int fd, int level, int option, void *value,
		   unsigned int *length);
struct socket_msghdr;
int sys_sendmsg(int fd, struct socket_msghdr *msg, int flags);
int sys_recvmsg(int fd, struct socket_msghdr *msg, int flags);
int sys_shutdown(int fd, int how);

#endif
