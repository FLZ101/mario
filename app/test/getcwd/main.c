#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>

#include "app/util.h"

#define N 100
char buf[N];

void pwd()
{
    char *s = getcwd(buf, N);
    if (!s)
        Exit();
    printf("cwd: %s\n", s);

}

int main()
{
    pwd();

    mkdir("wang-ye-nan", 0755);
    chdir("wang-ye-nan");
    pwd();

    mkdir("wu-wen-chen", 0755);
    chdir("wu-wen-chen");
    pwd();

    chdir("..");
    pwd();

    chdir("..");
    pwd();

    chdir("..");
    pwd();

    return 0;
}
