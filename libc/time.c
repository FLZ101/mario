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
