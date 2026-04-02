#include <time.h>
#include <stddef.h>

unsigned int sleep(unsigned int seconds)
{
    struct timespec req = { seconds, 0 }, rem = { 0, 0 };
    int err = nanosleep(&req, &rem);
    if (!err)
        return 0;
    return rem.tv_sec;
}

int usleep(unsigned int usec)
{
    struct timespec req = { 0, usec * 1000 };
    return nanosleep(&req, NULL);
}

static inline _syscall1(int,gettimeofday_time32,struct timeval*,tv)

time_t time(time_t *tloc)
{
    struct timeval tv = {0};
    int err = gettimeofday_time32(&tv);
    if (-1 == err)
        return -1;
    if (tloc)
        *tloc = tv.tv_sec;
    return tv.tv_sec;
}
