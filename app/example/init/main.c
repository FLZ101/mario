#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <assert.h>

#include <app/util.h>

static volatile sig_atomic_t sigchld = 0;
static volatile sig_atomic_t sigint = 0;
static volatile sig_atomic_t sigterm = 0;

static void handle_sig(int sig)
{
    switch (sig) {
    case SIGCHLD:
        sigchld = 1;
        break;
    case SIGINT:
        sigint = 1;
        break;
    case SIGTERM:
        sigterm = 1;
        break;
    }
}

struct getty {
    const char *device;
    pid_t pid;
    time_t last_spawn;
};

static struct getty getties[] = {
    {.device = "/dev/tty1"},
    {.device = "/dev/tty2"},
    {.device = "/dev/tty3"},
    {.device = "/dev/tty4"},
    {.device = "/dev/tty5"},
    {.device = "/dev/tty6"},
    {.device = "/dev/ttyS0"},
    {.device = "/dev/ttyS1"},
    {.device = "/dev/ttyS2"},
    {.device = "/dev/ttyS3"},
};

#define N_GETTY (sizeof(getties) / sizeof(struct getty))

static void spawn_getty(struct getty *gty) {
    time_t now = time(NULL);
    if (now - gty->last_spawn < 3) {
        sleep(3);
    }

    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/example/getty.exe", "getty", gty->device, NULL);
        Exit();
    } else if (pid > 0) {
        gty->pid = pid;
        gty->last_spawn = time(NULL);
        printf("[init] getty %s\n", gty->device);
    }
}

int tty_check(const char *device)
{
    if (!strcmp(device, "/dev/tty1"))
        return 1;

    int fd = open(device, O_RDWR);
    if (-1 == fd)
        return 1;
    close(fd);
    return 0;
}

void do_getty()
{
    for (int i = 0; i < N_GETTY; i++) {
        struct getty *gty = getties + i;
        gty->pid = 0;
        gty->last_spawn = 0;

        if (!tty_check(gty->device))
            spawn_getty(gty);
    }

    while (1) {
        pause();

        if (sigchld) {
            sigchld = 0;

            int status;
            pid_t pid;
            // respawn getty and reap zombies
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                for (int i = 0; i < N_GETTY; i++) {
                    if (getties[i].pid == pid) {
                        spawn_getty(getties + i);
                        break;
                    }
                }
            }
        }

        if (sigint) {
            // TODO: reboot
        }

        if (sigterm) {
            // TODO: shutdown
        }
    }
}

void run_tests()
{
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
#ifdef _MUSL
    RunL("/bin/test/time.exe", NULL);
#endif
}

int main(int argc, char *argv[], char *envp[])
{
    if (0 != open("/dev/console", O_RDWR))
        exit(EXIT_FAILURE);
    if (1 != dup(0))
        exit(EXIT_FAILURE);
    if (2 != dup(0))
        exit(EXIT_FAILURE);

    PrintFile("/etc/welcome.txt");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sig;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    HandleErr(sigaction(SIGCHLD, &sa, NULL));
    HandleErr(sigaction(SIGTERM, &sa, NULL));
    HandleErr(sigaction(SIGINT, &sa, NULL));

    HandleErr(signal(SIGPIPE, SIG_IGN));
    HandleErr(signal(SIGHUP, SIG_IGN));
    HandleErr(signal(SIGQUIT, SIG_IGN));

    run_tests();
    do_getty();
    return 0;
}
