#include <logbuf.h>

#include <errno.h>
#include <mm/uaccess.h>

#define SYSLOG_ACTION_READ_ALL        3
#define SYSLOG_ACTION_READ_CLEAR      4
#define SYSLOG_ACTION_CLEAR           5
#define SYSLOG_ACTION_CONSOLE_LEVEL   8
#define SYSLOG_ACTION_SIZE_UNREAD     9
#define SYSLOG_ACTION_SIZE_BUFFER     10

#define SYSLOG_COPY_MAX (64 * 1024)

static char syslog_copy_buf[SYSLOG_COPY_MAX];

int sys_syslog(int type, char *buf, int len)
{
	int err;
	int ret;

	switch (type) {
	case SYSLOG_ACTION_READ_ALL:
	case SYSLOG_ACTION_READ_CLEAR:
		if (!buf)
			return -EINVAL;
		if (len < 0)
			return -EINVAL;
		if (len > SYSLOG_COPY_MAX)
			len = SYSLOG_COPY_MAX;

		err = verify_area(VERIFY_WRITE, buf, len);
		if (err)
			return err;

		ret = logbuf_format_syslog(syslog_copy_buf, len,
					   type == SYSLOG_ACTION_READ_CLEAR);
		if (ret)
			memcpy_tofs(buf, syslog_copy_buf, ret);
		return ret;

	case SYSLOG_ACTION_CLEAR:
		logbuf_clear();
		return 0;

	case SYSLOG_ACTION_CONSOLE_LEVEL:
		return logbuf_console_loglevel(1, len);

	case SYSLOG_ACTION_SIZE_UNREAD:
	case SYSLOG_ACTION_SIZE_BUFFER:
		return logbuf_formatted_syslog_len();

	default:
		return -EINVAL;
	}
}
