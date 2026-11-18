#include <fs/chrdev.h>
#include <fs/fs.h>
#include <fs/psaux.h>
#include <fs/vmmouse.h>
#include <io.h>
#include <lib/spinlock.h>
#include <time.h>
#include <uapi/mario/input.h>

/* Keep this distinct from other per-description device ownership bits. */
#define FMODE_PSAUX_OWNER 0x2000
#define PSAUX_EVENT_COUNT 256

static spinlock_t psaux_lock = SPINLOCK_UNLOCKED;
static wait_queue_t psaux_wait;
static unsigned int psaux_owner_refs;
static int psaux_available;
static int psaux_absolute;
static struct mario_psaux_event psaux_events[PSAUX_EVENT_COUNT];
static unsigned int psaux_event_head;
static unsigned int psaux_event_tail;
static unsigned int psaux_event_count;
static uint8_t psaux_packet[3];
static unsigned int psaux_packet_count;
static int psaux_packet_lost;

static int psaux_read(struct inode *i, struct file *f, char *buf, int count);
static int psaux_poll(struct file *f, struct poll_context *ctx);

static void psaux_reset_locked(void)
{
	psaux_event_head = 0;
	psaux_event_tail = 0;
	psaux_event_count = 0;
	psaux_packet_count = 0;
	psaux_packet_lost = 0;
}

static int psaux_open(struct inode *i, struct file *f)
{
	unsigned long flags;

	(void)i;
	if (f->f_mode & FMODE_PSAUX_OWNER) {
		spin_lock_irqsave(&psaux_lock, flags);
		psaux_owner_refs++;
		spin_unlock_irqrestore(&psaux_lock, flags);
		return 0;
	}
	if (f->f_mode & 2)
		return -EACCES;
	spin_lock_irqsave(&psaux_lock, flags);
	if (psaux_owner_refs) {
		spin_unlock_irqrestore(&psaux_lock, flags);
		return -EBUSY;
	}
	psaux_owner_refs = 1;
	psaux_reset_locked();
	f->f_mode |= FMODE_PSAUX_OWNER;
	spin_unlock_irqrestore(&psaux_lock, flags);
	return 0;
}

static void psaux_release(struct inode *i, struct file *f)
{
	unsigned long flags;

	(void)i;
	if (!(f->f_mode & FMODE_PSAUX_OWNER))
		return;
	spin_lock_irqsave(&psaux_lock, flags);
	assert(psaux_owner_refs);
	psaux_owner_refs--;
	if (!psaux_owner_refs)
		psaux_reset_locked();
	spin_unlock_irqrestore(&psaux_lock, flags);
}

static int psaux_read(struct inode *i, struct file *f, char *buf, int count)
{
	unsigned long flags;
	unsigned int events;
	int bytes;

	(void)i;
	if (!count)
		return 0;
	if (count < sizeof(psaux_events[0]) || count % sizeof(psaux_events[0]))
		return -EINVAL;
	for (;;) {
		spin_lock_irqsave(&psaux_lock, flags);
		if (!psaux_available) {
			spin_unlock_irqrestore(&psaux_lock, flags);
			return -EIO;
		}
		if (psaux_event_count) {
			events = count / sizeof(psaux_events[0]);
			if (events > psaux_event_count)
				events = psaux_event_count;
			bytes = events * sizeof(psaux_events[0]);
			if (verify_area(VERIFY_WRITE, buf, bytes)) {
				spin_unlock_irqrestore(&psaux_lock, flags);
				return -EFAULT;
			}
			for (unsigned int index = 0; index < events; index++) {
				memcpy_tofs(buf + index * sizeof(psaux_events[0]),
					     &psaux_events[psaux_event_tail],
					     sizeof(psaux_events[0]));
				psaux_event_tail =
					(psaux_event_tail + 1) % PSAUX_EVENT_COUNT;
			}
			psaux_event_count -= events;
			spin_unlock_irqrestore(&psaux_lock, flags);
			return bytes;
		}
		spin_unlock_irqrestore(&psaux_lock, flags);
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (current->signal & ~current->blocked)
			return -ERESTARTSYS;
		sleep_on(&psaux_wait, TASK_INTERRUPTIBLE, NULL);
	}
}

static int psaux_poll(struct file *f, struct poll_context *ctx)
{
	unsigned long flags;
	int mask = 0;

	poll_wait(f, &psaux_wait, ctx);
	spin_lock_irqsave(&psaux_lock, flags);
	if (!psaux_available)
		mask = POLLHUP;
	else if (psaux_event_count)
		mask = POLLIN;
	spin_unlock_irqrestore(&psaux_lock, flags);
	return mask;
}

static struct file_operations psaux_fops = {
	.open = psaux_open,
	.release = psaux_release,
	.read = psaux_read,
	.poll = psaux_poll,
};

void psaux_controller_ready(void)
{
	unsigned long flags;

	spin_lock_irqsave(&psaux_lock, flags);
	psaux_absolute = vmmouse_init();
	psaux_available = 1;
	psaux_reset_locked();
	spin_unlock_irqrestore(&psaux_lock, flags);
	wake_up_all(&psaux_wait);
}

void psaux_controller_failed(void)
{
	unsigned long flags;

	spin_lock_irqsave(&psaux_lock, flags);
	psaux_available = 0;
	psaux_reset_locked();
	spin_unlock_irqrestore(&psaux_lock, flags);
	wake_up_all(&psaux_wait);
}

static int psaux_enqueue_locked(struct mario_psaux_event event)
{
	if (!psaux_owner_refs)
		return 0;
	event.ticks = jiffies;
	if (psaux_packet_lost)
		event.flags |= PSAUX_EVENT_OVERFLOW;
	psaux_packet_lost = 0;
	if (psaux_event_count == PSAUX_EVENT_COUNT) {
		psaux_event_tail = (psaux_event_tail + 1) % PSAUX_EVENT_COUNT;
		psaux_event_count--;
		event.flags |= PSAUX_EVENT_OVERFLOW;
	}
	psaux_events[psaux_event_head] = event;
	psaux_event_head = (psaux_event_head + 1) % PSAUX_EVENT_COUNT;
	psaux_event_count++;
	return 1;
}

void psaux_receive_byte(uint8_t byte)
{
	struct mario_psaux_event event;
	unsigned long flags;
	int wake = 0;

	spin_lock_irqsave(&psaux_lock, flags);
	if (!psaux_available || (!psaux_owner_refs && !psaux_absolute)) {
		psaux_packet_count = 0;
		spin_unlock_irqrestore(&psaux_lock, flags);
		return;
	}
	/* Bit 3 marks the first byte of every basic PS/2 mouse packet. */
	if (!psaux_packet_count && !(byte & 0x08)) {
		psaux_packet_lost = 1;
		spin_unlock_irqrestore(&psaux_lock, flags);
		return;
	}
	psaux_packet[psaux_packet_count++] = byte;
	if (psaux_packet_count != sizeof(psaux_packet)) {
		spin_unlock_irqrestore(&psaux_lock, flags);
		return;
	}
	psaux_packet_count = 0;
	if (psaux_absolute) {
		for (unsigned int index = 0; index < PSAUX_EVENT_COUNT; index++) {
			int result = vmmouse_read_event(&event);

			if (result < 0) {
				psaux_absolute = 0;
				psaux_packet_lost = 1;
				break;
			}
			if (!result)
				break;
			wake |= psaux_enqueue_locked(event);
		}
		goto done;
	}
	/* Overflowed deltas are wrapped by hardware and cannot be recovered. */
	if (psaux_packet[0] & 0xc0) {
		psaux_packet_lost = 1;
		spin_unlock_irqrestore(&psaux_lock, flags);
		return;
	}
	event = (struct mario_psaux_event) {
		.dx = (int8_t)psaux_packet[1],
		.dy = (int8_t)psaux_packet[2],
		.buttons = psaux_packet[0] & 0x07,
	};
	/* Retain recent input and mark the record after a FIFO overrun. */
	wake = psaux_enqueue_locked(event);
done:
	spin_unlock_irqrestore(&psaux_lock, flags);
	if (wake)
		wake_up_interruptible(&psaux_wait);
}

void irq_PS2_aux(void)
{
	unsigned char status = inb(0x64);

	if ((status & 0x21) == 0x21)
		psaux_receive_byte(inb(0x60));
}

void psaux_init(void)
{
	init_wait_queue(&psaux_wait);
	register_chrdev(PSAUX_MAJOR, &psaux_fops);
}
