#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main()
{
    time_t t;

    t = time(NULL);
    printf("%s", ctime(&t));

    sleep(3);

    t = time(NULL);
    printf("%s", ctime(&t));

    struct timespec ts = { .tv_sec = 2, .tv_nsec = 1000 * 1000 * 1000 - 1 };
    nanosleep(&ts, NULL);

    t = time(NULL);
    printf("%s", ctime(&t));

    return 0;
}
