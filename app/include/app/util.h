#ifndef _APP_UTIL_H
#define _APP_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

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

void PrintFd(int fd);
void PrintFile(char *filename);
void WriteFile(char *filename, char *content);
void ListDir(char *pathname);
char *GetDirentTypeName(unsigned char d_type);
char *GetModeName(mode_t mode);

#endif /* _APP_UTIL_H */
