#include <misc.h>
#include <io.h>
#include <idt.h>
#include <ldt.h>
#include <smp.h>
#include <sched.h>
#include <task.h>
#include <time.h>
#include <printk.h>
#include <lib/string.h>

#include <mm/mm.h>
#include <mm/page_alloc.h>
#include <mm/pagetable.h>

/*
 * The trampoline must live below 1MB because SIPI starts an AP in real mode.
 * This area is above the BIOS data area and Multiboot structures but below
 * the conventional-memory hole, so the running kernel can own it exclusively
 * for AP start-up.  Its physical address is hard-coded in smp_trampoline.asm.
 */
#define TRAMPOLINE_BASE		0x70000UL
#define TRAMPOLINE_ENTRY_PTR	(TRAMPOLINE_BASE + 0x240)
#define TRAMPOLINE_CR3		(TRAMPOLINE_BASE + 0x248)
#define TRAMPOLINE_STACK	(TRAMPOLINE_BASE + 0x250)

/*
 * Local APIC MMIO is outside normal RAM, so give it a dedicated fixed mapping
 * instead of sharing the single active ioremap slot with the AHCI driver.
 */
#define APIC_BASE_VIRT		0xffc00000UL
/* Temporary mapping for the ACPI tables above the E820-visible RAM top. */
#define ACPI_BASE_VIRT		0xffb00000UL

#define APIC_ID			0x020
#define APIC_EOI		0x0b0
#define APIC_SPURIOUS		0x0f0
#define APIC_ICR_LOW		0x300
#define APIC_ICR_HIGH		0x310

/* ICR delivery status: set while the previous IPI has not been accepted. */
#define APIC_DELIVERY_STATUS	(1 << 12)

/* Vectors used for SMP IPIs.  Keep them above the PIC IRQ vector range. */
#define IPI_RESCHEDULE_VECTOR	0xfb
#define IPI_TLB_FLUSH_VECTOR	0xfc

#define APIC_DEST_PHYSICAL	(0 << 11)
#define APIC_DM_INIT		(5 << 8)
#define APIC_DM_STARTUP		(6 << 8)
#define APIC_LEVEL_TRIGGER	(1 << 15)
#define APIC_ASSERT		(1 << 14)

/* ACPI RSDP/XSDT/MADT structures. */
struct acpi_rsdp {
	char signature[8];
	uint8_t checksum;
	char oem_id[6];
	uint8_t revision;
	uint32_t rsdt_address;
	uint32_t length;
	uint64_t xsdt_address;
	uint8_t extended_checksum;
	uint8_t reserved[3];
} __attribute__((gcc_struct, packed));

struct acpi_sdt_hdr {
	char signature[4];
	uint32_t length;
	uint8_t revision;
	uint8_t checksum;
	char oem_id[6];
	char oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((gcc_struct, packed));

/* Trampoline bytes and their link-time extent. */
extern char smp_trampoline_start[];
extern char smp_trampoline_end[];

/* entry.S IPI trampolines. */
extern void ipi_reschedule_entry(void);
extern void ipi_tlb_flush_entry(void);
extern void ap_reload_cs(void);

/* Logical CPU index for each APIC ID, used by APs to find their cpu_info. */
static int apicid_to_cpu[256];
static int cpu_apic_id[NR_CPUS];
static char ap_stacks[NR_CPUS][KSTACK_SIZE] __attribute__((aligned(PAGE_SIZE)));
static struct desc_struct ap_gdt[NR_CPUS][GDT_ENTRY_PERCPU + 1];
static struct task_struct idle_tasks[NR_CPUS];
static volatile uint32_t *lapic;

struct tss_struct ap_tss[NR_CPUS];
int nr_cpu_ids;

static inline void native_cpuid(unsigned int *eax, unsigned int *ebx,
				unsigned int *ecx, unsigned int *edx)
{
	__asm__ __volatile__("cpuid"
			     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
			     : "0"(*eax), "2"(*ecx));
}

static int boot_cpu_apic_id(void)
{
	unsigned int eax = 1, ebx, ecx = 0, edx;

	native_cpuid(&eax, &ebx, &ecx, &edx);
	return (ebx >> 24) & 0xff;
}

static unsigned char acpi_checksum(const void *start, int len)
{
	const unsigned char *p = start;
	unsigned char sum = 0;

	while (len--)
		sum += *p++;
	return sum;
}

static int acpi_rsdp_valid(const struct acpi_rsdp *rsdp)
{
	if (memcmp(rsdp->signature, "RSD PTR ", 8))
		return 0;
	if (acpi_checksum(rsdp, 20) != 0)
		return 0;
	if (rsdp->revision >= 2 &&
	    acpi_checksum(rsdp, sizeof(*rsdp)) != 0)
		return 0;
	return 1;
}

static struct acpi_rsdp *acpi_find_rsdp(void)
{
	uint16_t ebda_seg = *(uint16_t *)__vir(0x40e);
	unsigned long ebda = ((unsigned long)ebda_seg) << 4;

	if (0x400 <= ebda && ebda < 0xa0000) {
		for (unsigned long p = ebda; p < ebda + 1024; p += 16) {
			struct acpi_rsdp *rsdp = (struct acpi_rsdp *)__vir(p);

			if (acpi_rsdp_valid(rsdp))
				return rsdp;
		}
	}

	for (unsigned long p = 0xe0000; p < 0x100000; p += 16) {
		struct acpi_rsdp *rsdp = (struct acpi_rsdp *)__vir(p);

		if (acpi_rsdp_valid(rsdp))
			return rsdp;
	}

	return NULL;
}

static uint64_t acpi_rsdp_address(void)
{
	struct acpi_rsdp *rsdp = acpi_find_rsdp();

	return rsdp ? __phy(rsdp) : 0;
}

static int acpi_table_valid(const struct acpi_sdt_hdr *hdr, const char *sig)
{
	if (memcmp(hdr->signature, sig, 4))
		return 0;
	return acpi_checksum(hdr, hdr->length) == 0;
}

/*
 * Map one physical page at a fixed kernel address.  ACPI tables often live
 * above the E820 RAM range reported to Multiboot (QEMU places the RSDT just
 * past the usable-memory top), so they are not covered by the direct map.
 */
static void *acpi_map_page(unsigned long phys)
{
	unsigned long virt = ACPI_BASE_VIRT;
	pde_t *pde = pde_offset(swapper_pg_dir, virt);
	pte_t *pt;

	if (!__pe_present(pde)) {
		pt = (pte_t *)alloc_pt();
		if (!pt)
			return NULL;
		set_pde(pde, mk_pde(pt, _PDE));
	} else {
		pt = pte_offset(__vir(*pde), virt);
	}

	set_pte(pte_offset(pt, virt),
		__mk_pte(phys & PAGE_MASK, PG_PRESENT));
	flush_tlb();
	return (void *)(virt + (phys & ~PAGE_MASK));
}

static struct acpi_sdt_hdr *acpi_find_table(const char *sig)
{
	uint64_t rsdp_phys = acpi_rsdp_address();
	struct acpi_rsdp *rsdp;
	struct acpi_sdt_hdr *root;
	uint64_t entries[64];
	int nr;

	if (!rsdp_phys)
		return NULL;
	rsdp = acpi_map_page(rsdp_phys);
	if (!rsdp)
		return NULL;
	if (!acpi_rsdp_valid(rsdp))
		return NULL;

	if (rsdp->revision >= 2 && rsdp->xsdt_address) {
		root = acpi_map_page(rsdp->xsdt_address);
		if (!root || !acpi_table_valid(root, "XSDT"))
			return NULL;
		nr = (root->length - sizeof(*root)) / 8;
		if (nr > (int)(sizeof(entries) / sizeof(entries[0])))
			nr = sizeof(entries) / sizeof(entries[0]);
		for (int i = 0; i < nr; i++)
			entries[i] = ((uint64_t *)(root + 1))[i];
	} else {
		root = acpi_map_page(rsdp->rsdt_address);
		if (!root || !acpi_table_valid(root, "RSDT"))
			return NULL;
		nr = (root->length - sizeof(*root)) / 4;
		if (nr > (int)(sizeof(entries) / sizeof(entries[0])))
			nr = sizeof(entries) / sizeof(entries[0]);
		for (int i = 0; i < nr; i++)
			entries[i] = ((uint32_t *)(root + 1))[i];
	}

	for (int i = 0; i < nr; i++) {
		struct acpi_sdt_hdr *hdr = acpi_map_page(entries[i]);

		if (hdr && acpi_table_valid(hdr, sig))
			return hdr;
	}

	return NULL;
}

static unsigned long acpi_madt_lapic(const struct acpi_sdt_hdr *madt)
{
	uint32_t addr = *(uint32_t *)(madt + 1);

	return addr ? addr : 0xfee00000UL;
}

static int acpi_madt_parse(const struct acpi_sdt_hdr *madt)
{
	uint8_t *p = (uint8_t *)madt + 44;
	uint8_t *end = (uint8_t *)madt + madt->length;
	int nr = 0;

	while (p < end) {
		uint8_t type = p[0];
		uint8_t len = p[1];
		int enabled = 0;
		int apic_id = -1;

		if (len < 2)
			break;
		if (type == 0 && len >= 8) {
			enabled = *(uint32_t *)(p + 4) & 1;
			apic_id = p[3];
		} else if (type == 9 && len >= 16) {
			enabled = *(uint32_t *)(p + 8) & 1;
			apic_id = *(uint32_t *)(p + 4);
		}

		if (enabled && apic_id >= 0 && nr < NR_CPUS)
			cpu_apic_id[nr++] = apic_id;
		p += len;
	}

	return nr;
}

/*
 * Install a single kernel-only page for the local APIC at APIC_BASE_VIRT.
 * This mapping is intentionally not managed by ioremap(): the AHCI driver
 * already holds that one-shot mapping.
 */
static void apic_map(unsigned long phys)
{
	unsigned long virt = APIC_BASE_VIRT;
	pde_t *pde = pde_offset(swapper_pg_dir, virt);
	pte_t *pt;

	if (!__pe_present(pde)) {
		pt = (pte_t *)get_zero_page();
		set_pde(pde, mk_pde(pt, _PDE));
	} else {
		pt = pte_offset(__vir(*pde), virt);
	}

	set_pte(pte_offset(pt, virt),
		__mk_pte(phys & PAGE_MASK, PG_PRESENT | PG_RW));
	flush_tlb();
}

/*
 * The trampoline runs from low physical memory and turns paging on before
 * the final far jump to the high kernel entry point.  Keep the trampoline
 * page identity-mapped so that the instruction fetch immediately after
 * setting CR0.PG succeeds.  paging_init() deliberately unmaps low memory
 * to catch NULL dereferences, so this mapping must be recreated here.
 */
static void trampoline_map(void)
{
	unsigned long virt = TRAMPOLINE_BASE;
	pde_t *pde = pde_offset(swapper_pg_dir, virt);
	pte_t *pt;

	if (!__pe_present(pde)) {
		pt = (pte_t *)get_zero_page();
		set_pde(pde, mk_pde(pt, _PDE));
	} else {
		pt = pte_offset(__vir(*pde), virt);
	}

	set_pte(pte_offset(pt, virt),
		__mk_pte(TRAMPOLINE_BASE, PG_PRESENT | PG_RW));
	flush_tlb();
}

/*
 * Once every AP is online it executes only in the high kernel mapping.
 * Remove the temporary identity page so normal low-address protection and
 * process page-table copying do not see the trampoline as a user mapping.
 */
static void trampoline_unmap(void)
{
	pde_t *pde = pde_offset(swapper_pg_dir, TRAMPOLINE_BASE);

	if (__pe_present(pde)) {
		pte_t *pt = pte_offset(__vir(*pde), TRAMPOLINE_BASE);
		set_pde(pde, 0);
		free_pt(pt);
		flush_tlb();
	}
}

static uint32_t apic_read(unsigned int reg)
{
	return lapic[reg / 4];
}

static void apic_write(unsigned int reg, uint32_t value)
{
	lapic[reg / 4] = value;
}

static void apic_eoi(void)
{
	apic_write(APIC_EOI, 0);
}

static void send_ipi(int cpu, unsigned int vector)
{
	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_info[cpu].online)
		return;
	if (cpu == current->cpu)
		return;

	/*
	 * The local APIC has only one ICR.  If a previous IPI is still being
	 * delivered, overwriting the ICR can silently drop it.  Wait for the
	 * delivery-status bit to clear before composing the next message.
	 */
	while (apic_read(APIC_ICR_LOW) & APIC_DELIVERY_STATUS)
		__asm__ __volatile__("pause");

	/* Physical destination mode puts the APIC ID in the ICR high word. */
	apic_write(APIC_ICR_HIGH, cpu_apic_id[cpu] << 24);
	apic_write(APIC_ICR_LOW, vector | APIC_DEST_PHYSICAL);
}

void smp_send_reschedule(int cpu)
{
	send_ipi(cpu, IPI_RESCHEDULE_VECTOR);
}

void smp_send_tlb_flush(int cpu)
{
	send_ipi(cpu, IPI_TLB_FLUSH_VECTOR);
}

/* Called from entry.S for the reschedule and TLB-shootdown IPIs. */
void do_reschedule_ipi(void)
{
	apic_eoi();
	current->need_resched = 1;
}

void do_tlb_flush_ipi(void)
{
	apic_eoi();
	flush_tlb();
}

struct desc_struct *cpu_gdt(int cpu)
{
	return cpu == 0 ? gdt : ap_gdt[cpu];
}

struct task_struct *smp_idle_task(int cpu)
{
	return cpu == 0 ? &init_task : &idle_tasks[cpu];
}

static int lapic_id(void)
{
	return apic_read(APIC_ID) >> 24;
}

static void apic_delay_one_tick(void)
{
	volatile long start = jiffies;

	while (jiffies == start)
		__asm__ __volatile__("pause");
}

static void apic_init_ipi(int apic_id)
{
	apic_write(APIC_ICR_HIGH, apic_id << 24);
	apic_write(APIC_ICR_LOW,
		   APIC_DM_INIT | APIC_LEVEL_TRIGGER | APIC_ASSERT);
	apic_delay_one_tick();
	apic_write(APIC_ICR_LOW, APIC_DM_INIT | APIC_LEVEL_TRIGGER);
	apic_delay_one_tick();
}

static void apic_startup_ipi(int apic_id)
{
	unsigned int vector = TRAMPOLINE_BASE >> 12;

	apic_write(APIC_ICR_HIGH, apic_id << 24);
	apic_write(APIC_ICR_LOW, APIC_DM_STARTUP | vector);
	apic_delay_one_tick();
	apic_write(APIC_ICR_LOW, APIC_DM_STARTUP | vector);
}

static void ap_gdt_install(int cpu)
{
	struct {
		uint16_t limit;
		unsigned long base;
	} __attribute__((gcc_struct, packed)) gdtr;
	struct desc_struct *tss_desc = &ap_gdt[cpu][1];
	struct desc_struct *per_cpu_desc = &ap_gdt[cpu][GDT_ENTRY_PERCPU];

	memcpy(ap_gdt[cpu], gdt, sizeof(ap_gdt[cpu]));

	/*
	 * Retarget the AP's TSS descriptor to its private TSS.  All CPUs share
	 * selector 0x08, but the descriptor base must differ so interrupts do
	 * not load the BSP's ring-0 stack pointer on an AP.
	 */
	tss_desc->limit0 = 0x67;
	tss_desc->base0 = (unsigned long)&ap_tss[cpu] & 0xffff;
	tss_desc->base1 = ((unsigned long)&ap_tss[cpu] >> 16) & 0xff;
	tss_desc->type = 0x9;
	tss_desc->s = 0;
	tss_desc->dpl = 0;
	tss_desc->p = 1;
	tss_desc->limit1 = 0;
	tss_desc->avl = 0;
	tss_desc->l = 0;
	tss_desc->d = 0;
	tss_desc->g = 0;
	tss_desc->base2 = ((unsigned long)&ap_tss[cpu] >> 24) & 0xff;

	/*
	 * Point the per-CPU descriptor at this AP's own cpu_info slot.  The
	 * selector remains KERNEL_PERCPU_DS on every CPU; only the descriptor
	 * base differs, so get_current() naturally becomes CPU-local.
	 */
	per_cpu_desc->limit0 = 0xffff;
	per_cpu_desc->base0 = (unsigned long)&cpu_info[cpu] & 0xffff;
	per_cpu_desc->base1 = ((unsigned long)&cpu_info[cpu] >> 16) & 0xff;
	per_cpu_desc->type = 0x2;
	per_cpu_desc->s = 1;
	per_cpu_desc->dpl = 0;
	per_cpu_desc->p = 1;
	per_cpu_desc->limit1 = 0xf;
	per_cpu_desc->avl = 0;
	per_cpu_desc->l = 0;
	per_cpu_desc->d = 1;
	per_cpu_desc->g = 1;
	per_cpu_desc->base2 = ((unsigned long)&cpu_info[cpu] >> 24) & 0xff;

	gdtr.limit = sizeof(ap_gdt[cpu]) - 1;
	gdtr.base = (unsigned long)ap_gdt[cpu];
	/*
	 * The descriptor-table stores above are ordinary memory writes.  The
	 * memory clobber prevents the compiler from moving those writes across
	 * LGDT (or the segment loads below) at -O3; otherwise the AP can load a
	 * partially initialised GDT and fault intermittently before it reaches
	 * the idle loop.
	 */
	__asm__ __volatile__("lgdt (%0)" : : "r"(&gdtr) : "memory");
	unsigned short kernel_ds = KERNEL_DS;
	unsigned short per_cpu_ds = KERNEL_PERCPU_DS;
	load_segment(ds, kernel_ds);
	load_segment(es, kernel_ds);
	load_segment(ss, kernel_ds);
	load_segment(fs, kernel_ds);
	load_segment(gs, per_cpu_ds);

	/* The trampoline left CS as its temporary 0x08 code selector.  In this
	 * final GDT 0x08 is the TSS, so switch to the real kernel code selector
	 * before any interrupt return reloads CS from a saved frame. */
	ap_reload_cs();

	/* The IDT is shared, but IDTR is per CPU and must be loaded here. */
	load_idt();
	/* LTR reads the TSS descriptor from the just-installed GDT, so it also
	 * needs a memory clobber to keep the descriptor stores visible. */
	__asm__ __volatile__("ltr %0" : : "r"((unsigned short)0x08) : "memory");
}

void smp_ap_entry(void)
{
	int apic_id = lapic_id();
	int cpu = apicid_to_cpu[apic_id];
	struct task_struct *idle = &idle_tasks[cpu];

	ap_gdt_install(cpu);

	memset(idle, 0, sizeof(*idle));
	idle->state = TASK_RUNNING;
	idle->cpu = cpu;
	idle->cpus_allowed = 1UL << cpu;
	idle->mm = init_task.mm;
	idle->thread.fs = KERNEL_DS;
	idle->thread.gs = KERNEL_DS;
	idle->thread.esp0 = (unsigned long)ap_stacks[cpu] + KSTACK_SIZE;
	idle->thread.esp = idle->thread.esp0;
	idle->run_list.next = NULL;
	idle->run_list.prev = NULL;

	/* An AP's TSS supplies the ring-0 stack for its IPI handlers. */
	memset(&ap_tss[cpu], 0, sizeof(ap_tss[cpu]));
	ap_tss[cpu].esp0 = idle->thread.esp0;
	ap_tss[cpu].ss0 = KERNEL_DS;

	cpu_info[cpu].id = cpu;
	cpu_info[cpu].current_task = idle;
	cpu_info[cpu].online = 1;

	/* Enable this CPU's local APIC so reschedule/TLB IPIs can be delivered. */
	apic_write(APIC_SPURIOUS, apic_read(APIC_SPURIOUS) | 0x1ff);

	/* APs now participate in the per-CPU scheduler. */
	ap_cpu_idle();
}

static void copy_trampoline_and_patch(void)
{
	size_t size = smp_trampoline_end - smp_trampoline_start;
	unsigned long dst = __vir(TRAMPOLINE_BASE);

	memcpy((void *)dst, smp_trampoline_start, size);

	/*
	 * The trampoline contains placeholder data at fixed physical offsets.
	 * Fill them with values that only the kernel link can know.
	 */
	*(uint32_t *)__vir(TRAMPOLINE_ENTRY_PTR) = (uint32_t)smp_ap_entry;
	/*
	 * The final far jump still executes from the temporary trampoline GDT.
	 * Use that GDT 0x08 code selector; KERNEL_CS is 0x10, which is the
	 * temporary data selector and would fault here.
	 */
	*(uint16_t *)__vir(TRAMPOLINE_ENTRY_PTR + 4) = 0x08;
	*(uint32_t *)__vir(TRAMPOLINE_CR3) = __phy(swapper_pg_dir);
	*(uint32_t *)__vir(TRAMPOLINE_STACK) =
		(unsigned long)ap_stacks[0] + KSTACK_SIZE;
}

static void boot_aps(int nr)
{
	pr_info("cpu 0 online\n");

	for (int cpu = 1; cpu < nr; cpu++) {
		int apic_id = cpu_apic_id[cpu];
		volatile int waited = 0;

		*(uint32_t *)__vir(TRAMPOLINE_STACK) =
			(unsigned long)ap_stacks[cpu] + KSTACK_SIZE;
		cpu_info[cpu].online = 0;

		apic_init_ipi(apic_id);
		apic_startup_ipi(apic_id);
		while (!cpu_info[cpu].online && waited++ < 100000)
			__asm__ __volatile__("pause");

		if (cpu_info[cpu].online)
			pr_info("cpu %d online\n", cpu);
		else
			pr_warn("cpu %d failed to come online\n", cpu);
	}

	nr_cpu_ids = 0;
	for (int cpu = 0; cpu < nr; cpu++)
		if (cpu_info[cpu].online)
			nr_cpu_ids++;
}

void smp_init(void)
{
	struct acpi_sdt_hdr *madt = NULL;
	unsigned long lapic_addr = 0xfee00000UL;
	int nr = 0, bsp, logical = 1;

	madt = acpi_find_table("APIC");
	if (madt) {
		nr = acpi_madt_parse(madt);
		lapic_addr = acpi_madt_lapic(madt);
	}
	if (nr <= 1) {
		pr_info("cpu 0 online\n");
		nr_cpu_ids = 1;
		return;
	}

	memset(apicid_to_cpu, 0, sizeof(apicid_to_cpu));
	bsp = boot_cpu_apic_id();
	apicid_to_cpu[bsp] = 0;
	cpu_apic_id[0] = bsp;

	/*
	 * The MADT may list processors in any order.  Rebuild a logical order
	 * with the BSP first and only the remaining enabled CPUs after it.
	 */
	for (int i = 0; i < nr && logical < NR_CPUS; i++) {
		int apic_id = cpu_apic_id[i];

		if (apic_id == bsp)
			continue;
		cpu_apic_id[logical] = apic_id;
		apicid_to_cpu[apic_id] = logical;
		logical++;
	}

	nr = logical;
	apic_map(lapic_addr);
	lapic = (volatile uint32_t *)APIC_BASE_VIRT;
	/* Enable the local APIC with the lowest-priority spurious vector. */
	apic_write(APIC_SPURIOUS, apic_read(APIC_SPURIOUS) | 0x1ff);
	set_intr_gate(IPI_RESCHEDULE_VECTOR, ipi_reschedule_entry);
	set_intr_gate(IPI_TLB_FLUSH_VECTOR, ipi_tlb_flush_entry);
	trampoline_map();
	copy_trampoline_and_patch();
	boot_aps(nr);
	trampoline_unmap();
}
