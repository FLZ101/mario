#include <fs/fs.h>
#include <hda.h>
#include <uapi/mario/pcm.h>

#define PCM_CAPACITY (HDA_PERIODS * HDA_PERIOD_FRAMES)
#define PCM_REFILL_GUARD 96
#define PCM_POLL_LIMIT (35 * HDA_WALL_TICKS_PER_MS)
#define PCM_STALL_LIMIT (150 * HDA_WALL_TICKS_PER_MS)
#define PCM_TAIL (60 * HDA_WALL_TICKS_PER_MS)
#define PCM_CLOCKS_PER_FRAME (HDA_WALL_TICKS_PER_MS / (MARIO_PCM_RATE / 1000))

static spinlock_t pcm_lock = SPINLOCK_UNLOCKED;
static wait_queue_t pcm_wait;
static struct file *pcm_owner;
static enum mario_pcm_state pcm_state;
static int16_t fifo[PCM_CAPACITY][2];
static unsigned int fifo_read, fifo_write, fifo_used;
static unsigned int valid[HDA_PERIODS];
static unsigned int position, dma_pending;
static uint64_t consumed;
static unsigned int underruns;
static uint32_t last_poll, last_progress, tail_start;
static int in_tail, explicit_drain;
static int defer_poll;

static unsigned int pcm_min(unsigned int left, unsigned int right)
{
	return left < right ? left : right;
}

static unsigned int pcm_room(void)
{
	unsigned int reserved = fifo_used;
	if (pcm_state == MARIO_PCM_FAILED || pcm_state == MARIO_PCM_DRAINING)
		return 0;
	for (unsigned int period = 0; period < HDA_PERIODS; period++)
		if (valid[period])
			reserved += HDA_PERIOD_FRAMES;
	return reserved < PCM_CAPACITY ? PCM_CAPACITY - reserved : 0;
}

static void pcm_stage(unsigned int slot)
{
	unsigned int frames = pcm_min(fifo_used, HDA_PERIOD_FRAMES);
	unsigned int first = pcm_min(frames, PCM_CAPACITY - fifo_read);
	int16_t *samples = hda_pcm_period(slot);

	memcpy(samples, fifo[fifo_read], first * MARIO_PCM_FRAME_BYTES);
	memcpy(samples + first * 2, fifo, (frames - first) * MARIO_PCM_FRAME_BYTES);
	fifo_read = (fifo_read + frames) % PCM_CAPACITY;
	fifo_used -= frames;
	valid[slot] = frames;
	dma_pending += frames;
	asm volatile("" ::: "memory");
}

static int pcm_active(void)
{
	return pcm_state == MARIO_PCM_RUNNING || pcm_state == MARIO_PCM_PAUSING ||
	       pcm_state == MARIO_PCM_DRAINING;
}

static int pcm_stop(void)
{
	if (hda_pcm_stop()) {
		pcm_state = MARIO_PCM_FAILED;
		return -EIO;
	}
	for (unsigned int slot = 0; slot < HDA_PERIODS; slot++) {
		memset(hda_pcm_period(slot), 0, HDA_PERIOD_BYTES);
		valid[slot] = 0;
	}
	position = dma_pending = 0;
	in_tail = explicit_drain = 0;
	defer_poll = 0;
	return 0;
}

static int pcm_reset(void)
{
	if (pcm_stop())
		return -EIO;
	fifo_read = fifo_write = fifo_used = 0;
	consumed = underruns = 0;
	pcm_state = MARIO_PCM_STOPPED;
	return 0;
}

static int pcm_start(void)
{
	if (pcm_state == MARIO_PCM_FAILED)
		return -EIO;
	if (pcm_active())
		return -EBUSY;
	if (!fifo_used)
		return -EAGAIN;
	for (unsigned int slot = 0; slot < HDA_PERIODS && fifo_used; slot++)
		pcm_stage(slot);
	if (hda_pcm_start()) {
		pcm_state = MARIO_PCM_FAILED;
		return -EIO;
	}
	last_poll = last_progress = hda_pcm_clock();
	defer_poll = hda_pcm_defer_first_poll();
	pcm_state = MARIO_PCM_RUNNING;
	return 0;
}

static void pcm_lost_position(void)
{
	if (pcm_stop())
		return;
	fifo_read = fifo_write = fifo_used = 0;
	underruns++;
	pcm_state = MARIO_PCM_UNDERRUN;
}

static void pcm_advance(unsigned int next)
{
	unsigned int remaining = (PCM_CAPACITY + next - position) % PCM_CAPACITY;
	while (remaining) {
		unsigned int slot = position / HDA_PERIOD_FRAMES;
		unsigned int offset = position % HDA_PERIOD_FRAMES;
		unsigned int step = pcm_min(remaining, HDA_PERIOD_FRAMES - offset);
		unsigned int played = offset < valid[slot] ?
			pcm_min(step, valid[slot] - offset) : 0;
		consumed += played;
		dma_pending -= played;
		if (offset + step == HDA_PERIOD_FRAMES && valid[slot]) {
			memset(hda_pcm_period(slot), 0, HDA_PERIOD_BYTES);
			valid[slot] = 0;
		}
		position = (position + step) % PCM_CAPACITY;
		remaining -= step;
	}
}

static void pcm_refill(void)
{
	unsigned int playing = position / HDA_PERIOD_FRAMES;
	unsigned int until_next = HDA_PERIOD_FRAMES - position % HDA_PERIOD_FRAMES;
	for (unsigned int ahead = 1; ahead < HDA_PERIODS && fifo_used; ahead++) {
		unsigned int slot = (playing + ahead) % HDA_PERIODS;
		if (valid[slot])
			continue;
		if (ahead == 1 && until_next < PCM_REFILL_GUARD)
			break;
		if (fifo_used < HDA_PERIOD_FRAMES && !explicit_drain && dma_pending)
			break;
		pcm_stage(slot);
	}
}

static void pcm_tick(void)
{
	uint32_t clock = hda_pcm_clock();
	uint32_t elapsed = clock - last_poll;
	if (defer_poll && elapsed < PCM_POLL_LIMIT)
		return;
	int next = hda_pcm_position();
	if (next < 0 || elapsed >= PCM_POLL_LIMIT ||
	    clock - last_progress >= PCM_STALL_LIMIT) {
		pr_warn("PCM: DMA poll lost position (frame %d, poll %u ms, stall %u ms)\n",
			next, elapsed / HDA_WALL_TICKS_PER_MS,
			(clock - last_progress) / HDA_WALL_TICKS_PER_MS);
		pcm_lost_position();
		return;
	}
	unsigned int advance = (PCM_CAPACITY + next - position) % PCM_CAPACITY;
	if (advance > 2 * elapsed / PCM_CLOCKS_PER_FRAME + 2 * HDA_PERIOD_FRAMES) {
		pr_warn("PCM: implausible DMA advance of %u frames in %u ms\n",
			advance, elapsed / HDA_WALL_TICKS_PER_MS);
		pcm_lost_position();
		return;
	}
	last_poll = clock;
	if ((unsigned int)next != position) {
		last_progress = clock;
		pcm_advance(next);
	}
	if (pcm_state == MARIO_PCM_RUNNING || explicit_drain)
		pcm_refill();
	if (dma_pending || (fifo_used && pcm_state != MARIO_PCM_PAUSING))
		return;
	if (pcm_state == MARIO_PCM_RUNNING)
		pcm_state = MARIO_PCM_DRAINING;
	if (!in_tail) {
		in_tail = 1;
		tail_start = clock;
		return;
	}
	if (clock - tail_start >= PCM_TAIL) {
		enum mario_pcm_state state = pcm_state == MARIO_PCM_PAUSING ?
			MARIO_PCM_PAUSED : explicit_drain ? MARIO_PCM_STOPPED :
			MARIO_PCM_UNDERRUN;
		if (!pcm_stop()) {
			pcm_state = state;
			if (state == MARIO_PCM_UNDERRUN)
				underruns++;
		}
	}
}

void pcm_service(void)
{
	unsigned long flags;
	int notify = 0;
	if (!hda_pcm_present())
		return;
	spin_lock_irqsave(&pcm_lock, flags);
	if (pcm_active()) {
		unsigned int room = pcm_room();
		enum mario_pcm_state state = pcm_state;
		pcm_tick();
		notify = pcm_room() > room || pcm_state != state;
	}
	spin_unlock_irqrestore(&pcm_lock, flags);
	if (notify)
		wake_up_interruptible(&pcm_wait);
}

static int pcm_open(struct inode *inode, struct file *file)
{
	unsigned long flags;
	int result = 0;
	if (MINOR(inode->i_rdev) != PCM_MINOR || !hda_pcm_present())
		return -ENODEV;
	if (!(file->f_mode & 2))
		return -EACCES;
	spin_lock_irqsave(&pcm_lock, flags);
	if (pcm_owner)
		result = -EBUSY;
	else if (pcm_state == MARIO_PCM_FAILED)
		result = -EIO;
	else
		pcm_owner = file;
	spin_unlock_irqrestore(&pcm_lock, flags);
	return result;
}

static void pcm_release(struct inode *inode, struct file *file)
{
	unsigned long flags;
	spin_lock_irqsave(&pcm_lock, flags);
	if (pcm_owner == file) {
		pcm_reset();
		pcm_owner = NULL;
	}
	spin_unlock_irqrestore(&pcm_lock, flags);
	wake_up_interruptible(&pcm_wait);
}

static void pcm_sleep(unsigned long flags)
{
	wait_queue_node_t waiter;
	init_wait_queue_node(&waiter, current);
	current->state = TASK_INTERRUPTIBLE;
	in_wait_queue(&pcm_wait, &waiter);
	if (current->signal & ~current->blocked)
		current->state = TASK_RUNNING;
	spin_unlock_irqrestore(&pcm_lock, flags);
	schedule();
	out_wait_queue(&pcm_wait, &waiter);
}

static int pcm_write(struct inode *inode, struct file *file, char *buffer, int bytes)
{
	int16_t samples[256][2];
	unsigned int written = 0;
	if (bytes < 0 || bytes % MARIO_PCM_FRAME_BYTES)
		return -EINVAL;
	while (written < (unsigned int)bytes) {
		unsigned int requested = pcm_min((bytes - written) / MARIO_PCM_FRAME_BYTES, 256);
		unsigned long flags;
		int result;
		memcpy_fromfs(samples, buffer + written, requested * MARIO_PCM_FRAME_BYTES);
		for (;;) {
			unsigned int frames;
			spin_lock_irqsave(&pcm_lock, flags);
			frames = pcm_min(requested, pcm_room());
			if (frames) {
				unsigned int first = pcm_min(frames, PCM_CAPACITY - fifo_write);
				memcpy(fifo[fifo_write], samples, first * MARIO_PCM_FRAME_BYTES);
				memcpy(fifo, samples + first, (frames - first) * MARIO_PCM_FRAME_BYTES);
				fifo_write = (fifo_write + frames) % PCM_CAPACITY;
				fifo_used += frames;
				written += frames * MARIO_PCM_FRAME_BYTES;
				spin_unlock_irqrestore(&pcm_lock, flags);
				break;
			}
			if (pcm_state == MARIO_PCM_FAILED)
				result = -EIO;
			else if (pcm_state == MARIO_PCM_DRAINING)
				result = -EBUSY;
			else if (written || (file->f_flags & O_NONBLOCK))
				result = -EAGAIN;
			else if (current->signal & ~current->blocked)
				result = -ERESTARTSYS;
			else {
				pcm_sleep(flags);
				continue;
			}
			spin_unlock_irqrestore(&pcm_lock, flags);
			return written ? (int)written : result;
		}
	}
	return written;
}

static int pcm_poll(struct file *file, struct poll_context *context)
{
	unsigned long flags;
	int mask = 0;
	poll_wait(file, &pcm_wait, context);
	spin_lock_irqsave(&pcm_lock, flags);
	if (pcm_room())
		mask |= POLLOUT;
	if (pcm_state == MARIO_PCM_FAILED)
		mask |= POLLERR;
	spin_unlock_irqrestore(&pcm_lock, flags);
	return mask;
}

static int pcm_ioctl(struct inode *inode, struct file *file, unsigned int command,
		     unsigned long argument)
{
	unsigned long flags;
	int result = 0;
	struct mario_pcm_status status;
	if (command == MARIO_PCM_STATUS &&
	    verify_area(VERIFY_WRITE, (void *)argument, sizeof(status)))
		return -EFAULT;
	spin_lock_irqsave(&pcm_lock, flags);
	switch (command) {
	case MARIO_PCM_STATUS:
		status = (struct mario_pcm_status){
			.version = MARIO_PCM_ABI_VERSION,
			.state = pcm_state,
			.capacity_frames = PCM_CAPACITY,
			.writable_frames = pcm_room(),
			.pending_frames = fifo_used + dma_pending,
			.underruns = underruns,
			.consumed_frames = consumed,
		};
		break;
	case MARIO_PCM_START:
		result = pcm_state == MARIO_PCM_PAUSED ? -EINVAL : pcm_start();
		break;
	case MARIO_PCM_PAUSE:
		if (pcm_state == MARIO_PCM_RUNNING) {
			pcm_state = MARIO_PCM_PAUSING;
			in_tail = 0;
		} else {
			result = -EINVAL;
		}
		break;
	case MARIO_PCM_RESUME:
		result = pcm_state == MARIO_PCM_PAUSED ? pcm_start() : -EINVAL;
		break;
	case MARIO_PCM_RESET:
		result = pcm_reset();
		break;
	case MARIO_PCM_DRAIN:
		if (pcm_state == MARIO_PCM_RUNNING || pcm_state == MARIO_PCM_DRAINING) {
			pcm_state = MARIO_PCM_DRAINING;
			explicit_drain = 1;
		} else {
			result = -EINVAL;
		}
		break;
	default:
		result = -ENOTTY;
	}
	spin_unlock_irqrestore(&pcm_lock, flags);
	if (command == MARIO_PCM_STATUS)
		memcpy_tofs((void *)argument, &status, sizeof(status));
	else if (!result)
		wake_up_interruptible(&pcm_wait);
	return result;
}

static struct file_operations pcm_fops = {
	.open = pcm_open,
	.release = pcm_release,
	.write = pcm_write,
	.ioctl = pcm_ioctl,
	.poll = pcm_poll,
	.share_on_fork = 1,
};

void pcm_init(void)
{
	init_wait_queue(&pcm_wait);
	register_chrdev(PCM_MAJOR, &pcm_fops);
}
