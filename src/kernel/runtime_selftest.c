#include <types.h>
#include <printk.h>

#include <mm/kmalloc.h>
#include <mm/page_alloc.h>

#if defined(CONFIG_KMALLOC_DEBUG) && defined(CONFIG_KMALLOC_SELF_TEST)
__attribute__((noinline))
static void kmalloc_selftest(void)
{
	void *p = kmalloc(16);

	kfree(p);
	kfree(p);
}
#endif

#if defined(CONFIG_PAGE_ALLOC_DEBUG) && defined(CONFIG_PAGE_ALLOC_SELF_TEST)
__attribute__((noinline))
static void page_alloc_selftest(void)
{
	unsigned long p = pages_alloc(0);

	pages_free(p, 0);
	pages_free(p, 0);
}
#endif

#if defined(CONFIG_STACK_PROTECTOR_SELF_TEST)
__attribute__((noinline))
static void stack_protector_selftest(void)
{
	char buf[16];
	volatile char *p = (volatile char *)buf;

	__asm__ __volatile__("movb $0, 16(%0)" : : "r"(p) : "memory");
}
#endif

#if defined(CONFIG_UBSAN) && defined(CONFIG_UBSAN_SELF_TEST)
__attribute__((noinline))
static void ubsan_selftest(void)
{
	volatile unsigned long bad = ~0UL;
	volatile char *p = (volatile char *)bad;

	p = p + 1;
	__asm__ __volatile__("" : : "r"(p) : "memory");
}
#endif

void runtime_selftest(void)
{
#if defined(CONFIG_KMALLOC_DEBUG) && defined(CONFIG_KMALLOC_SELF_TEST)
	kmalloc_selftest();
#endif
#if defined(CONFIG_PAGE_ALLOC_DEBUG) && defined(CONFIG_PAGE_ALLOC_SELF_TEST)
	page_alloc_selftest();
#endif
#if defined(CONFIG_STACK_PROTECTOR_SELF_TEST)
	stack_protector_selftest();
#endif
#if defined(CONFIG_UBSAN) && defined(CONFIG_UBSAN_SELF_TEST)
	ubsan_selftest();
#endif
}
