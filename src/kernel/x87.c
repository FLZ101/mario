#include <x87.h>
#include <lib/string.h>

_Static_assert(sizeof(struct x87_state) == 108, "32-bit FSAVE image size");

static const struct x87_state initial_state = {
	.control = 0x037f,
	.tag = 0xffff,
};

int fpu_xsave_enabled;
static struct fpu_state initial_fpu;

#define CR0_MP (1UL << 1)
#define CR0_EM (1UL << 2)
#define CR0_TS (1UL << 3)
#define CR0_NE (1UL << 5)

#define CR4_OSFXSR (1UL << 9)
#define CR4_OSXMMEXCPT (1UL << 10)
#define CR4_OSXSAVE (1UL << 18)

#define XCR0_X87 1U
#define XCR0_SSE 2U
#define XCR0_YMM 4U

static inline void native_cpuid(unsigned int *eax, unsigned int *ebx,
				unsigned int *ecx, unsigned int *edx)
{
	asm volatile("cpuid"
		: "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
		: "0"(*eax), "2"(*ecx));
}

static inline unsigned long read_cr4(void)
{
	unsigned long value;

	asm volatile("mov %%cr4, %0" : "=r"(value));
	return value;
}

static inline void write_cr4(unsigned long value)
{
	asm volatile("mov %0, %%cr4" : : "r"(value) : "memory");
}

static inline uint64_t xgetbv(unsigned int index)
{
	uint32_t eax, edx;

	asm volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
	return ((uint64_t)edx << 32) | eax;
}

static inline void xsetbv(unsigned int index, uint64_t value)
{
	uint32_t eax = value;
	uint32_t edx = value >> 32;

	asm volatile("xsetbv" : : "a"(eax), "d"(edx), "c"(index) : "memory");
}

static void fpu_build_initial(void)
{
	uint16_t *control;
	uint16_t *tag;
	uint32_t *mxcsr;

	memset(&initial_fpu, 0, sizeof(initial_fpu));
	control = (uint16_t *)initial_fpu.data;
	tag = (uint16_t *)(initial_fpu.data + 4);
	mxcsr = (uint32_t *)(initial_fpu.data + 24);
	*control = 0x037f;
	*tag = 0xffff;
	*mxcsr = 0x1f80;
	if (fpu_xsave_enabled)
		*(uint64_t *)(initial_fpu.data + 512) =
			XCR0_X87 | XCR0_SSE | XCR0_YMM;
}

void fpu_reset(void)
{
	fpu_restore(&initial_fpu);
}

void fpu_init_cpu(void)
{
	unsigned int eax = 1, ebx = 0, ecx = 0, edx = 0;
	unsigned long cr4;
	unsigned int sse2;
	unsigned int avx;
	unsigned int osxsave;

	native_cpuid(&eax, &ebx, &ecx, &edx);
	sse2 = (edx >> 26) & 1;
	avx = (ecx >> 28) & 1;
	osxsave = (ecx >> 27) & 1;

	cr4 = read_cr4();
	if (sse2)
		cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
	write_cr4(cr4);

	if (sse2 && avx && osxsave) {
		cr4 |= CR4_OSXSAVE;
		write_cr4(cr4);
		xsetbv(0, XCR0_X87 | XCR0_SSE | XCR0_YMM);

		eax = 0x0d;
		ebx = 0;
		ecx = 0;
		edx = 0;
		native_cpuid(&eax, &ebx, &ecx, &edx);
		if (ebx && ebx <= XSAVE_AREA_SIZE) {
			fpu_xsave_enabled = 1;
		} else {
			xsetbv(0, XCR0_X87 | XCR0_SSE);
			fpu_xsave_enabled = 0;
		}
	} else {
		fpu_xsave_enabled = 0;
	}

	fpu_build_initial();
	fpu_reset();
}

void x87_reset(void)
{
	x87_restore(&initial_state);
}

void x87_init_cpu(void)
{
	unsigned long cr0;

	asm volatile("mov %%cr0, %0" : "=r"(cr0));
	cr0 = (cr0 | CR0_MP | CR0_NE) & ~(CR0_EM | CR0_TS);
	asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
	x87_reset();
}
