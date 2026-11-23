#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "main-fn.h"

static int open_mp4(const char *path)
{
    size_t length = strlen(path);
    struct stat status;
    if (path[0] == '-' || strstr(path, "://") || length <= 4 ||
        strcasecmp(path + length - 4, ".mp4") || stat(path, &status) ||
        !S_ISREG(status.st_mode))
        return -1;

    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0 && (fstat(fd, &status) || !S_ISREG(status.st_mode))) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    int fd = argc == 2 ? open_mp4(argv[1]) : -1;
    if (fd < 0) {
        fputs("Usage: mpv FILE.mp4 (one readable local regular file)\n", stderr);
        return 2;
    }

    char source[32];
    snprintf(source, sizeof(source), "fd://%d", fd);
    char *options[] = {
        "mpv", "--no-config", "--no-autoload-files",
#if HAVE_LUA || HAVE_JAVASCRIPT || HAVE_CPLUGINS
        "--no-load-scripts",
#endif
#if HAVE_LUA
        "--no-ytdl", "--no-load-console", "--no-load-select", "--load-auto-profiles=no",
#endif
        "--no-access-references", "--no-resume-playback", "--sid=no",
        "--vo=mario", "--ao=mario", "--no-audio-fallback-to-null",
        "--framedrop=no", "--demuxer=+lavf", "--demuxer-lavf-mario-mp4=yes",
        "--", source, NULL,
    };
    int result = mpv_main(sizeof(options) / sizeof(options[0]) - 1, options);
    close(fd);
    return result;
}
