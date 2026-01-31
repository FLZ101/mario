#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <syscall.h>

static inline __attribute__((always_inline)) _syscall0(pid_t,fork)
static inline __attribute__((always_inline)) _syscall0(int,pause)

static inline _syscall3(int,read,int,fd,void *,buf,size_t,count)
static inline _syscall3(int,write,int,fd,const char *,buf,off_t,count)
static inline _syscall1(int,close,int,fd)
static inline _syscall1(int,dup,int,fd)
static inline _syscall3(int,execve,const char *,file,char **,argv,char **,envp)
static inline _syscall1(int,pipe,int *,pipefd)
static inline _syscall0(pid_t,getpid)
static inline _syscall0(pid_t,getppid)

__attribute__ ((__noreturn__)) void _exit(int);

unsigned int sleep(unsigned int seconds);

#endif	/* _UNISTD_H */
