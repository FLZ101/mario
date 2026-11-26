#ifndef MARIO_LWIP_CC_H
#define MARIO_LWIP_CC_H

#include <types.h>
#include <lib/stddef.h>
#include <printk.h>

#define LWIP_NO_STDDEF_H 1
#define LWIP_NO_STDINT_H 1
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_LIMITS_H 1
#define LWIP_NO_UNISTD_H 1
#define LWIP_NO_CTYPE_H 1
#define LWIP_HAVE_INT64 1
#define INT_MAX __INT_MAX__
#define SSIZE_MAX INT_MAX

typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef uint8_t u8_t;
typedef int8_t s8_t;
typedef uint16_t u16_t;
typedef int16_t s16_t;
typedef uint32_t u32_t;
typedef int32_t s32_t;
typedef uint64_t u64_t;
typedef int64_t s64_t;
typedef unsigned long mem_ptr_t;

#define X8_F "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "lu"
#define S32_F "ld"
#define X32_F "lx"
#define SZT_F "u"

void mario_lwip_assert(const char *expr, const char *file, int line)
	__attribute__((noreturn));

#define LWIP_PLATFORM_DIAG(args) do { printk args; } while (0)
#define LWIP_PLATFORM_ASSERT(expr) mario_lwip_assert(expr, __FILE__, __LINE__)

#endif
