#include <errno.h>
#include <lib/spinlock.h>
#include <lib/string.h>
#include <misc.h>
#include <mm/ioremap.h>
#include <mm/page_alloc.h>
#include <mm/uaccess.h>
#include <net/ethernet.h>
#include <net/worker.h>
#include <pci.h>
#include <time.h>
#include <uapi/mario/netctl.h>

#include <lwip/etharp.h>
#include <netif/ethernet.h>

#define E1000_CTRL 0x0000
#define E1000_STATUS 0x0008
#define E1000_ICR 0x00c0
#define E1000_IMS 0x00d0
#define E1000_IMC 0x00d8
#define E1000_RCTL 0x0100
#define E1000_TCTL 0x0400
#define E1000_TIPG 0x0410
#define E1000_RDBAL 0x2800
#define E1000_RDBAH 0x2804
#define E1000_RDLEN 0x2808
#define E1000_RDH 0x2810
#define E1000_RDT 0x2818
#define E1000_TDBAL 0x3800
#define E1000_TDBAH 0x3804
#define E1000_TDLEN 0x3808
#define E1000_TDH 0x3810
#define E1000_TDT 0x3818
#define E1000_RAL 0x5400
#define E1000_RAH 0x5404

#define CTRL_RST (1U << 26)
#define CTRL_SLU (1U << 6)
#define STATUS_LU (1U << 1)
#define RAH_AV (1U << 31)
#define RCTL_EN (1U << 1)
#define RCTL_BAM (1U << 15)
#define RCTL_SECRC (1U << 26)
#define TCTL_EN (1U << 1)
#define TCTL_PSP (1U << 3)
#define TX_EOP (1U << 0)
#define TX_IFCS (1U << 1)
#define TX_RS (1U << 3)
#define TX_DD (1U << 0)
#define TX_ERRORS 0x0e
#define RX_DD (1U << 0)
#define RX_EOP (1U << 1)
#define ICR_TXDW (1U << 0)
#define ICR_LSC (1U << 2)
#define ICR_RXDMT0 (1U << 4)
#define ICR_RXO (1U << 6)
#define ICR_RXT0 (1U << 7)
#define E1000_EVENTS (ICR_TXDW | ICR_LSC | ICR_RXDMT0 | ICR_RXO | ICR_RXT0)
#define PCI_INTX_DISABLE (1U << 10)

#define RX_RING_SIZE 16U
#define TX_RING_SIZE 8U
#define RX_BATCH 16U
#define BUF_SIZE 2048U
#define PAGE_SLOTS (PAGE_SIZE / BUF_SIZE)
#define FRAME_MIN 14U
#define FRAME_MAX 1514U

struct rx_descriptor {
	uint64_t address;
	uint16_t length, checksum;
	uint8_t status, errors;
	uint16_t special;
};

struct tx_descriptor {
	uint64_t address;
	uint16_t length;
	uint8_t cso, command, status, css;
	uint16_t special;
};

_Static_assert(sizeof(struct rx_descriptor) == 16, "82540 RX descriptor size");
_Static_assert(sizeof(struct tx_descriptor) == 16, "82540 TX descriptor size");
_Static_assert(sizeof(struct mario_net_status) == 76, "netctl status ABI size");
_Static_assert(sizeof(struct mario_net_config) == 20, "netctl config ABI size");

static volatile uint8_t *registers;
static struct netif eth0;
static struct mario_net_status status;
static struct rx_descriptor *rx_ring;
static struct tx_descriptor *tx_ring;
static unsigned long rx_pages[RX_RING_SIZE / PAGE_SLOTS];
static unsigned long tx_pages[TX_RING_SIZE / PAGE_SLOTS];
static unsigned int rx_next, tx_next, tx_clean, tx_outstanding;
static unsigned int tx_posted;
static int rx_discard;
static unsigned int irq_events;
static unsigned int irq_count;
static unsigned int irq_enabled;
static spinlock_t irq_lock = SPINLOCK_UNLOCKED;

static uint32_t reg_read(unsigned int offset)
{
	return readl(registers + offset);
}

static void reg_write(unsigned int offset, uint32_t value)
{
	writel(value, registers + offset);
}

static void dma_fence(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void *ring_buffer(const unsigned long *pages, unsigned int index)
{
	return (void *)(pages[index / PAGE_SLOTS] + index % PAGE_SLOTS * BUF_SIZE);
}

static void free_rings(void)
{
	for (unsigned int page = 0; page < RX_RING_SIZE / PAGE_SLOTS; page++)
		if (rx_pages[page])
			page_free(rx_pages[page]);
	for (unsigned int page = 0; page < TX_RING_SIZE / PAGE_SLOTS; page++)
		if (tx_pages[page])
			page_free(tx_pages[page]);
	if (rx_ring)
		page_free((unsigned long)rx_ring);
	if (tx_ring)
		page_free((unsigned long)tx_ring);
}

static int alloc_rings(void)
{
	rx_ring = (void *)zero_page_alloc();
	tx_ring = (void *)zero_page_alloc();
	if (!rx_ring || !tx_ring)
		return -ENOMEM;
	for (unsigned int page = 0; page < RX_RING_SIZE / PAGE_SLOTS; page++) {
		rx_pages[page] = zero_page_alloc();
		if (!rx_pages[page])
			return -ENOMEM;
	}
	for (unsigned int page = 0; page < TX_RING_SIZE / PAGE_SLOTS; page++) {
		tx_pages[page] = zero_page_alloc();
		if (!tx_pages[page])
			return -ENOMEM;
	}
	for (unsigned int index = 0; index < RX_RING_SIZE; index++)
		rx_ring[index].address = __phy(ring_buffer(rx_pages, index));
	for (unsigned int index = 0; index < TX_RING_SIZE; index++)
		tx_ring[index].address = __phy(ring_buffer(tx_pages, index));
	return 0;
}

static int match_e1000(const struct pci_device *dev, void *context)
{
	if (dev->vendor_id != 0x8086 || dev->device_id != 0x100e)
		return 0;
	*(struct pci_device *)context = *dev;
	return 1;
}

static int reset_and_read_mac(void)
{
	unsigned long deadline = jiffies + HZ;
	uint32_t low, high;

	reg_write(E1000_IMC, ~0U);
	reg_write(E1000_RCTL, 0);
	reg_write(E1000_TCTL, 0);
	reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_RST);
	while (reg_read(E1000_CTRL) & CTRL_RST) {
		if ((long)(jiffies - deadline) >= 0)
			return -ETIMEDOUT;
	}
	reg_write(E1000_IMC, ~0U);
	reg_read(E1000_ICR);
	low = reg_read(E1000_RAL);
	high = reg_read(E1000_RAH);
	if (!(high & RAH_AV) || (low & 1) || (!low && !(high & 0xffff)))
		return -EIO;
	for (unsigned int byte = 0; byte < 4; byte++)
		status.mac[byte] = low >> (8 * byte);
	status.mac[4] = high;
	status.mac[5] = high >> 8;
	reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_SLU);
	return 0;
}

void ethernet_probe(void)
{
	struct pci_device dev;
	unsigned long bar;
	size_t size;
	uint16_t command;
	int error;

	if (!pci_scan_class(0x02, 0x00, PCI_ANY_PROG_IF, match_e1000, &dev))
		return;
	status.irq = dev.irq;
	if (dev.irq != 10 && dev.irq != 11) {
		error = -ENXIO;
		goto fail;
	}
	error = pci_mem_bar32(&dev, 0, &bar, &size);
	if (error)
		goto fail;
	if (size < E1000_RAH + sizeof(uint32_t)) {
		error = -ENODEV;
		goto fail;
	}
	registers = ioremap(bar, E1000_RAH + sizeof(uint32_t));
	if (!registers) {
		error = -ENOMEM;
		goto fail;
	}
	command = pci_command(&dev);
	pci_write_command(&dev, (command | PCI_COMMAND_MEMORY) & ~PCI_COMMAND_MASTER);
	error = reset_and_read_mac();
	if (!error)
		error = alloc_rings();
	if (error) {
		free_rings();
		pci_write_command(&dev, command & ~PCI_COMMAND_MASTER);
		iounmap((void *)registers);
		registers = NULL;
		goto fail;
	}
	pci_write_command(&dev, (command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER) & ~PCI_INTX_DISABLE);
	pr_info("e1000: 82540EM, IRQ %u, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
		status.irq, status.mac[0], status.mac[1], status.mac[2],
		status.mac[3], status.mac[4], status.mac[5]);
	return;

fail:
	status.config.flags = MARIO_NET_PROBE_FAILED;
	status.probe_error = -error;
	pr_err("e1000: probe failed (%d)\n", error);
}

static void tx_reclaim(void)
{
	while (tx_outstanding) {
		uint8_t completion = __atomic_load_n(&tx_ring[tx_clean].status, __ATOMIC_ACQUIRE);

		if (!(completion & TX_DD))
			break;
		if (completion & TX_ERRORS)
			status.tx_errors++;
		else
			status.tx_packets++;
		tx_clean = (tx_clean + 1) % TX_RING_SIZE;
		tx_outstanding--;
	}
}

static err_t ethernet_transmit(struct netif *iface, struct pbuf *packet)
{
	struct tx_descriptor *desc = tx_ring + tx_next;

	if (!netif_is_up(iface) || !netif_is_link_up(iface))
		return ERR_IF;
	if (packet->tot_len < FRAME_MIN || packet->tot_len > FRAME_MAX)
		return ERR_BUF;
	tx_reclaim();
	if (tx_outstanding == TX_RING_SIZE - 1) {
		status.tx_busy++;
		return ERR_MEM;
	}
	if (pbuf_copy_partial(packet, ring_buffer(tx_pages, tx_next), packet->tot_len, 0) !=
	    packet->tot_len)
		return ERR_BUF;
	desc->length = packet->tot_len;
	desc->status = 0;
	desc->command = TX_EOP | TX_IFCS | TX_RS;
	tx_next = (tx_next + 1) % TX_RING_SIZE;
	if (++tx_outstanding > status.tx_high_water)
		status.tx_high_water = tx_outstanding;
	return ERR_OK;
}

void ethernet_flush(void)
{
	if (tx_next == tx_posted)
		return;
	dma_fence();
	reg_write(E1000_TDT, tx_next);
	tx_posted = tx_next;
}

static err_t ethernet_init(struct netif *iface)
{
	iface->name[0] = 'e';
	iface->name[1] = 't';
	iface->hwaddr_len = sizeof(status.mac);
	memcpy(iface->hwaddr, status.mac, sizeof(status.mac));
	iface->mtu = 1500;
	iface->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
	iface->output = etharp_output;
	iface->linkoutput = ethernet_transmit;
	return ERR_OK;
}

void ethernet_start(void)
{
	unsigned long flags;

	if (!registers)
		return;
	netif_add_noaddr(&eth0, NULL, ethernet_init, ethernet_input);
	netif_set_default(&eth0);
	reg_write(E1000_RDBAL, __phy(rx_ring));
	reg_write(E1000_RDBAH, 0);
	reg_write(E1000_RDLEN, RX_RING_SIZE * sizeof(*rx_ring));
	reg_write(E1000_RDH, 0);
	dma_fence();
	reg_write(E1000_RDT, RX_RING_SIZE - 1);
	reg_write(E1000_TDBAL, __phy(tx_ring));
	reg_write(E1000_TDBAH, 0);
	reg_write(E1000_TDLEN, TX_RING_SIZE * sizeof(*tx_ring));
	reg_write(E1000_TDH, 0);
	reg_write(E1000_TDT, 0);
	reg_write(E1000_TIPG, 10U | (8U << 10) | (6U << 20));
	reg_write(E1000_TCTL, TCTL_EN | TCTL_PSP | (0x10U << 4) | (0x40U << 12));
	reg_write(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
	status.config.flags = MARIO_NET_PRESENT;
	status.rx_ring_size = RX_RING_SIZE;
	status.tx_ring_size = TX_RING_SIZE;
	spin_lock_irqsave(&irq_lock, flags);
	irq_enabled = 1;
	reg_write(E1000_IMS, E1000_EVENTS);
	spin_unlock_irqrestore(&irq_lock, flags);
}

static int ethernet_present(void)
{
	return !!(status.config.flags & MARIO_NET_PRESENT);
}

void irq_e1000(unsigned int line)
{
	unsigned long flags;
	uint32_t cause;

	if (!registers || line != status.irq)
		return;
	spin_lock_irqsave(&irq_lock, flags);
	cause = irq_enabled ? reg_read(E1000_ICR) & E1000_EVENTS : 0;
	if (cause) {
		reg_write(E1000_IMC, E1000_EVENTS);
		__atomic_fetch_or(&irq_events, cause, __ATOMIC_RELEASE);
		__atomic_add_fetch(&irq_count, 1, __ATOMIC_RELAXED);
	}
	spin_unlock_irqrestore(&irq_lock, flags);
	if (cause)
		net_kick();
}

int ethernet_stop(void *unused)
{
	unsigned long flags;

	if (!ethernet_present())
		return 0;
	spin_lock_irqsave(&irq_lock, flags);
	irq_enabled = 0;
	reg_write(E1000_IMC, ~0U);
	reg_read(E1000_ICR);
	reg_write(E1000_RCTL, 0);
	reg_write(E1000_TCTL, 0);
	reg_read(E1000_STATUS);
	spin_unlock_irqrestore(&irq_lock, flags);
	netif_set_down(&eth0);
	netif_set_link_down(&eth0);
	status.config.flags = 0;
	tx_posted = tx_next;
	return 0;
}

static void ethernet_receive(void)
{
	struct rx_descriptor *desc = rx_ring + rx_next;
	unsigned int length = desc->length;
	unsigned int end = desc->status & RX_EOP;
	struct pbuf *packet = NULL;

	if (!rx_discard && end && !desc->errors && length >= FRAME_MIN && length <= FRAME_MAX &&
	    netif_is_up(&eth0) && netif_is_link_up(&eth0)) {
		packet = pbuf_alloc(PBUF_RAW, length, PBUF_POOL);
		if (packet && pbuf_take(packet, ring_buffer(rx_pages, rx_next), length) != ERR_OK) {
			pbuf_free(packet);
			packet = NULL;
		}
	}
	rx_discard = !end;
	desc->status = 0;
	dma_fence();
	reg_write(E1000_RDT, rx_next);
	rx_next = (rx_next + 1) % RX_RING_SIZE;
	if (packet) {
		if (eth0.input(packet, &eth0) == ERR_OK) {
			status.rx_packets++;
			return;
		}
		pbuf_free(packet);
	}
	status.rx_dropped++;
}

int ethernet_poll(void)
{
	unsigned int queued = 0;
	unsigned int cursor = rx_next;

	if (!ethernet_present())
		return 0;
	if (__atomic_exchange_n(&irq_events, 0, __ATOMIC_ACQUIRE) & ICR_RXO)
		status.rx_overruns++;
	if (reg_read(E1000_STATUS) & STATUS_LU) {
		netif_set_link_up(&eth0);
		status.config.flags |= MARIO_NET_CARRIER;
	} else if (netif_is_link_up(&eth0)) {
		netif_set_link_down(&eth0);
		etharp_cleanup_netif(&eth0);
		status.config.flags &= ~MARIO_NET_CARRIER;
	}
	tx_reclaim();
	while (queued < RX_RING_SIZE &&
	       (__atomic_load_n(&rx_ring[cursor].status, __ATOMIC_ACQUIRE) & RX_DD)) {
		queued++;
		cursor = (cursor + 1) % RX_RING_SIZE;
	}
	if (queued > status.rx_high_water)
		status.rx_high_water = queued;
	for (unsigned int count = 0; count < MIN(queued, RX_BATCH); count++)
		ethernet_receive();
	reg_write(E1000_IMS, E1000_EVENTS);
	return __atomic_load_n(&rx_ring[rx_next].status, __ATOMIC_ACQUIRE) & RX_DD;
}

static int unicast(uint32_t address)
{
	return (address >> 24) > 0 && (address >> 24) != 127 && (address >> 24) < 224;
}

static int ethernet_configure(void *data)
{
	const struct mario_net_config *config = data;
	ip4_addr_t address = { .addr = config->address };
	ip4_addr_t gateway = { .addr = config->gateway };
	ip4_addr_t netmask = {0};
	uint32_t host = lwip_ntohl(config->address);
	uint32_t router = lwip_ntohl(config->gateway);
	uint32_t mask;

	if (!ethernet_present())
		return -ENODEV;
	if (config->version != MARIO_NET_VERSION || config->flags & ~MARIO_NET_UP ||
	    config->prefix > 32)
		return -EINVAL;
	mask = config->prefix ? ~0U << (32 - config->prefix) : 0;
	if (!host) {
		if (config->prefix || router)
			return -EINVAL;
	} else if (!unicast(host) || !mask ||
		   (config->prefix < 31 && (!(host & ~mask) || (host & ~mask) == ~mask))) {
		return -EINVAL;
	}
	if (router && (!unicast(router) || router == host || ((host ^ router) & mask) ||
	    (config->prefix < 31 && (!(router & ~mask) || (router & ~mask) == ~mask))))
		return -EINVAL;
	netmask.addr = lwip_htonl(mask);
	ethernet_flush();
	if (!(config->flags & MARIO_NET_UP))
		netif_set_down(&eth0);
	if (config->address != status.config.address || config->prefix != status.config.prefix ||
	    config->gateway != status.config.gateway)
		etharp_cleanup_netif(&eth0);
	netif_set_addr(&eth0, &address, &netmask, &gateway);
	if (config->flags & MARIO_NET_UP)
		netif_set_up(&eth0);
	status.config.flags = (status.config.flags & ~MARIO_NET_UP) | config->flags;
	status.config.address = config->address;
	status.config.prefix = config->prefix;
	status.config.gateway = config->gateway;
	return 0;
}

static int ethernet_status(void *data)
{
	struct mario_net_status *result = data;

	*result = status;
	result->config.version = MARIO_NET_VERSION;
	result->interrupts = __atomic_load_n(&irq_count, __ATOMIC_RELAXED);
	return 0;
}

uid_t sys_geteuid32(void);

int ethernet_ioctl(unsigned int cmd, unsigned long arg)
{
	void *user = (void *)arg;
	struct mario_net_config config;
	struct mario_net_status result;
	int error;

	switch (cmd) {
	case MARIO_NET_GET:
		if (verify_area(VERIFY_READ, user, sizeof(uint32_t)) ||
		    verify_area(VERIFY_WRITE, user, sizeof(result)))
			return -EFAULT;
		if (get_fs_long(user) != MARIO_NET_VERSION)
			return -EINVAL;
		error = net_call(ethernet_status, &result);
		if (!error)
			memcpy_tofs(user, &result, sizeof(result));
		return error;
	case MARIO_NET_SET:
		if (sys_geteuid32())
			return -EPERM;
		if (verify_area(VERIFY_READ, user, sizeof(config)))
			return -EFAULT;
		memcpy_fromfs(&config, user, sizeof(config));
		return net_call(ethernet_configure, &config);
	default:
		return -ENOTTY;
	}
}
