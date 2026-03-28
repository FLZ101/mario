#ifndef _APP_UTIL_H
#define _APP_UTIL_H

#include <stdio.h>
#include <stdlib.h>

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

void Run(char *filename, char **argv, char **envp);
void RunL(char *filename, ...);

void PrintFile(char *filename);
void ListDir(char *pathname);

#endif /* _APP_UTIL_H */
