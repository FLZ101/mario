#include <signal.h>
#include <sched.h>
#include <timer.h>
#include <time.h>
#include <trap.h>
#include <misc.h>
#include <task.h>
#include <bh.h>
#include <io.h>

#include <mm/uaccess.h>

time_t boot_time_sec = 0;

#define LATCH (1193180/HZ)

volatile long jiffies = 0;

#define CMOS_READ(addr) ({ \
outb(0x70, 0x80+(addr)); \
inb(0x71); \
})

#define RTC_SECONDS		0
#define RTC_MINUTES		2
#define RTC_HOURS		4
#define RTC_DAY_OF_WEEK		6
#define RTC_DAY_OF_MONTH	7
#define RTC_MONTH		8
#define RTC_YEAR		9

#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

/* Converts Gregorian date to seconds since 1970-01-01 00:00:00.
 * Assumes input in normal date format, i.e. 1980-12-31 23:59:59
 * => year=1980, mon=12, day=31, hour=23, min=59, sec=59.
 *
 * [For the Julian calendar (which was used in Russia before 1917,
 * Britain & colonies before 1752, anywhere else before 1582,
 * and is still in use by some communities) leave out the
 * -year/100+year/400 terms, and add 10.]
 *
 * This algorithm was first published by Gauss (I think).
 *
 * WARNING: this function will overflow on 2106-02-07 06:28:16 on
 * machines were long is 32-bit! (However, as time_t is signed, we
 * will already get problems at other places on 2038-01-19 03:14:08)
 */
static inline time_t mktime(unsigned int year, unsigned int mon,
	unsigned int day, unsigned int hour,
	unsigned int min, unsigned int sec)
{
	if (0 >= (int) (mon -= 2)) {	/* 1..12 -> 11,12,1..10 */
		mon += 12;	/* Puts Feb last since it has leap day */
		year -= 1;
	}
	return (((
	    (time_t)(year/4 - year/100 + year/400 + 367*mon/12 + day) +
	      year*365 - 719499
	    )*24 + hour /* now have hours */
	   )*60 + min /* now have minutes */
	  )*60 + sec; /* finally seconds */
}

time_t get_cmos_time(void)
{
	unsigned int year, mon, day, hour, min, sec;
	do {
		sec = CMOS_READ(RTC_SECONDS);
		min = CMOS_READ(RTC_MINUTES);
		hour = CMOS_READ(RTC_HOURS);
		day = CMOS_READ(RTC_DAY_OF_MONTH);
		mon = CMOS_READ(RTC_MONTH);
		year = CMOS_READ(RTC_YEAR);
	} while (sec != CMOS_READ(RTC_SECONDS));

	BCD_TO_BIN(sec);
	BCD_TO_BIN(min);
	BCD_TO_BIN(hour);
	BCD_TO_BIN(day);
	BCD_TO_BIN(mon);
	BCD_TO_BIN(year);

	if ((year += 1900) < 1970)
		year += 100;

	return mktime(year, mon, day, hour, min, sec);
}

int sys_gettimeofday_time32(struct timeval *tv)
{
	if (!tv)
		return 0;
	int error = verify_area(VERIFY_WRITE, tv, sizeof(*tv));
	if (error)
		return error;

	put_fs_long(boot_time_sec + jiffies / HZ, &tv->tv_sec);
	put_fs_long(jiffies % HZ * 1000000 / HZ, &tv->tv_usec);
	return 0;
}

static int do_clock_gettime64(clockid_t clockid, struct timespec64 *tp)
{
	tp->tv_sec = jiffies / HZ;
	tp->tv_nsec = jiffies % HZ * 1000000000 / HZ;

	switch (clockid) {
	case CLOCK_REALTIME:
	case CLOCK_REALTIME_COARSE:
	case CLOCK_REALTIME_ALARM:
		tp->tv_sec += boot_time_sec;
		break;
	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_RAW:
	case CLOCK_MONOTONIC_COARSE:
	case CLOCK_BOOTTIME:
	case CLOCK_BOOTTIME_ALARM:
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int sys_clock_gettime64(clockid_t clockid, struct timespec64 *tp)
{
	int error = verify_area(VERIFY_WRITE, tp, sizeof(*tp));
	if (error)
		return error;

	struct timespec64 ts;
	int err = do_clock_gettime64(clockid, &ts);
	if (err)
		return err;

	memcpy_tofs(tp, &ts, sizeof(ts));
	return 0;
}

int sys_clock_gettime32(clockid_t clockid, struct timespec *tp)
{
	int error = verify_area(VERIFY_WRITE, tp, sizeof(*tp));
	if (error)
		return error;

	struct timespec64 ts;
	int err = do_clock_gettime64(clockid, &ts);
	if (err)
		return err;

	put_fs_long(ts.tv_sec, &tp->tv_sec);
	put_fs_long(ts.tv_nsec, &tp->tv_nsec);
	return 0;
}

static void PIT_bh(void *unused)
{
	run_timer_list();
}

/*
 * Initialize the 8253 Programmable Interval Timer
 */
void i8253_init(void)
{
	outb(0x43, 0x36);	/* Binary, Mode 3, LSB/MSB, Counter 0 */
	outb(0x40, LATCH & 0xff);
	outb(0x40, LATCH >> 8);
}

void time_init(void)
{
	boot_time_sec = get_cmos_time();

	i8253_init();
	enable_bh(PIT_BH);
	bh_base[PIT_BH].routine = PIT_bh;
}

void irq_PIT(struct trap_frame tr)
{
	jiffies++;

	if userland(&tr)
		++current->utime;
	else
		++current->stime;

	if (&init_task == current) {
		current->need_resched = 1;
		goto tail;
	}

	if (current->counter > 0)
		if (!--current->counter)
			current->need_resched = 1;

	if (userland(&tr)) {
		if (current->it_virt_value && !--current->it_virt_value) {
			current->it_virt_value = current->it_virt_incr;
			send_sig(SIGVTALRM, current, 1);
		}
	}

	if (current->it_prof_value && !--current->it_prof_value) {
		current->it_prof_value = current->it_prof_incr;
		send_sig(SIGPROF, current, 1);
	}

tail:
	mark_bh(PIT_BH);
}
