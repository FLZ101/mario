#ifndef _STDLIB_H
#define _STDLIB_H

void exit(int);

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);

#endif /* _STDLIB_H */
