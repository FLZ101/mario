#ifndef _PRINTK_H
#define _PRINTK_H

#include <types.h>
#include <lib/stdarg.h>

int printk(const char *fmt, ...);
int sprintk(char *buf, const char *fmt, ...);
int sprintkn(char *buf, size_t size, const char *fmt, ...);
int vsprintkn(char *buf, size_t size, const char *fmt, va_list ap);

/*
 * Kernel message priorities.  The numeric prefixes are parsed by printk()
 * so callers can pass KERN_ERR "..." directly without leaking "<N>" into the
 * stored or displayed text.
 */
#define KERN_EMERG   "<0>"
#define KERN_ALERT   "<1>"
#define KERN_CRIT    "<2>"
#define KERN_ERR     "<3>"
#define KERN_WARNING "<4>"
#define KERN_NOTICE  "<5>"
#define KERN_INFO    "<6>"
#define KERN_DEBUG   "<7>"

#define pr_emerg(fmt, ...)  printk(KERN_EMERG fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...)  printk(KERN_ALERT fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...)   printk(KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)    printk(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)   printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_warning(fmt, ...) pr_warn(fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...) printk(KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)   printk(KERN_INFO fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)  printk(KERN_DEBUG fmt, ##__VA_ARGS__)

/*
 * Task-context helpers.  These must only be called where current is valid,
 * such as syscalls and process-lifecycle paths, not from interrupt handlers.
 */
#define pr_task_err(fmt, ...) \
	pr_err("pid %d %s: " fmt, current->pid, current->comm, ##__VA_ARGS__)
#define pr_task_warn(fmt, ...) \
	pr_warn("pid %d %s: " fmt, current->pid, current->comm, ##__VA_ARGS__)
#define pr_task_info(fmt, ...) \
	pr_info("pid %d %s: " fmt, current->pid, current->comm, ##__VA_ARGS__)
#define pr_task_debug(fmt, ...) \
	pr_debug("pid %d %s: " fmt, current->pid, current->comm, ##__VA_ARGS__)

#define hang(...) do { \
	printk(KERN_CRIT __VA_ARGS__); \
	__asm__ __volatile__ ("cli; 1:hlt; jmp 1b"); \
} while (0)

#define unreachable() \
	hang("unreachable"); \
	__builtin_unreachable()

#define assert(expr) do { \
	if (!(expr)) { \
		hang("Assertion '" #expr "' failed in %s() @ %s:%d", \
			__func__, __FILE__, __LINE__); \
	} \
} while (0)

#endif /* _PRINTK_H */
