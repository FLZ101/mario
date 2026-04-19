#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <app/util.h>

int main()
{
    HandleErr(mkdir("tmp", 0));
    HandleErr(chdir("tmp"));

    HandleErr(mkdir("foo", 0));
    ListDir(".");

    WriteFile("bar.txt", "quick brown fox\n");
    HandleErr(symlink("../bar.txt", "foo/bar.txt"));

    ListDir("foo");

    PrintFile("foo/bar.txt");

    HandleErr(symlink("foo", "zoo"));

    PrintFile("zoo/bar.txt");
    return 0;
}
