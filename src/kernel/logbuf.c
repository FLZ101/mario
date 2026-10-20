#include <logbuf.h>

#include <misc.h>
#include <errno.h>
#include <sched.h>
#include <time.h>

#include <lib/spinlock.h>
#include <lib/string.h>

#define LOG_RECORD_HEADER __builtin_offsetof(struct log_record, text)

static char log_buf[LOG_BUF_LEN];
static unsigned int log_head;  /* next write offset */
static unsigned int log_tail;  /* oldest complete record */
static unsigned int log_count;
static unsigned int log_used;
static uint32_t log_next_seq;

static spinlock_t logbuf_lock = SPINLOCK_UNLOCKED;
static wait_queue_t kmsg_wait;
static int logbuf_ready;
static int console_loglevel = LOG_DEBUG;

static unsigned int record_size(const struct log_record *rec)
{
	return LOG_RECORD_HEADER + rec->len + 1;
}

static void copy_to_log(unsigned int off, const void *src, unsigned int len)
{
	unsigned int first;

	off %= LOG_BUF_LEN;
	first = LOG_BUF_LEN - off;
	if (first > len)
		first = len;

	memcpy(log_buf + off, src, first);
	if (len > first)
		memcpy(log_buf, (const char *)src + first, len - first);
}

static void copy_from_log(unsigned int off, void *dst, unsigned int len)
{
	unsigned int first;

	off %= LOG_BUF_LEN;
	first = LOG_BUF_LEN - off;
	if (first > len)
		first = len;

	memcpy(dst, log_buf + off, first);
	if (len > first)
		memcpy((char *)dst + first, log_buf, len - first);
}

static void drop_oldest_locked(void)
{
	struct log_record rec;
	unsigned int size;

	if (!log_count)
		return;

	copy_from_log(log_tail, &rec, LOG_RECORD_HEADER);
	size = record_size(&rec);
	log_used -= size;
	log_tail = (log_tail + size) % LOG_BUF_LEN;
	log_count--;
}

static uint32_t oldest_seq_locked(void)
{
	struct log_record rec;

	if (!log_count)
		return log_next_seq;

	copy_from_log(log_tail, &rec, LOG_RECORD_HEADER);
	return rec.seq;
}

static int find_seq_locked(uint32_t seq, unsigned int *off)
{
	struct log_record rec;
	unsigned int cur;
	unsigned int i;

	cur = log_tail;
	for (i = 0; i < log_count; i++) {
		copy_from_log(cur, &rec, LOG_RECORD_HEADER);
		if (rec.seq == seq) {
			*off = cur;
			return 0;
		}
		cur = (cur + record_size(&rec)) % LOG_BUF_LEN;
	}

	return -1;
}

static char *append_u32(char *p, uint32_t value)
{
	char tmp[10];
	int n = 0;

	do {
		tmp[n++] = '0' + value % 10;
		value /= 10;
	} while (value);

	while (n)
		*p++ = tmp[--n];

	return p;
}

static char *append_u64(char *p, uint64_t value)
{
	char tmp[20];
	int n = 0;

	do {
		tmp[n++] = '0' + value % 10;
		value /= 10;
	} while (value);

	while (n)
		*p++ = tmp[--n];

	return p;
}

static char *append_u64_fixed(char *p, uint64_t value, int digits)
{
	char tmp[20];
	int n = 0;

	do {
		tmp[n++] = '0' + value % 10;
		value /= 10;
	} while (value);

	while (n < digits)
		tmp[n++] = '0';
	while (n)
		*p++ = tmp[--n];

	return p;
}

static size_t format_kmsg_line(const struct log_record *rec,
			       const char *text, char *line)
{
	char *p = line;

	p = append_u32(p, rec->prio);
	*p++ = ',';
	p = append_u32(p, rec->seq);
	*p++ = ',';
	p = append_u64(p, rec->usec);
	*p++ = ',';
	p = append_u32(p, rec->flags);
	*p++ = ';';
	*p++ = ' ';

	memcpy(p, text, rec->len);
	p += rec->len;
	*p++ = '\n';

	return p - line;
}

static size_t format_syslog_line(const struct log_record *rec,
				 const char *text, char *line)
{
	char *p = line;

	*p++ = '<';
	p = append_u32(p, rec->prio);
	*p++ = '[';
	p = append_u64(p, rec->usec / 1000000);
	*p++ = '.';
	p = append_u64_fixed(p, rec->usec % 1000000, 6);
	*p++ = ']';
	*p++ = ' ';

	memcpy(p, text, rec->len);
	p += rec->len;
	*p++ = '\n';

	return p - line;
}

static int read_record_locked(unsigned int off, char *buf, size_t size,
			      uint32_t *next_seq)
{
	struct log_record rec;
	char text[LOG_LINE_MAX];
	char line[LOG_LINE_MAX + 64];
	size_t total;

	copy_from_log(off, &rec, LOG_RECORD_HEADER);
	copy_from_log(off + LOG_RECORD_HEADER, text, rec.len);

	total = format_kmsg_line(&rec, text, line);
	if (total > size)
		total = size;
	memcpy(buf, line, total);

	*next_seq = rec.seq + 1;
	return total;
}

static int read_seq_locked(uint32_t seq, char *buf, size_t size,
			   uint32_t *next_seq)
{
	unsigned int off;
	uint32_t oldest;

	if (!log_count || seq == log_next_seq)
		return 0;

	oldest = oldest_seq_locked();
	if ((int32_t)(seq - oldest) < 0)
		return -EPIPE;

	if (find_seq_locked(seq, &off))
		return -EPIPE;

	return read_record_locked(off, buf, size, next_seq);
}

void logbuf_init(void)
{
	INIT_LOCK(&logbuf_lock);
	init_wait_queue(&kmsg_wait);
	console_loglevel = LOG_DEBUG;
	log_head = 0;
	log_tail = 0;
	log_count = 0;
	log_used = 0;
	log_next_seq = 0;
	logbuf_ready = 1;
}

void logbuf_store(int prio, const char *text, size_t len)
{
	struct log_record rec;
	unsigned int total;
	unsigned int text_off;
	unsigned int nul_off;
	char nul = '\0';

	if (!logbuf_ready || !text || !len)
		return;

	if (len > LOG_LINE_MAX)
		len = LOG_LINE_MAX;

	total = LOG_RECORD_HEADER + len + 1;
	if (total > LOG_BUF_LEN) {
		len = LOG_BUF_LEN - LOG_RECORD_HEADER - 1;
		total = LOG_BUF_LEN;
	}

	irq_save();
	ACQUIRE_LOCK(&logbuf_lock);

	while (log_count && LOG_BUF_LEN - log_used < total)
		drop_oldest_locked();

	rec.len = len;
	rec.prio = prio;
	rec.flags = 0;
	rec.seq = log_next_seq++;
	rec.usec = (uint64_t)jiffies * 1000000ULL / HZ;

	copy_to_log(log_head, &rec, LOG_RECORD_HEADER);
	text_off = log_head + LOG_RECORD_HEADER;
	copy_to_log(text_off, text, len);
	nul_off = text_off + len;
	copy_to_log(nul_off, &nul, 1);

	log_head = (log_head + total) % LOG_BUF_LEN;
	log_used += total;
	log_count++;

	RELEASE_LOCK(&logbuf_lock);
	irq_restore();

	wake_up_all(&kmsg_wait);
}

int logbuf_read_seq(uint32_t seq, char *buf, size_t size,
		    uint32_t *next_seq)
{
	int ret;

	irq_save();
	ACQUIRE_LOCK(&logbuf_lock);
	ret = read_seq_locked(seq, buf, size, next_seq);
	RELEASE_LOCK(&logbuf_lock);
	irq_restore();

	return ret;
}

int logbuf_read_wait(uint32_t seq, char *buf, size_t size,
		     uint32_t *next_seq, int nonblock)
{
	int ret;

	for (;;) {
		if (current->signal & ~current->blocked)
			return -ERESTARTSYS;

		irq_save();
		ACQUIRE_LOCK(&logbuf_lock);
		ret = read_seq_locked(seq, buf, size, next_seq);

		if (ret || nonblock) {
			RELEASE_LOCK(&logbuf_lock);
			irq_restore();
			return ret ? ret : -EAGAIN;
		}

		sleep_on(&kmsg_wait, TASK_INTERRUPTIBLE, &logbuf_lock);
	}
}

uint32_t logbuf_oldest_seq(void)
{
	uint32_t seq;

	irq_save();
	ACQUIRE_LOCK(&logbuf_lock);
	seq = oldest_seq_locked();
	RELEASE_LOCK(&logbuf_lock);
	irq_restore();

	return seq;
}

uint32_t logbuf_next_seq(void)
{
	uint32_t seq;

	irq_save();
	ACQUIRE_LOCK(&logbuf_lock);
	seq = log_next_seq;
	RELEASE_LOCK(&logbuf_lock);
	irq_restore();

	return seq;
}

void logbuf_clear(void)
{
	irq_save();
	ACQUIRE_LOCK(&logbuf_lock);
	log_head = 0;
	log_tail = 0;
	log_count = 0;
	log_used = 0;
	log_next_seq = 0;
	RELEASE_LOCK(&logbuf_lock);
	irq_restore();
}

size_t logbuf_formatted_syslog_len(void)
{
	struct log_record rec;
	char text[LOG_LINE_MAX];
	char line[LOG_LINE_MAX + 64];
	unsigned int off;
	unsigned int i;
	size_t total = 0;

	irq_save();
	ACQUIRE_LOCK(&logbuf_lock);

	off = log_tail;
	for (i = 0; i < log_count; i++) {
		copy_from_log(off, &rec, LOG_RECORD_HEADER);
		copy_from_log(off + LOG_RECORD_HEADER, text, rec.len);
		total += format_syslog_line(&rec, text, line);
		off = (off + record_size(&rec)) % LOG_BUF_LEN;
	}

	RELEASE_LOCK(&logbuf_lock);
	irq_restore();

	return total;
}

int logbuf_format_syslog(char *buf, size_t size, int clear)
{
	struct log_record rec;
	char text[LOG_LINE_MAX];
	char line[LOG_LINE_MAX + 64];
	unsigned int off;
	unsigned int i;
	size_t total;
	size_t copied = 0;

	irq_save();
	ACQUIRE_LOCK(&logbuf_lock);

	off = log_tail;
	for (i = 0; i < log_count && copied < size; i++) {
		copy_from_log(off, &rec, LOG_RECORD_HEADER);
		copy_from_log(off + LOG_RECORD_HEADER, text, rec.len);
		total = format_syslog_line(&rec, text, line);
		if (total > size - copied)
			total = size - copied;
		memcpy(buf + copied, line, total);
		copied += total;
		off = (off + record_size(&rec)) % LOG_BUF_LEN;
	}

	if (clear) {
		log_head = 0;
		log_tail = 0;
		log_count = 0;
		log_used = 0;
		log_next_seq = 0;
	}

	RELEASE_LOCK(&logbuf_lock);
	irq_restore();

	return copied;
}

int logbuf_console_loglevel(int set, int level)
{
	int ret;

	irq_save();
	ACQUIRE_LOCK(&logbuf_lock);

	if (set) {
		if (level < 1 || level > 8) {
			ret = console_loglevel;
		} else {
			console_loglevel = level;
			ret = 0;
		}
	} else {
		ret = console_loglevel;
	}

	RELEASE_LOCK(&logbuf_lock);
	irq_restore();

	return ret;
}
