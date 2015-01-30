#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

int memcmp(const void *ptr1, const void *ptr2, size_t num);
void *memchr(const void *ptr, int c, size_t num);
void *memcpy(void *dst, const void *src, size_t num);
void *memmove(void *dst, const void *src, size_t num);
void *memset(void *dst, int c, size_t num);
void *memsetw(void *dst, int c, size_t num);
void *memsetl(void *dst, int c, size_t num);

int strcmp(const char *string1, const char *string2);
int strncmp(const char *string1, const char *string2, size_t num);
char *strchr(const char *string, int c);
char *strrchr(const char *string, int c);
char *strstr(const char *string1, const char *string2);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t num);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t num);
char *strpbrk(const char *string1, const char *string2);
char *strtok(char *string1, const char *string2);
size_t strlen(const char *string);
size_t strspn(const char *string1, const char *string2);
size_t strcspn(const char *string1, const char *string2);

#endif	/* _STRING_H */