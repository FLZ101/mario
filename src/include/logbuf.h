#ifndef _LOGBUF_H
#define _LOGBUF_H

#include <types.h>

#define LOG_BUF_LEN    (16 * 1024)
#define LOG_LINE_MAX   1024

#define LOG_EMERG     0
#define LOG_ALERT     1
#define LOG_CRIT      2
#define LOG_ERR       3
#define LOG_WARNING   4
#define LOG_NOTICE    5
#define LOG_INFO      6
#define LOG_DEBUG     7

#define LOG_KERN      (0 << 3)

struct log_record {
	uint32_t len;      /* text length, excluding NUL */
	uint16_t prio;     /* LOG_* priority/facility, default LOG_INFO */
	uint16_t flags;
	uint32_t seq;
	uint64_t usec;     /* boot-time monotonic microseconds */
	char text[];
};

void logbuf_init(void);
void logbuf_store(int prio, const char *text, size_t len);

int logbuf_read_seq(uint32_t seq, char *buf, size_t size,
		    uint32_t *next_seq);
int logbuf_read_wait(uint32_t seq, char *buf, size_t size,
		     uint32_t *next_seq, int nonblock);
uint32_t logbuf_oldest_seq(void);
uint32_t logbuf_next_seq(void);

size_t logbuf_formatted_syslog_len(void);
int logbuf_format_syslog(char *buf, size_t size, int clear);
void logbuf_clear(void);
int logbuf_console_loglevel(int set, int level);

#endif /* _LOGBUF_H */
