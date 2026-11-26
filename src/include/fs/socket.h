#ifndef _SOCKET_H
#define _SOCKET_H

#include <fs/fs.h>

#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_NONBLOCK O_NONBLOCK
#define SOCK_CLOEXEC O_CLOEXEC
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_ACCEPTCONN 30
#define SO_DEBUG 1
#define SO_DONTROUTE 5
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_MARK 36

#define IPPROTO_IP 0
#define IP_TTL 2
#define IP_RECVTTL 12

struct socket_iovec {
	void *base;
	unsigned int len;
};

struct socket_msghdr {
	void *name;
	unsigned int namelen;
	struct socket_iovec *iov;
	int iovlen;
	void *control;
	unsigned int controllen;
	int flags;
};

int do_socketpair(int domain, int type, int protocol, int fd[2]);
void socket_init(void);
int sys_socketcall(int call, unsigned long *args);
int sys_socket(int domain, int type, int protocol);
int sys_socketpair(int domain, int type, int protocol, int *fd);
int sys_bind(int fd, const void *address, unsigned int length);
int sys_connect(int fd, const void *address, unsigned int length);
int sys_listen(int fd, int backlog);
int sys_accept4(int fd, void *address, unsigned int *length, int flags);
int sys_getsockname(int fd, void *address, unsigned int *length);
int sys_getpeername(int fd, void *address, unsigned int *length);
int sys_getsockopt(int fd, int level, int option, void *value,
		   unsigned int *length);
int sys_sendmsg(int fd, struct socket_msghdr *msg, int flags);
int sys_recvmsg(int fd, struct socket_msghdr *msg, int flags);
int sys_sendto(int fd, char *buffer, unsigned int length, int flags,
	       const void *address, unsigned int address_length);
int sys_recvfrom(int fd, char *buffer, unsigned int length, int flags,
		 void *address, unsigned int *address_length);
int sys_shutdown(int fd, int how);

#endif
