#include "test.h"

static void f(void *arg)
{
	early_print("---");
	int a = 1/0;
}

static void g(void *arg)
{
	early_print("+++");
	int a = 1/0;
}

void test_trap(void *arg)
{
	kernel_thread(f, NULL);
	kernel_thread(g, NULL);
}

