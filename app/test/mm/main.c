#include <unistd.h>
#include <sys/mman.h>
#include <stdio.h>

// musl libc's sbrk always return -1, except for `sbrk(0)`.
//
// Avoid using `brk()` and `sbrk()`: the `malloc(3)` memory allocation package is the
// portable and comfortable way of allocating memory.
//
void test_sbrk()
{
	puts("------ test_sbrk");

	for (int i = 0; i < 3; ++i) {
		void *addr = sbrk(0x0800);
		printf("%x ", addr);
	}
	puts("");

	for (int i = 0; i < 3; ++i) {
		void *addr = sbrk(-0x0800);
		printf("%x ", addr);
	}
	puts("");
}

void test_linked_list();

int main(int argc, char *argv[], char *envp[])
{
	test_sbrk();
	test_linked_list();
}
