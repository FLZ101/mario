#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <syscall.h>
#include <sys/stat.h>

#define SYS_getcwd_ SYS_getcwd

static inline _syscall2(int,getcwd_,char *,buf, size_t, size)

char *getcwd(char *buf, size_t size)
{
	char tmp[buf ? 1 : PATH_MAX];
	if (!buf) {
		buf = tmp;
		size = sizeof tmp;
	} else if (!size) {
		errno = EINVAL;
		return 0;
	}
	long ret = getcwd_(buf, size);
	if (ret < 0)
		return 0;
	if (ret == 0 || buf[0] != '/') {
		errno = ENOENT;
		return 0;
	}
	return buf == tmp ? strdup(buf) : buf;
}
