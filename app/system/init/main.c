#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <sys/reboot.h>

#include <app/util.h>
#include <app/gui-session.h>

static volatile sig_atomic_t sigchld = 0;
static volatile sig_atomic_t sigint = 0;
static volatile sig_atomic_t sigterm = 0;
static volatile sig_atomic_t sigusr1 = 0;
static volatile sig_atomic_t sigusr2 = 0;

/* The compositor is a service only on framebuffer-capable boots. */
static struct service {
    const char *program;
    pid_t pid;
    time_t last_spawn;
    int enabled;
} window_manager = {
    .program = "/usr/bin/mario-wm",
};

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
    case SIGUSR1:
        sigusr1 = 1;
        break;
    case SIGUSR2:
        sigusr2 = 1;
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
        execl("/sbin/getty.exe", "getty", gty->device, NULL);
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

static int framebuffer_available(void)
{
    int fd = open("/dev/fb0", O_RDONLY);

    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static void spawn_window_manager(void)
{
    time_t now = time(NULL);
    pid_t pid;

    if (now - window_manager.last_spawn < 3)
        sleep(3);

    pid = fork();
    if (pid == 0) {
        int log = open("/tmp/mario-wm.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);

        /* Keep the GUI service independent from init's controlling console. */
        if (setsid() < 0)
            _exit(EXIT_FAILURE);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
            if (log > STDERR_FILENO)
                close(log);
        }
        execl(window_manager.program, "mario-wm", NULL);
        _exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        window_manager.pid = pid;
        window_manager.last_spawn = time(NULL);
        printf("[init] mario-wm\n");
    }
}

static void start_window_manager(void)
{
    /* Text-only boots deliberately retain the original getty-only session. */
    if (!framebuffer_available() || access(window_manager.program, X_OK) < 0)
        return;
    window_manager.enabled = 1;
    spawn_window_manager();
}

static int window_manager_stopped(int status)
{
    /* mario-wm handles SIGTERM itself and reports its clean shutdown as zero. */
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int run_initrc(void)
{
    pid_t pid;
    int status;

    /* Run the early-userspace setup script synchronously. */
    pid = fork();
    if (pid == 0) {
        execl("/bin/oksh", "oksh", "/etc/initrc", NULL);
        _exit(EXIT_FAILURE);
    }
    if (pid < 0)
        return -1;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void init_terminate_children(void)
{
    /* Ask every process except init and the current task to exit. */
    kill(-1, SIGTERM);
    sleep(1);

    /* Reap cleanly exited children, then force the stragglers. */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    kill(-1, SIGKILL);
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static void init_reboot(int command)
{
    const char *action = command == RB_AUTOBOOT ? "reboot" :
        command == RB_HALT_SYSTEM ? "halt" : "power-off";

    printf("[init] %s\n", action);
    fflush(stdout);
    init_terminate_children();
    reboot(command);

    /* reboot() is not expected to return; stay alive if it does. */
    for (;;)
        pause();
}

void do_getty()
{
    printf("[init] Press F1-6 to switch to /dev/tty1-6\n");

    for (int i = 0; i < N_GETTY; i++) {
        struct getty *gty = getties + i;
        gty->pid = 0;
        gty->last_spawn = 0;

        if (!tty_check(gty->device))
            spawn_getty(gty);
    }

    while (1) {
        pause();

        if (sigint) {
            sigint = 0;
            init_reboot(RB_AUTOBOOT);
        }
        if (sigterm) {
            sigterm = 0;
            init_reboot(RB_AUTOBOOT);
        }
        if (sigusr1) {
            sigusr1 = 0;
            init_reboot(RB_HALT_SYSTEM);
        }
        if (sigusr2) {
            sigusr2 = 0;
            init_reboot(RB_POWER_OFF);
        }

        if (sigchld) {
            sigchld = 0;

            int status;
            pid_t pid;
            // respawn services and reap zombies
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                if (window_manager.enabled && window_manager.pid == pid) {
                    window_manager.pid = 0;
                    /* Preserve a cleanly stopped GUI session until reboot. */
                    if (!window_manager_stopped(status)) {
                        gui_session_remove_status_files();
                        spawn_window_manager();
                    }
                    continue;
                }
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

int main(int argc, char *argv[], char *envp[])
{
    if (0 != open("/dev/console", O_RDWR))
        exit(EXIT_FAILURE);
    if (1 != dup(0))
        exit(EXIT_FAILURE);
    if (2 != dup(0))
        exit(EXIT_FAILURE);

    if (1 != getpid()) {
        fprintf(stderr, "init must be PID 1\n");
        exit(EXIT_FAILURE);
    }

    if (0 != mount("proc", "/proc", "proc", 0, NULL)) {
        perror("mount proc");
        exit(EXIT_FAILURE);
    }
    if (0 != mount("sysfs", "/sys", "sysfs", 0, NULL)) {
        perror("mount sysfs");
        exit(EXIT_FAILURE);
    }
    if (0 != mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL)) {
        perror("mount debugfs");
        exit(EXIT_FAILURE);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sig;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    HandleErr(sigaction(SIGCHLD, &sa, NULL));
    HandleErr(sigaction(SIGTERM, &sa, NULL));
    HandleErr(sigaction(SIGINT, &sa, NULL));
    HandleErr(sigaction(SIGUSR1, &sa, NULL));
    HandleErr(sigaction(SIGUSR2, &sa, NULL));

    HandleErr(signal(SIGPIPE, SIG_IGN));
    HandleErr(signal(SIGHUP, SIG_IGN));
    HandleErr(signal(SIGQUIT, SIG_IGN));

    /* Finish early userspace setup before the first login.  The script
     * mounts /usr, then brings up networking and image services such as
     * sshd.  Failing the script is treated like the previous hard-coded
     * mount failure: the guest has no usable /usr, so stop early.
     */
    if (run_initrc() != 0) {
        fprintf(stderr, "run /etc/initrc failed\n");
        exit(EXIT_FAILURE);
    }
    start_window_manager();
    do_getty();

    while (1)
        pause();
    return 0;
}
