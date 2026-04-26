#include <unistd.h>
#include <app/util.h>

int main()
{
    HandleErr(chdir("/"));

    RunL("/bin/example/sl.exe", NULL);
    RunL("/bin/example/cp.exe", "/etc/welcome.txt", "/etc/tmp.txt", NULL);
    RunL("/bin/example/cat.exe", "/etc/tmp.txt", NULL);

    RunL("/bin/test/chrdev.exe", NULL);
    Run("/bin/test/exec.exe",
        (char *[]){"hello", "world", NULL},
        (char *[]){"quick", "brown", "fox", NULL});
    RunL("/bin/test/fcntl.exe", NULL);
    RunL("/bin/test/fork.exe", NULL);
    RunL("/bin/test/mm.exe", NULL);
    // Will fail if the number of ramdisks loaded is less than 3
    RunL("/bin/test/mount.exe", NULL);
    RunL("/bin/test/pipe.exe", NULL);
    RunL("/bin/test/signal.exe", NULL);
    RunL("/bin/test/getcwd.exe", NULL);
    RunL("/bin/test/openat.exe", NULL);
    RunL("/bin/test/stat.exe", NULL);
    RunL("/bin/test/symlink.exe", NULL);
    RunL("/bin/test/poll_tty.exe", NULL);
    RunL("/bin/test/poll_pipe.exe", NULL);
    RunL("/bin/test/select_tty.exe", NULL);
    RunL("/bin/test/select_pipe.exe", NULL);
#ifdef _MUSL
    RunL("/bin/test/time.exe", NULL);
    RunL("/bin/test/float.exe", NULL);
#endif
    return 0;
}
