#include <stdio.h>

int errno;

char *strerror(int errnum)
{
    switch (errnum) {

#define USE(name, code, msg) case code: return msg;
#include "errno.def"
#undef USE

    default:
        return "Unknown error";
    }
}

void perror(const char *s)
{
    if (s && *s)
        printf("%s : %s\n", s, strerror(errno));
    else
        printf("%s\n", strerror(errno));
}
