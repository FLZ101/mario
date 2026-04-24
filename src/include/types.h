#ifndef _TYPES_H
#define _TYPES_H

typedef signed char        __s8;
typedef signed short       __s16;
typedef signed long        __s32;
typedef signed long long   __s64;

typedef unsigned char        __u8;
typedef unsigned short       __u16;
typedef unsigned long        __u32;
typedef unsigned long long   __u64;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed long        int32_t;
typedef signed long long   int64_t;

typedef unsigned char        uint8_t;
typedef unsigned short       uint16_t;
typedef unsigned long        uint32_t;
typedef unsigned long long   uint64_t;

typedef int pid_t;

typedef long off_t;
typedef long long off64_t;

typedef int ssize_t;
typedef unsigned int size_t;

typedef int clockid_t;

typedef unsigned long ino_t;
typedef unsigned long long ino64_t;
typedef unsigned short dev_t;

typedef unsigned short mode_t;
typedef unsigned short umode_t;
typedef unsigned short nlink_t;

typedef long long time_t;
typedef long long suseconds_t;
typedef long clock_t;

typedef unsigned uid_t;
typedef unsigned gid_t;

typedef long blksize_t;
typedef __s64 blkcnt_t;

#define FD_SETSIZE 1024

typedef unsigned long fd_mask;

typedef struct {
	unsigned long fds_bits[FD_SETSIZE / 8 / sizeof(long)];
} fd_set;

#define FD_SET(fd, fdsetp) \
__asm__ __volatile__( \
	"btsl %1, %0" \
	:"=m" (*(fd_set *) (fdsetp)) \
	:"r" ((int) (fd)) \
	:)

#define FD_CLR(fd, fdsetp) \
__asm__ __volatile__( \
	"btrl %1, %0" \
	:"=m" (*(fd_set *) (fdsetp)) \
	:"r" ((int) (fd)) \
	:)

#define FD_ISSET(fd,fdsetp) (__extension__ \
({ \
	unsigned char __result; \
	__asm__ __volatile__( \
		"btl %1, %2; setb %0" \
		:"=q" (__result) \
		:"r" ((int) (fd)), "m" (*(fd_set *) (fdsetp)) \
		:); \
	__result; \
}))

#define FD_ZERO(fdsetp) \
__asm__ __volatile__( \
	"cld; rep; stosl" \
	:"=m" (*(fd_set *) (fdsetp)) \
	:"a" (0), "c" (8), "D" ((fd_set *) (fdsetp)) \
	:)

#endif	/* _TYPES_H */
