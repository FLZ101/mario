#include <misc.h>
#include <errno.h>

#include <fs/fs.h>
#include <fs/ahci.h>
#include <fs/gpt.h>

#include <pci.h>

#include <mm/page_alloc.h>
#include <mm/ioremap.h>

#include <lib/string.h>

/*
 * AHCI/SATA block-device driver.
 *
 * The HBA is memory-mapped through its PCI BAR5 (the ABAR).  Every
 * implemented AHCI port is treated as one independent disk, so a machine
 * with several SATA drives exposes /dev/hda through /dev/hdf.  Each port
 * owns its command header, command table, completion semaphore, and error
 * state; the HBA interrupt handler wakes only the port whose command has
 * finished.
 *
 * Boot-time identify and MBR/EBR parsing poll the command bit while
 * interrupts are still masked.  Normal reads and writes wait on the port
 * semaphore, which irq_ahci() signals from IRQ10/IRQ11.
 *
 * Minor numbering is disk-minor based:
 *
 *   disk_index = minor / HD_MINORS_PER_DISK
 *   part       = minor % HD_MINORS_PER_DISK
 *
 * part == 0 addresses the whole disk and part 1..HD_MAX_PARTITIONS
 * addresses a parsed MBR/EBR partition.
 */

#define AHCI_HBA_SIZE		0x2000
#define AHCI_MAX_PORTS		32

#define AHCI_CAP		0x00
#define AHCI_GHC		0x04
#define AHCI_IS			0x08
#define AHCI_PI			0x0c

#define AHCI_PX_OFFSET		0x100
#define AHCI_PX_STRIDE		0x80
#define AHCI_PxCLB		0x00
#define AHCI_PxCLBU		0x04
#define AHCI_PxFB		0x08
#define AHCI_PxFBU		0x0c
#define AHCI_PxIS		0x10
#define AHCI_PxIE		0x14
#define AHCI_PxCMD		0x18
#define AHCI_PxTFD		0x20
#define AHCI_PxSSTS		0x28
#define AHCI_PxCI		0x38

#define AHCI_GHC_AE		(1U << 31)
#define AHCI_GHC_IE		(1U << 1)

#define AHCI_PxCMD_ST		(1U << 0)
#define AHCI_PxCMD_FRE		(1U << 4)
#define AHCI_PxCMD_FR		(1U << 14)
#define AHCI_PxCMD_CR		(1U << 15)

#define AHCI_PxIS_DHRS		(1U << 0)
#define AHCI_PxIS_PSS		(1U << 1)
#define AHCI_PxIS_TFES		(1U << 30)
#define AHCI_PxIE_DHRE		(1U << 0)
#define AHCI_PxIE_TFEE		(1U << 30)

#define AHCI_PxTFD_ERR		(1U << 0)
#define AHCI_PxTFD_DF		(1U << 5)

#define AHCI_PxSSTS_DET_MASK	0x0fU
#define AHCI_PxSSTS_DET_PRESENT	0x03U

#define ATA_CMD_IDENTIFY	0xec
#define ATA_CMD_READ_EXT	0x24
#define ATA_CMD_WRITE_EXT	0x34
#define ATA_CMD_FLUSH_EXT	0xea

#define ATA_FIS_TYPE_H2D	0x27

#define AHCI_TIMEOUT		10000000U

#define HD_SECTOR_SIZE		512
#define HD_MAX_PARTITIONS	16
#define HD_MINORS_PER_DISK	(HD_MAX_PARTITIONS + 1)
#define HD_MAX_DISKS		6	/* /dev/hda through /dev/hdf */

struct hba_command_header {
	uint32_t flags_prdtl;
	uint32_t prdbc;
	uint32_t ctba;
	uint32_t ctbau;
	uint32_t reserved[4];
} __attribute__((packed));

struct hba_prdt_entry {
	uint32_t dba;
	uint32_t dbau;
	uint32_t reserved;
	uint32_t dbc;
} __attribute__((packed));

struct hba_command_table {
	uint8_t cfis[64];
	uint8_t acmd[16];
	uint8_t reserved[48];
	struct hba_prdt_entry prdt[1];
} __attribute__((packed));

/* A parsed DOS/MBR or EBR partition. */
struct hd_partition {
	uint64_t start_sector;
	uint64_t nr_sectors;
	uint8_t present;
};

/*
 * AHCI state that must be private to each port.  Without this separation,
 * an interrupt from one disk would wake the waiter of a different disk.
 */
struct ahci_port {
	volatile uint32_t *port_regs;
	unsigned int port;
	unsigned long memory;	/* page holding command header/FIS/table */
	struct hba_command_header *command_header;
	struct hba_command_table *command_table;
	struct semaphore done;
	volatile int complete;
	volatile int error;
	int initialized;
};

/* One logical disk, bound to an AHCI port when that port is present. */
struct hd_disk {
	struct ahci_port *port;
	uint64_t total_sectors;
	struct hd_partition partitions[HD_MAX_PARTITIONS];
	int present;
};

static volatile uint32_t *hba;		/* ABAR mapped into kernel space */
static struct ahci_port ahci_ports[HD_MAX_DISKS];
static struct hd_disk hd_disks[HD_MAX_DISKS];
static unsigned int nr_ahci_disks;

/*
 * MBR/EBR fields are little-endian even though the rest of this kernel does
 * not have a byte-swapping MMIO layer, so decode them explicitly here.
 */
static uint32_t read_le32(const uint8_t *p)
{
	return (uint32_t)p[0] |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static int wait_clear(volatile uint32_t *reg, uint32_t bits)
{
	unsigned int timeout;

	for (timeout = 0; timeout < AHCI_TIMEOUT; timeout++)
		if (!(readl(reg) & bits))
			return 0;
	return -ETIMEDOUT;
}

static int wait_set(volatile uint32_t *reg, uint32_t bits)
{
	unsigned int timeout;

	for (timeout = 0; timeout < AHCI_TIMEOUT; timeout++)
		if ((readl(reg) & bits) == bits)
			return 0;
	return -ETIMEDOUT;
}

static void hba_write(unsigned int offset, uint32_t value)
{
	writel(value, &hba[offset / 4]);
}

static uint32_t hba_read(unsigned int offset)
{
	return readl(&hba[offset / 4]);
}

static void port_write(struct ahci_port *port, unsigned int offset,
		       uint32_t value)
{
	writel(value, &port->port_regs[offset / 4]);
}

static uint32_t port_read(struct ahci_port *port, unsigned int offset)
{
	return readl(&port->port_regs[offset / 4]);
}

/*
 * Build a host-to-device register FIS for an LBA48 command.
 *
 * Byte 7 is the device register: the LBA bit (0x40) marks this as LBA
 * addressing and holds bits 24-27 of the LBA.  Bytes 8-10 carry the upper
 * LBA bytes used by READ/WRITE SECTORS EXT.
 */
static void fill_command_fis(uint8_t command, uint64_t lba,
			     uint16_t sector_count, uint8_t *cfis)
{
	cfis[0] = ATA_FIS_TYPE_H2D;
	cfis[1] = 0x80;		/* command */
	cfis[2] = command;
	cfis[3] = 0;		/* features low */
	cfis[4] = lba & 0xff;
	cfis[5] = (lba >> 8) & 0xff;
	cfis[6] = (lba >> 16) & 0xff;
	cfis[7] = 0x40 | ((lba >> 24) & 0x0f);
	cfis[8] = (lba >> 24) & 0xff;
	cfis[9] = (lba >> 32) & 0xff;
	cfis[10] = (lba >> 40) & 0xff;
	cfis[11] = 0;		/* features high */
	cfis[12] = sector_count & 0xff;
	cfis[13] = (sector_count >> 8) & 0xff;
	cfis[14] = 0;
	cfis[15] = 0;
}

static int ahci_wait_poll(struct ahci_port *port)
{
	uint32_t tfd;
	uint32_t is;
	unsigned int timeout;

	if (wait_clear(&port->port_regs[AHCI_PxCI / 4], 1))
		return -ETIMEDOUT;

	/*
	 * IDENTIFY is a PIO command: the HBA raises PSS when the PIO Setup FIS
	 * arrives and waits for software to acknowledge it before finishing the
	 * data transfer.  Clear PSS, then wait for the register FIS or error.
	 */
	for (timeout = 0; timeout < AHCI_TIMEOUT; timeout++) {
		is = port_read(port, AHCI_PxIS);
		if (is & AHCI_PxIS_TFES)
			return -EIO;
		if (is & AHCI_PxIS_PSS)
			port_write(port, AHCI_PxIS, AHCI_PxIS_PSS);
		if (is & AHCI_PxIS_DHRS)
			break;
	}
	if (timeout == AHCI_TIMEOUT)
		return -ETIMEDOUT;

	tfd = port_read(port, AHCI_PxTFD);
	if (tfd & (AHCI_PxTFD_ERR | AHCI_PxTFD_DF))
		return -EIO;

	is = port_read(port, AHCI_PxIS);
	if (is & AHCI_PxIS_TFES)
		return -EIO;

	return 0;
}

/*
 * Fill the selected port's command table and issue it.  Polling is used
 * before interrupts are enabled; the normal path blocks on port->done.
 */
static int ahci_issue_command(struct ahci_port *port, uint8_t command,
			      uint64_t lba, uint16_t sector_count, char *buf,
			      int write, int poll)
{
	struct hba_command_table *table = port->command_table;
	struct hba_prdt_entry *prdt = table->prdt;
	uint32_t flags;
	int prdtl;

	if (wait_clear(&port->port_regs[AHCI_PxCI / 4], 1))
		return -ETIMEDOUT;

	memset(table, 0, sizeof(*table));
	fill_command_fis(command, lba, sector_count, table->cfis);
	prdtl = 0;
	if (buf) {
		/* Transfer one 512-byte sector and request a completion interrupt. */
		prdt->dba = __phy((unsigned long)buf);
		prdt->dbau = 0;
		prdt->dbc = (HD_SECTOR_SIZE - 1) | (1U << 31);
		prdtl = 1;
	}

	/* CFL=5, PRDTL set from the data path, and W is set for writes. */
	flags = 5 | ((uint32_t)prdtl << 16) |
		((write ? 1U : 0U) << 6);
	port->command_header->flags_prdtl = flags;
	port->command_header->prdbc = 0;
	port->command_header->ctba = __phy((unsigned long)table);
	port->command_header->ctbau = 0;

	sema_init(&port->done, 0);
	port->complete = 0;
	port->error = 0;
	port_write(port, AHCI_PxIS, 0xffffffffU);

	__asm__ __volatile__("" ::: "memory");
	port_write(port, AHCI_PxCI, 1);

	if (poll)
		return ahci_wait_poll(port);

	down(&port->done);
	if (port->error)
		return -EIO;
	return 0;
}

/*
 * IRQ10/IRQ11 entry point installed by src/kernel/entry.S.  It walks the
 * initialized disks, handles only ports signalled in AHCI_IS, and wakes the
 * matching port waiter when a register FIS or task-file error arrives.
 */
void irq_ahci(void)
{
	uint32_t is;
	unsigned int i;

	if (!hba)
		return;

	is = hba_read(AHCI_IS);
	if (!is)
		return;

	for (i = 0; i < nr_ahci_disks; i++) {
		struct hd_disk *disk = &hd_disks[i];
		struct ahci_port *port = disk->port;
		uint32_t pis;

		if (!disk->present || !port->initialized)
			continue;
		if (!(is & (1U << port->port)))
			continue;

		pis = port_read(port, AHCI_PxIS);
		if (pis & AHCI_PxIS_TFES)
			port->error = 1;
		if (pis & AHCI_PxIS_DHRS)
			port->complete = 1;
		if (pis)
			port_write(port, AHCI_PxIS, pis);

		if (port->complete || port->error)
			up(&port->done);
	}

	hba_write(AHCI_IS, is);
}

static int ahci_read_sectors(struct ahci_port *port, uint64_t lba,
			     unsigned long nr, char *buf)
{
	unsigned long i;

	for (i = 0; i < nr; i++) {
		int error = ahci_issue_command(port, ATA_CMD_READ_EXT, lba + i,
					       1, buf + i * HD_SECTOR_SIZE,
					       0, 0);
		if (error)
			return error;
	}
	return 0;
}

static int ahci_write_sectors(struct ahci_port *port, uint64_t lba,
			      unsigned long nr, char *buf)
{
	unsigned long i;

	for (i = 0; i < nr; i++) {
		int error = ahci_issue_command(port, ATA_CMD_WRITE_EXT, lba + i,
					       1, buf + i * HD_SECTOR_SIZE,
					       1, 0);
		if (error)
			return error;
	}
	return 0;
}

static int ahci_flush(struct ahci_port *port)
{
	return ahci_issue_command(port, ATA_CMD_FLUSH_EXT, 0, 0, NULL, 0, 0);
}

/*
 * Identical to the IRQ-driven read path, except it polls PxCI/PxIS.  This is
 * needed while interrupts are still disabled during hd_init().
 */
static int ahci_read_sectors_poll(struct ahci_port *port, uint64_t lba,
				  unsigned long nr, char *buf)
{
	unsigned long i;

	for (i = 0; i < nr; i++) {
		int error = ahci_issue_command(port, ATA_CMD_READ_EXT, lba + i,
					       1, buf + i * HD_SECTOR_SIZE,
					       0, 1);
		if (error)
			return error;
	}
	return 0;
}

static int ahci_gpt_read_sectors(void *priv, uint64_t lba,
				 unsigned int nr, void *buf)
{
	struct hd_disk *disk = (struct hd_disk *)priv;

	return ahci_read_sectors_poll(disk->port, lba, nr, (char *)buf);
}

static int ahci_identify(struct ahci_port *port, uint16_t *identify)
{
	return ahci_issue_command(port, ATA_CMD_IDENTIFY, 0, 1,
				  (char *)identify, 0, 1);
}

/*
 * Stop the port DMA engine, point it at its private command list and
 * received-FIS area, start FIS receive/DMA, then require a present SATA
 * device.
 */
static int ahci_port_init(struct ahci_port *port, unsigned int port_number)
{
	uint32_t cmd;

	memset(port, 0, sizeof(*port));
	port->port = port_number;
	port->port_regs =
		hba + (AHCI_PX_OFFSET + port_number * AHCI_PX_STRIDE) / 4;

	cmd = port_read(port, AHCI_PxCMD);
	cmd &= ~(AHCI_PxCMD_ST | AHCI_PxCMD_FRE);
	port_write(port, AHCI_PxCMD, cmd);
	if (wait_clear(&port->port_regs[AHCI_PxCMD / 4],
		       AHCI_PxCMD_CR | AHCI_PxCMD_FR))
		return -ETIMEDOUT;

	port->memory = zero_page_alloc();
	if (!port->memory)
		return -ENOMEM;

	port->command_header = (struct hba_command_header *)port->memory;
	port->command_table =
		(struct hba_command_table *)(port->memory + 2048);

	port_write(port, AHCI_PxCLB, __phy(port->memory));
	port_write(port, AHCI_PxCLBU, 0);
	port_write(port, AHCI_PxFB, __phy(port->memory + 1024));
	port_write(port, AHCI_PxFBU, 0);
	port_write(port, AHCI_PxIS, 0xffffffffU);
	port_write(port, AHCI_PxIE, AHCI_PxIE_DHRE | AHCI_PxIE_TFEE);

	port_write(port, AHCI_PxCMD, port_read(port, AHCI_PxCMD) |
		   AHCI_PxCMD_FRE);
	if (wait_set(&port->port_regs[AHCI_PxCMD / 4], AHCI_PxCMD_FR))
		return -ETIMEDOUT;
	port_write(port, AHCI_PxCMD, port_read(port, AHCI_PxCMD) |
		   AHCI_PxCMD_ST);
	if (wait_set(&port->port_regs[AHCI_PxCMD / 4], AHCI_PxCMD_CR))
		return -ETIMEDOUT;

	if (wait_set(&port->port_regs[AHCI_PxSSTS / 4],
		     AHCI_PxSSTS_DET_PRESENT) < 0)
		return -ENODEV;
	if ((port_read(port, AHCI_PxSSTS) & AHCI_PxSSTS_DET_MASK) !=
	    AHCI_PxSSTS_DET_PRESENT)
		return -ENODEV;

	port->initialized = 1;
	return 0;
}

static void hd_add_partition(struct hd_disk *disk, unsigned int minor,
			     uint64_t start, uint64_t nr_sectors)
{
	unsigned int index;

	if (minor == 0 || minor > HD_MAX_PARTITIONS)
		return;
	index = minor - 1;
	if (disk->partitions[index].present)
		return;
	if (start >= disk->total_sectors ||
	    nr_sectors > disk->total_sectors - start)
		return;

	disk->partitions[index].start_sector = start;
	disk->partitions[index].nr_sectors = nr_sectors;
	disk->partitions[index].present = 1;
}

/*
 * Walk the DOS extended-partition chain.  Each EBR contains one logical
 * partition in slot 0 (relative to this EBR) and the next EBR in slot 1
 * (relative to the extended partition base).
 */
static int hd_parse_ebr(struct hd_disk *disk, uint64_t extended_base)
{
	uint8_t sector[HD_SECTOR_SIZE];
	uint64_t ebr;
	unsigned int logical_minor = 5;
	unsigned int iteration;

	ebr = extended_base;
	for (iteration = 0; iteration < HD_MAX_PARTITIONS; iteration++) {
		uint8_t *entry0;
		uint8_t *entry1;
		uint8_t type0;
		uint8_t type1;
		uint32_t start0;
		uint32_t nr0;
		uint32_t next_ebr;
		uint64_t absolute_start;

		if (logical_minor > HD_MAX_PARTITIONS)
			break;
		if (ebr >= disk->total_sectors)
			break;
		if (ahci_read_sectors_poll(disk->port, ebr, 1, (char *)sector))
			break;
		if (sector[510] != 0x55 || sector[511] != 0xaa)
			break;

		entry0 = (uint8_t *)sector + 446;
		entry1 = (uint8_t *)sector + 446 + 16;
		type0 = entry0[4];
		type1 = entry1[4];
		start0 = read_le32(entry0 + 8);
		nr0 = read_le32(entry0 + 12);
		next_ebr = read_le32(entry1 + 8);

		if (type0 && start0 && nr0) {
			absolute_start = ebr + start0;
			if (absolute_start < disk->total_sectors &&
			    nr0 <= disk->total_sectors - absolute_start)
				hd_add_partition(disk, logical_minor++,
						 absolute_start, nr0);
		}

		if (!type1 || !next_ebr)
			break;
		ebr = extended_base + next_ebr;
		if (ebr >= disk->total_sectors)
			break;
	}

	return 0;
}

static void hd_parse_mbr(struct hd_disk *disk)
{
	uint8_t sector[HD_SECTOR_SIZE];
	uint64_t extended_base = 0;
	int i;

	if (ahci_read_sectors_poll(disk->port, 0, 1, (char *)sector))
		return;
	if (sector[510] != 0x55 || sector[511] != 0xaa)
		return;

	for (i = 0; i < 4; i++) {
		uint8_t *entry = (uint8_t *)sector + 446 + i * 16;
		uint8_t type = entry[4];
		uint32_t start = read_le32(entry + 8);
		uint32_t nr = read_le32(entry + 12);

		if (!type || !start || !nr)
			continue;
		if ((uint64_t)start + nr > disk->total_sectors)
			continue;
		if (type == 0xee)
			continue;

		if (type == 0x05 || type == 0x0f || type == 0x85) {
			if (!extended_base)
				extended_base = start;
			continue;
		}

		hd_add_partition(disk, i + 1, start, nr);
	}

	if (extended_base)
		hd_parse_ebr(disk, extended_base);
}

static int hd_parse_gpt(struct hd_disk *disk)
{
	struct gpt_partition parts[HD_MAX_PARTITIONS];
	unsigned int nr_parts;
	unsigned int i;
	int error;

	error = gpt_parse(disk, disk->total_sectors, ahci_gpt_read_sectors,
			  parts, HD_MAX_PARTITIONS, &nr_parts);
	if (error)
		return error;

	for (i = 0; i < HD_MAX_PARTITIONS; i++) {
		if (!parts[i].present)
			continue;
		hd_add_partition(disk, i + 1, parts[i].first_lba,
				 parts[i].last_lba - parts[i].first_lba + 1);
	}
	return 0;
}

/*
 * Resolve a device minor to its disk and per-disk partition number.  The
 * caller is responsible for validating part when part is nonzero.
 */
static struct hd_disk *hd_get_disk(dev_t dev, unsigned int *part)
{
	unsigned int minor = MINOR(dev);
	unsigned int disk_index = minor / HD_MINORS_PER_DISK;
	struct hd_disk *disk;

	if (disk_index >= HD_MAX_DISKS)
		return NULL;

	disk = &hd_disks[disk_index];
	if (!disk->present)
		return NULL;

	*part = minor % HD_MINORS_PER_DISK;
	return disk;
}

static int hd_range(struct hd_disk *disk, unsigned int part,
		    unsigned long sector, unsigned long nr, uint64_t *start,
		    uint64_t *limit)
{
	if (part == 0) {
		*start = 0;
		*limit = disk->total_sectors;
	} else {
		struct hd_partition *partition;

		if (part > HD_MAX_PARTITIONS)
			return -ENOENT;
		partition = &disk->partitions[part - 1];
		if (!partition->present)
			return -ENOENT;
		*start = partition->start_sector;
		*limit = partition->nr_sectors;
	}

	if (sector >= *limit || nr > *limit - sector)
		return -EINVAL;
	return 0;
}

/*
 * Minor 0 of each disk addresses the whole disk; nonzero minors address a
 * parsed partition and are range-checked before submission.
 */
static int hd_read(dev_t dev, unsigned long sector, unsigned long nr,
		   char *buf)
{
	struct hd_disk *disk;
	unsigned int part;
	uint64_t start;
	uint64_t limit;
	int error;

	disk = hd_get_disk(dev, &part);
	if (!disk)
		return -ENODEV;
	error = hd_range(disk, part, sector, nr, &start, &limit);
	if (error)
		return error;
	return ahci_read_sectors(disk->port, start + sector, nr, buf);
}

static int hd_write(dev_t dev, unsigned long sector, unsigned long nr,
		    char *buf)
{
	struct hd_disk *disk;
	unsigned int part;
	uint64_t start;
	uint64_t limit;
	int error;

	disk = hd_get_disk(dev, &part);
	if (!disk)
		return -ENODEV;
	error = hd_range(disk, part, sector, nr, &start, &limit);
	if (error)
		return error;
	error = ahci_write_sectors(disk->port, start + sector, nr, buf);
	if (error)
		return error;
	if (nr)
		return ahci_flush(disk->port);
	return 0;
}

static int hd_get_info(dev_t dev, struct blkdev_info *info)
{
	struct hd_disk *disk;
	unsigned int part;
	uint64_t sectors;

	disk = hd_get_disk(dev, &part);
	if (!disk)
		return -ENODEV;

	if (part == 0) {
		sectors = disk->total_sectors;
	} else if (part <= HD_MAX_PARTITIONS &&
		   disk->partitions[part - 1].present) {
		sectors = disk->partitions[part - 1].nr_sectors;
	} else {
		return -ENOENT;
	}

	if (sectors > 0x7fffffffULL)
		return -EOVERFLOW;

	info->sector_size = HD_SECTOR_SIZE;
	info->nr = sectors;
	return 0;
}

struct blkdev_operations hd_ops = {
	.blkdev_read = hd_read,
	.blkdev_write = hd_write,
	.blkdev_get_info = hd_get_info,
};

/*
 * Optional initialization called from blkdev_init().  If no AHCI controller
 * is present this is a no-op; the ramdisk root remains unchanged.  All
 * implemented ports are probed, up to HD_MAX_DISKS disks.
 */
void hd_init(void)
{
	struct pci_device dev;
	uint64_t abar;
	uint32_t bar_flags;
	int bar_count;
	uint32_t pi;
	uint32_t ghc;
	uint16_t identify[256];
	unsigned int port_number;
	int error;

	if (!pci_find_device_by_class(0x01, 0x06, 0x01, &dev))
		return;
	if (pci_bar_info(&dev, 5, &abar, &bar_flags, &bar_count))
		return;
	if (bar_flags & 0x01)
		return;
	if (!abar)
		return;

	hba = ioremap(abar, AHCI_HBA_SIZE);
	if (!hba)
		return;

	ghc = hba_read(AHCI_GHC);
	hba_write(AHCI_GHC, ghc | AHCI_GHC_AE | AHCI_GHC_IE);

	pi = hba_read(AHCI_PI);
	if (!pi)
		return;

	memset(ahci_ports, 0, sizeof(ahci_ports));
	memset(hd_disks, 0, sizeof(hd_disks));
	nr_ahci_disks = 0;

	for (port_number = 0;
	     port_number < AHCI_MAX_PORTS && nr_ahci_disks < HD_MAX_DISKS;
	     port_number++) {
		struct ahci_port *port;
		struct hd_disk *disk;
		uint64_t total_sectors;

		/* PI bit zero means this port is not implemented by the HBA. */
		if (!(pi & (1U << port_number)))
			continue;

		port = &ahci_ports[nr_ahci_disks];
		error = ahci_port_init(port, port_number);
		if (error)
			continue;

		error = ahci_identify(port, identify);
		if (error)
			continue;
		if (!(identify[83] & (1U << 10)) ||
		    !(identify[86] & (1U << 10)))
			continue;

		total_sectors = (uint64_t)identify[100] |
				((uint64_t)identify[101] << 16) |
				((uint64_t)identify[102] << 32) |
				((uint64_t)identify[103] << 48);
		if (!total_sectors)
			continue;

		disk = &hd_disks[nr_ahci_disks];
		disk->port = port;
		disk->total_sectors = total_sectors;
		memset(disk->partitions, 0, sizeof(disk->partitions));
		if (hd_parse_gpt(disk))
			hd_parse_mbr(disk);
		disk->present = 1;

		printk("AHCI disk %c: %u sectors\n",
		       'a' + nr_ahci_disks, (unsigned int)total_sectors);
		nr_ahci_disks++;
	}

	if (!nr_ahci_disks)
		return;

	error = register_blkdev(HD_MAJOR, &hd_ops);
	if (error)
		return;
}
