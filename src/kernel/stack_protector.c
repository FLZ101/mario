#include <types.h>
#include <printk.h>

uint32_t __stack_chk_guard;

__attribute__((noreturn, no_stack_protector))
void __stack_chk_fail(void)
{
	hang("kernel stack smashing detected");
}
