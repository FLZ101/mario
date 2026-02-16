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
static inline _syscall2(int,link,const char *,oldpath, const char *,newpath)
static inline _syscall2(int,truncate,const char *,path, off_t, length)
static inline _syscall2(int,ftruncate,int, fd, off_t, length)
static inline _syscall1(int,unlink,const char *,pathname)
static inline _syscall2(int,setpgid,pid_t,pid,pid_t,pgid)
static inline _syscall1(pid_t,getpgid,pid_t,pid)
static inline _syscall1(pid_t,getsid,pid_t,pid)
static inline _syscall0(pid_t,setsid)

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

static inline _syscall3(off_t,lseek,int,fd, off_t, offset, int, whence)

__attribute__ ((__noreturn__)) void _exit(int);

unsigned int sleep(unsigned int seconds);

pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);

#endif	/* _UNISTD_H */
