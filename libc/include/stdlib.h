#ifndef _STDLIB_H
#define _STDLIB_H

void exit(int);

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

int atexit(void (*f)(void));

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, unsigned new_size);
void *calloc(unsigned num, unsigned size);

#endif /* _STDLIB_H */
