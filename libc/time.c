#include <unistd.h>
#include <time.h>

unsigned int sleep(unsigned int seconds)
{
    struct timespec req = { seconds, 0 }, rem = { 0, 0 };
    int err = nanosleep(&req, &rem);
    if (!err)
        return 0;
    return rem.tv_sec;
}
