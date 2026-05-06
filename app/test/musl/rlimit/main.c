#include <sys/resource.h>
#include <app/util.h>

int main()
{
    struct rlimit r;
    HandleErr(getrlimit(RLIMIT_NOFILE, &r));
    printf("RLIMIT_NOFILE: %lld, %lld\n", r.rlim_cur, r.rlim_max);
    return 0;
}
