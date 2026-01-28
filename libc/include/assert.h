#ifndef _ASSERT_H
#define _ASSERT_H

void exit(int status);

#define assert(expr) \
do { \
	if (!(expr)) { \
		printf("Assertion failed: %s\n", #expr); \
		printf("  at %s:%d\n", __FILE__, __LINE__); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

#endif /* _ASSERT_H */
