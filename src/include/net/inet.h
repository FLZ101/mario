#ifndef MARIO_NET_INET_H
#define MARIO_NET_INET_H

struct file;
struct file_operations;
struct socket_msghdr;

extern struct file_operations inet_fops;
unsigned int inet_udp_drop_count(void);

int inet_socket(int type, int protocol);
int inet_bind(struct file *file, const void *address, unsigned int length);
int inet_connect(struct file *file, const void *address, unsigned int length);
int inet_listen(struct file *file, int backlog);
int inet_accept(struct file *file, void *address, unsigned int *length, int flags);
int inet_name(struct file *file, void *address, unsigned int *length, int peer);
int inet_shutdown(struct file *file, int how);
int inet_setsockopt(struct file *file, int level, int option, const void *value,
		    unsigned int length);
int inet_getsockopt(struct file *file, int level, int option, void *value,
		    unsigned int *length);
int inet_sendto(struct file *file, const char *buffer, unsigned int length,
		int flags, const void *address, unsigned int address_length);
int inet_recvfrom(struct file *file, char *buffer, unsigned int length,
		  int flags, void *address, unsigned int *address_length);
int inet_sendmsg(struct file *file, struct socket_msghdr *message, int flags);
int inet_recvmsg(struct file *file, struct socket_msghdr *message, int flags);

#endif
