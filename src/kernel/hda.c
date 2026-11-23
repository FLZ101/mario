#include <hda.h>
#include <lib/stddef.h>
#include <lib/string.h>
#include <misc.h>
#include <mm/ioremap.h>
#include <mm/page_alloc.h>
#include <multiboot.h>
#include <pci.h>
#include <time.h>

#define HDA_CLASS 0x04
#define HDA_SUBCLASS 0x03
#define HDA_GCAP 0x00
#define HDA_VERSION_MAJOR 0x03
#define HDA_GCTL 0x08
#define HDA_WAKEEN 0x0c
#define HDA_STATESTS 0x0e
#define HDA_INTCTL 0x20
#define HDA_WALLCLK 0x30
#define HDA_CORBLBASE 0x40
#define HDA_CORBUBASE 0x44
#define HDA_CORBWP 0x48
#define HDA_CORBRP 0x4a
#define HDA_CORBCTL 0x4c
#define HDA_CORBSTS 0x4d
#define HDA_CORBSIZE 0x4e
#define HDA_RIRBLBASE 0x50
#define HDA_RIRBUBASE 0x54
#define HDA_RIRBWP 0x58
#define HDA_RINTCNT 0x5a
#define HDA_RIRBCTL 0x5c
#define HDA_RIRBSTS 0x5d
#define HDA_RIRBSIZE 0x5e
#define HDA_DPLBASE 0x70
#define HDA_DPUBASE 0x74
#define HDA_STREAM_BASE 0x80
#define HDA_STREAM_STRIDE 0x20

#define SD_CTL 0x00
#define SD_CTL_TAG 0x02
#define SD_STS 0x03
#define SD_LPIB 0x04
#define SD_CBL 0x08
#define SD_LVI 0x0c
#define SD_FMT 0x12
#define SD_BDPL 0x18
#define SD_BDPU 0x1c
#define SD_RESET 0x01
#define SD_RUN 0x02
#define SD_ERROR 0x18

#define HDA_RING_DMA 0x02
#define HDA_RING_RESET 0x8000
#define HDA_RIRB_IRQ 0x01
#define HDA_RIRB_OVERRUN 0x04
#define HDA_RING_256 0x02
#define HDA_CAP_RING(size) (1 << (4 + (size)))
#define HDA_RIRB_UNSOLICITED (1 << 4)
#define HDA_COMMAND_INT_DISABLE (1 << 10)
#define HDA_TIMEOUT_TICKS (HZ / 10)

struct hda_response {
	uint32_t value;
	uint32_t info;
};

struct hda_descriptor {
	uint32_t low;
	uint32_t high;
	uint32_t bytes;
	uint32_t flags;
};

struct hda_tables {
	uint32_t corb[256];
	struct hda_response rirb[256];
	struct hda_descriptor bdl[HDA_PERIODS];
};

_Static_assert(sizeof(struct hda_tables) <= PAGE_SIZE, "HDA tables fit one page");
_Static_assert(HDA_PERIOD_BYTES <= PAGE_SIZE && !(HDA_PERIOD_BYTES % 128),
	       "HDA periods fit an aligned DMA page");

static enum { HDA_DEVICE, HDA_PROBE, HDA_SELFTEST, HDA_LATE_TEST } boot_mode;
static unsigned char *controller, *stream;
static struct hda_tables *tables;
static int16_t *periods[HDA_PERIODS];
static uint8_t corb_write, rirb_read;
static uint8_t corb_mask, rirb_mask;
static int verb_failed;
static int pcm_present;

void hda_boot_options(const struct multiboot_info *boot)
{
	const char *option;

	if (!(boot->flags & MB_FLAG_CMDLINE) || !boot->cmdline)
		return;
	option = (const char *)__vir(boot->cmdline);
	while (*option) {
		if (!strncmp(option, "hda=probe", 9) &&
		    (!option[9] || option[9] == ' ')) {
			boot_mode = HDA_PROBE;
			return;
		}
		if (!strncmp(option, "hda=selftest", 12) &&
		    (!option[12] || option[12] == ' ')) {
			boot_mode = HDA_SELFTEST;
			return;
		}
		if (!strncmp(option, "hda=test-late", 13) &&
		    (!option[13] || option[13] == ' ')) {
			boot_mode = HDA_LATE_TEST;
			return;
		}
		while (*option && *option != ' ')
			option++;
		while (*option == ' ')
			option++;
	}
}

static int hda_probe(const struct pci_device *dev, void *context)
{
	unsigned long address;
	size_t size;
	uint16_t command, caps;
	uint8_t version;
	unsigned char *bar, *alias;
	int stable = 0;

	if (pci_mem_bar32(dev, 0, &address, &size) || size < 4)
		return 0;
	bar = ioremap(address, size);
	if (!bar)
		return 0;
	command = pci_command(dev);
	pci_write_command(dev, command | PCI_COMMAND_MEMORY);
	caps = readw(bar + HDA_GCAP);
	version = readb(bar + HDA_VERSION_MAJOR);
	alias = ioremap(address + HDA_VERSION_MAJOR, sizeof(version));
	if (alias) {
		iounmap(bar + HDA_VERSION_MAJOR);
		stable = version == 1 && caps != 0xffff && readb(alias) == version;
		iounmap(alias);
	}
	stable = stable && !ioremap(0xfffffffeUL, 4) &&
		 readw(bar + HDA_GCAP) == caps && readb(bar + HDA_VERSION_MAJOR) == version;
	iounmap(bar);
	pci_write_command(dev, command);
	if (stable)
		pr_info("HDA: probe %u:%u.%u GCAP=%x major=%u; unmapped\n",
			dev->bus, dev->slot, dev->func, caps, version);
	else
		pr_warn("HDA: unstable probe at %u:%u.%u\n",
			dev->bus, dev->slot, dev->func);
	return 0;
}

static unsigned long hda_elapsed(unsigned long start)
{
	return (unsigned long)jiffies - start;
}

static void hda_delay(unsigned int ticks)
{
	unsigned long start = jiffies;
	while (hda_elapsed(start) < ticks)
		safe_halt();
}

static int hda_wait(const unsigned char *reg, unsigned int width,
		    uint32_t mask, uint32_t expected)
{
	unsigned long start = jiffies;
	do {
		uint32_t value = width == 4 ? readl(reg) :
				 width == 2 ? readw(reg) : readb(reg);
		if ((value & mask) == expected)
			return 0;
		safe_halt();
	} while (hda_elapsed(start) < HDA_TIMEOUT_TICKS);
	return -1;
}

static int hda_reset(void)
{
	writel(0, controller + HDA_INTCTL);
	writew(0, controller + HDA_WAKEEN);
	writel(0, controller + HDA_GCTL);
	if (hda_wait(controller + HDA_GCTL, 4, 1, 0))
		return -1;
	hda_delay(2);
	writel(1, controller + HDA_GCTL);
	if (hda_wait(controller + HDA_GCTL, 4, 1, 1))
		return -1;
	hda_delay(3);
	return 0;
}

static int hda_setup_rings(void)
{
	uint8_t corb_caps = readb(controller + HDA_CORBSIZE);
	uint8_t rirb_caps = readb(controller + HDA_RIRBSIZE);
	static const uint8_t masks[] = { 1, 15, 255 };
	int corb_size = HDA_RING_256, rirb_size = HDA_RING_256;

	while (corb_size >= 0 && !(corb_caps & HDA_CAP_RING(corb_size)))
		corb_size--;
	while (rirb_size >= 0 && !(rirb_caps & HDA_CAP_RING(rirb_size)))
		rirb_size--;
	if (corb_size < 0 || rirb_size < 0)
		return -1;
	writeb(0, controller + HDA_CORBCTL);
	writeb(0, controller + HDA_RIRBCTL);
	if (hda_wait(controller + HDA_CORBCTL, 1, HDA_RING_DMA, 0) ||
	    hda_wait(controller + HDA_RIRBCTL, 1, HDA_RING_DMA, 0))
		return -1;
	writeb(corb_size, controller + HDA_CORBSIZE);
	writeb(rirb_size, controller + HDA_RIRBSIZE);
	writel(__phy(tables->corb), controller + HDA_CORBLBASE);
	writel(0, controller + HDA_CORBUBASE);
	writel(__phy(tables->rirb), controller + HDA_RIRBLBASE);
	writel(0, controller + HDA_RIRBUBASE);
	writew(0, controller + HDA_CORBWP);
	writew(HDA_RING_RESET, controller + HDA_CORBRP);
	if (hda_wait(controller + HDA_CORBRP, 2, HDA_RING_RESET, HDA_RING_RESET))
		return -1;
	writew(0, controller + HDA_CORBRP);
	if (hda_wait(controller + HDA_CORBRP, 2, HDA_RING_RESET, 0))
		return -1;
	writew(HDA_RING_RESET, controller + HDA_RIRBWP);
	writeb(1, controller + HDA_CORBSTS);
	writeb(HDA_RIRB_IRQ | HDA_RIRB_OVERRUN, controller + HDA_RIRBSTS);
	writew(1, controller + HDA_RINTCNT);
	corb_write = 0;
	rirb_read = 0;
	corb_mask = masks[corb_size];
	rirb_mask = masks[rirb_size];
	verb_failed = 0;
	writeb(HDA_RING_DMA | HDA_RIRB_IRQ, controller + HDA_RIRBCTL);
	writeb(HDA_RING_DMA, controller + HDA_CORBCTL);
	return 0;
}

int hda_verb(unsigned int codec, unsigned int node, uint32_t verb,
	     uint32_t *response)
{
	unsigned long start = jiffies;
	volatile struct hda_response *rirb = tables->rirb;

	if (verb_failed)
		return -1;
	corb_write = (corb_write + 1) & corb_mask;
	tables->corb[corb_write] = (codec << 28) | (node << 20) | verb;
	asm volatile("" ::: "memory");
	writew(corb_write, controller + HDA_CORBWP);
	do {
		uint8_t producer = readw(controller + HDA_RIRBWP) & rirb_mask;
		uint8_t status = readb(controller + HDA_RIRBSTS);
		if (status & HDA_RIRB_OVERRUN || readb(controller + HDA_CORBSTS) & 1)
			break;
		while (rirb_read != producer) {
			rirb_read = (rirb_read + 1) & rirb_mask;
			uint32_t info = rirb[rirb_read].info;
			writeb(HDA_RIRB_IRQ, controller + HDA_RIRBSTS);
			if (info & HDA_RIRB_UNSOLICITED)
				continue;
			if ((info & 0x0f) != codec)
				goto fail;
			*response = rirb[rirb_read].value;
			return 0;
		}
		asm volatile("pause" ::: "memory");
	} while (hda_elapsed(start) < HDA_TIMEOUT_TICKS);
fail:
	verb_failed = 1;
	return -1;
}

uint32_t hda_pcm_clock(void)
{
	return readl(controller + HDA_WALLCLK);
}

static int hda_stream_wait(uint8_t mask, uint8_t expected)
{
	uint32_t start = hda_pcm_clock();
	for (unsigned int tries = 0; tries < 100000; tries++) {
		if ((readb(stream + SD_CTL) & mask) == expected)
			return 0;
		if (hda_pcm_clock() - start >= HDA_WALL_TICKS_PER_MS / 2)
			break;
		asm volatile("pause" ::: "memory");
	}
	return -1;
}

int hda_pcm_stop(void)
{
	writeb(0, stream + SD_CTL);
	return hda_stream_wait(SD_RUN, 0);
}

static int hda_setup_stream(void)
{
	if (hda_pcm_stop())
		return -1;
	writeb(SD_RESET, stream + SD_CTL);
	if (hda_stream_wait(SD_RESET, SD_RESET))
		return -1;
	writeb(0, stream + SD_CTL);
	if (hda_stream_wait(SD_RESET, 0))
		return -1;
	writeb(HDA_STREAM_TAG << 4, stream + SD_CTL_TAG);
	writeb(0x1c, stream + SD_STS);
	writew(HDA_PCM_FORMAT, stream + SD_FMT);
	writel(HDA_BUFFER_BYTES, stream + SD_CBL);
	writew(HDA_PERIODS - 1, stream + SD_LVI);
	writel(__phy(tables->bdl), stream + SD_BDPL);
	writel(0, stream + SD_BDPU);
	return 0;
}

int hda_pcm_start(void)
{
	if (hda_setup_stream())
		return -1;
	asm volatile("" ::: "memory");
	writeb(SD_RUN, stream + SD_CTL);
	return hda_stream_wait(SD_RUN, SD_RUN);
}

int hda_pcm_position(void)
{
	uint32_t bytes = readl(stream + SD_LPIB);
	if (bytes >= HDA_BUFFER_BYTES || (readb(stream + SD_STS) & SD_ERROR))
		return -1;
	return bytes / 4;
}

int16_t *hda_pcm_period(unsigned int index)
{
	return periods[index];
}

int hda_pcm_present(void)
{
	return pcm_present;
}

int hda_pcm_defer_first_poll(void)
{
	if (boot_mode != HDA_LATE_TEST)
		return 0;
	boot_mode = HDA_DEVICE;
	return 1;
}

static int hda_test_pcm(void)
{
	unsigned long start = jiffies;
	unsigned int position, last = 0;

	for (unsigned int frame = 0; frame < HDA_PERIOD_FRAMES / 2; frame++) {
		periods[0][frame * 2] = frame % 96 < 48 ? 12000 : -12000;
		periods[0][(frame + HDA_PERIOD_FRAMES / 2) * 2 + 1] =
			frame % 64 < 32 ? 8000 : -8000;
	}
	if (hda_pcm_start())
		return -1;
	do {
		position = readl(stream + SD_LPIB);
		if (position < last || readb(stream + SD_STS) & SD_ERROR)
			return -1;
		if (position >= 3 * HDA_PERIOD_BYTES)
			return hda_pcm_stop();
		last = position;
		safe_halt();
	} while (hda_elapsed(start) < HDA_TIMEOUT_TICKS);
	return -1;
}

static int hda_release(void)
{
	if (!stream && !tables)
		return 0;
	int error = stream ? hda_pcm_stop() : 0;

	writeb(0, controller + HDA_CORBCTL);
	writeb(0, controller + HDA_RIRBCTL);
	if (hda_wait(controller + HDA_CORBCTL, 1, HDA_RING_DMA, 0) ||
	    hda_wait(controller + HDA_RIRBCTL, 1, HDA_RING_DMA, 0))
		error = -1;
	if (error) {
		writel(0, controller + HDA_GCTL);
		if (hda_wait(controller + HDA_GCTL, 4, 1, 0))
			return -1;
	}
	for (unsigned int index = 0; index < HDA_PERIODS; index++) {
		if (periods[index])
			page_free((unsigned long)periods[index]);
		periods[index] = NULL;
	}
	if (tables)
		page_free((unsigned long)tables);
	tables = NULL;
	return 0;
}

static int hda_prepare_controller(void)
{
	uint16_t caps, codecs;
	unsigned int input_streams, offset;
	int configured = 0;

	caps = readw(controller + HDA_GCAP);
	if (readb(controller + HDA_VERSION_MAJOR) != 1 || !(caps >> 12) ||
	    hda_reset())
		return -1;
	codecs = readw(controller + HDA_STATESTS) & 0x7fff;
	if (!codecs) {
		pr_info("HDA: no codecs detected\n");
		return -1;
	}
	writew(codecs, controller + HDA_STATESTS);
	input_streams = (caps >> 8) & 0x0f;
	offset = HDA_STREAM_BASE + input_streams * HDA_STREAM_STRIDE;
	stream = controller + offset;
	writel(0, controller + HDA_DPLBASE);
	writel(0, controller + HDA_DPUBASE);
	tables = (void *)zero_page_alloc();
	if (!tables || hda_setup_rings())
		return -1;
	for (unsigned int codec = 0; codec < 15; codec++) {
		if ((codecs & (1 << codec)) && !hda_configure_codec(codec)) {
			configured = 1;
			break;
		}
		if (verb_failed)
			break;
	}
	if (!configured) {
		pr_info("HDA: no usable analog codec path\n");
		return -1;
	}
	for (unsigned int index = 0; index < HDA_PERIODS; index++) {
		periods[index] = (void *)zero_page_alloc();
		if (!periods[index])
			return -1;
		tables->bdl[index].low = __phy(periods[index]);
		tables->bdl[index].bytes = HDA_PERIOD_BYTES;
	}
	return hda_setup_stream();
}

static int hda_try_play(const struct pci_device *dev, void *context)
{
	unsigned long address;
	size_t size;
	uint16_t command;
	int error, quiesced;

	if (pci_mem_bar32(dev, 0, &address, &size) || size < 0x400)
		return 0;
	controller = ioremap(address, size);
	if (!controller)
		return 0;
	command = pci_command(dev);
	pci_write_command(dev, command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
			       HDA_COMMAND_INT_DISABLE);
	pr_info("HDA: controller %u:%u.%u vendor=%x device=%x\n", dev->bus,
		dev->slot, dev->func, dev->vendor_id, dev->device_id);
	error = hda_prepare_controller();
	if (!error && (boot_mode == HDA_DEVICE || boot_mode == HDA_LATE_TEST)) {
		pcm_present = 1;
		pr_info("HDA: PCM device ready\n");
		return 1;
	}
	if (!error)
		error = hda_test_pcm();
	quiesced = !hda_release();
	pci_write_command(dev, quiesced ? command : command & ~PCI_COMMAND_MASTER);
	iounmap(controller);
	controller = stream = NULL;
	if (!quiesced) {
		pr_warn("HDA: DMA stop failed; buffers quarantined\n");
		return 1;
	}
	if (!error)
		pr_info("HDA: PCM self-test passed\n");
	else
		pr_warn("HDA: PCM unavailable on %u:%u.%u\n", dev->bus, dev->slot, dev->func);
	return !error;
}

void hda_init(void)
{
	if (boot_mode == HDA_PROBE)
		pci_scan_class(HDA_CLASS, HDA_SUBCLASS, PCI_ANY_PROG_IF,
			       hda_probe, NULL);
	else if (!pci_scan_class(HDA_CLASS, HDA_SUBCLASS, PCI_ANY_PROG_IF,
				hda_try_play, NULL))
		pr_info("HDA: no playback device\n");
}
