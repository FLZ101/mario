#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

extern int memcmp(const void *ptr1, const void *ptr2, size_t num);
extern void *memchr(const void *ptr, int c, size_t num);
extern void *memcpy(void *dst, const void *src, size_t num);
extern void *memmove(void *dst, const void *src, size_t num);
extern void *memset(void *dst, int c, size_t num);
 
extern int strcmp(const char *string1, const char *string2);
extern int strncmp(const char *string1, const char *string2, size_t num);
extern char *strchr(const char *string, int c);
extern char *strrchr(const char *string, int c);
extern char *strstr(const char *string1, const char *string2);
extern char *strcpy(char *dst, const char *src);
extern char *strncpy(char *dst, const char *src, size_t num);
extern char *strcat(char *dst, const char *src);
extern char *strncat(char *dst, const char *src, size_t num);
extern size_t strlen(const char *string);

#endif	/* _STRING_H */