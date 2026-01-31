#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <syscall.h>

static inline __attribute__((always_inline)) _syscall0(pid_t,fork)
static inline __attribute__((always_inline)) _syscall0(int,pause)

int brk(void *addr);
void *sbrk(int increment);

static inline _syscall3(int,read,int,fd,void *,buf,size_t,count)
static inline _syscall3(int,write,int,fd,const char *,buf,off_t,count)
static inline _syscall1(int,close,int,fd)
static inline _syscall1(int,dup,int,fd)
static inline _syscall2(int,dup2,int,oldfd,int,newfd)
static inline _syscall3(int,execve,const char *,file,char **,argv,char **,envp)
static inline _syscall1(int,pipe,int *,pipefd)
static inline _syscall0(pid_t,getpid)
static inline _syscall0(pid_t,getppid)
static inline _syscall1(int,rmdir,const char *,pathname)
static inline _syscall1(unsigned int,alarm,unsigned int,seconds)
static inline _syscall1(int,chdir,const char *,path)
static inline _syscall1(int,fchdir,int,fd)
static inline _syscall1(int,chroot,const char *,path)

__attribute__ ((__noreturn__)) void _exit(int);

unsigned int sleep(unsigned int seconds);

#endif	/* _UNISTD_H */
