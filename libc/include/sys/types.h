#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

typedef signed char        __s8;
typedef signed short       __s16;
typedef signed long        __s32;
typedef signed long long   __s64;

typedef unsigned char        __u8;
typedef unsigned short       __u16;
typedef unsigned long        __u32;
typedef unsigned long long   __u64;

typedef int pid_t;

typedef long off_t;
// typedef long long loff_t;
typedef long loff_t;

typedef unsigned int size_t;

typedef unsigned long ino_t;
typedef unsigned short dev_t;

typedef unsigned short mode_t;
typedef unsigned short umode_t;
typedef unsigned short nlink_t;

typedef long time_t;
typedef long clock_t;

#endif	/* _SYS_TYPES_H */