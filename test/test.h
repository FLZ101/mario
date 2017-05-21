#ifndef _TEST_H
#define _TEST_H

#include <misc.h>

#include <fs/fs.h>

#include <mm/mm.h>
#include <mm/mman.h>
#include <mm/kmalloc.h>

#define __KERNEL_SYSCALLS__
#include <unistd.h>

#include <lib/stddef.h>

extern void kernel_thread(void (*fun)(void *), void *arg);

extern int sys_alarm(long seconds);
extern int sys_open(const char *filename, int flags);
extern int sys_read(unsigned int fd, char *buf, unsigned int count);
extern int sys_write(unsigned int fd, char *buf, unsigned int count);
extern int sys_close(unsigned int fd);
extern int sys_lseek(unsigned int fd, off_t offset, unsigned int origin);
extern int sys_stat(char *filename, struct stat *statbuf);
extern int sys_fstat(unsigned int fd, struct stat *statbuf);
extern int sys_truncate(const char *path, int length);
extern int sys_creat(const char *pathname);
extern int sys_ftruncate(unsigned int fd, int length);
extern int sys_chdir(const char *filename);
extern int sys_rmdir(char *pathname);
extern int sys_mkdir(char *pathname);
extern int sys_chroot(char *filename);
extern int sys_fchdir(char *filename);
extern int sys_link(char *oldname, char *newname);
extern int sys_unlink(char *pathname);
extern int sys_rename(char *oldname, char *newname);
extern int sys_getdents(unsigned int fd, void *dirent, unsigned int count);
extern int sys_mknod(char *filename, int mode, dev_t dev);
extern int sys_mount(char *dev_name, char *dir_name, char *type);
extern int sys_umount(char *name);
extern int sys_munmap(unsigned long addr, unsigned long len);
extern int sys_mmap(struct mmap_arg_struct *arg);
extern int sys_exit(int error_code);
extern int sys_waitpid(int pid, int *status, int option);
extern int sys_pause(void);
extern int sys_getpgid(pid_t pid);
extern int sys_getpgrp(void);
extern int sys_setpgid(pid_t pid, pid_t pgid);
extern int sys_setsid(void);
extern int sys_getpid(void);
extern int sys_getppid(void);
extern int sys_kill(int pid, int sig);
extern int sys_sigprocmask(int how, sigset_t *set, sigset_t *oset);
extern int sys_sigpending(sigset_t *set);
extern unsigned long sys_signal(int signum, void (*handler)(int));
extern int sys_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

#endif	/* _TEST_H */
