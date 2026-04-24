#ifndef _APP_UTIL_H
#define _APP_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <termios.h>
#include <poll.h>

#define STRINGIFY(x)	#x
#define TO_STRING(x)	STRINGIFY(x)

#define PError() perror(__FILE__ ":" TO_STRING(__LINE__))

#define Exit() \
do { \
    PError(); \
    exit(EXIT_FAILURE); \
} while (0)

#define HandleErr(x) \
do { \
    if (-1 == (int)(x)) { \
        Exit(); \
    } \
} while (0)

char *Sprintf(const char *fmt, ...);

void Wait(pid_t pid);

void Run(char *filename, char **argv, char **envp);
void RunL(char *filename, ...);

static inline void Putchar(int ch)
{
    putchar(ch);
    fflush(stdout);
}

void PrintFd(int fd);
void PrintFile(char *filename);
void WriteFile(char *filename, char *content);
void ListDir(char *pathname);
char *GetDirentTypeName(unsigned char d_type);
char *GetModeName(mode_t mode);

void MakeTtyRaw(int fd, struct termios *t);
void RestoreTty(int fd, struct termios *t);

int Poll(struct pollfd *fds, unsigned int nfds, int timeout);

#endif /* _APP_UTIL_H */
