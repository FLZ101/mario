#include "test.h"

static int a = 1, b = 0, c;

static void f(void *arg)
{
	early_print("---");
	c = a / b;
}

static void g(void *arg)
{
	early_print("+++");
	c = a / b;
}

void test_trap(void *arg)
{
	kernel_thread(f, NULL);
	kernel_thread(g, NULL);
}
