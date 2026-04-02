#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <errno.h>

#define SYS_restart_syscall      0
#define SYS_exit		  1
#define SYS_fork		  2
#define SYS_read		  3
#define SYS_write		  4
#define SYS_open		  5
#define SYS_close		  6
#define SYS_waitpid		  7
#define SYS_creat		  8
#define SYS_link		  9
#define SYS_unlink		 10
#define SYS_execve		 11
#define SYS_chdir		 12
#define SYS_time		 13
#define SYS_mknod		 14
#define SYS_chmod		 15
#define SYS_lchown		 16
#define SYS_break		 17
#define SYS_oldstat		 18
#define SYS_lseek		 19
#define SYS_getpid		 20
#define SYS_mount		 21
#define SYS_umount		 22
#define SYS_setuid		 23
#define SYS_getuid		 24
#define SYS_stime		 25
#define SYS_ptrace		 26
#define SYS_alarm		 27
#define SYS_oldfstat		 28
#define SYS_pause		 29
#define SYS_utime		 30
#define SYS_stty		 31
#define SYS_gtty		 32
#define SYS_access		 33
#define SYS_nice		 34
#define SYS_ftime		 35
#define SYS_sync		 36
#define SYS_kill		 37
#define SYS_rename		 38
#define SYS_mkdir		 39
#define SYS_rmdir		 40
#define SYS_dup		 41
#define SYS_pipe		 42
#define SYS_times		 43
#define SYS_prof		 44
#define SYS_brk		 45
#define SYS_setgid		 46
#define SYS_getgid		 47
#define SYS_signal		 48
#define SYS_geteuid		 49
#define SYS_getegid		 50
#define SYS_acct		 51
#define SYS_umount2		 52
#define SYS_lock		 53
#define SYS_ioctl		 54
#define SYS_fcntl		 55
#define SYS_mpx		 56
#define SYS_setpgid		 57
#define SYS_ulimit		 58
#define SYS_oldolduname	 59
#define SYS_umask		 60
#define SYS_chroot		 61
#define SYS_ustat		 62
#define SYS_dup2		 63
#define SYS_getppid		 64
#define SYS_getpgrp		 65
#define SYS_setsid		 66
#define SYS_sigaction		 67
#define SYS_sgetmask		 68
#define SYS_ssetmask		 69
#define SYS_setreuid		 70
#define SYS_setregid		 71
#define SYS_sigsuspend		 72
#define SYS_sigpending		 73
#define SYS_sethostname	 74
#define SYS_setrlimit		 75
#define SYS_getrlimit		 76   /* Back compatible 2Gig limited rlimit */
#define SYS_getrusage		 77
#define SYS_gettimeofday_time32	 78
#define SYS_settimeofday_time32	 79
#define SYS_getgroups		 80
#define SYS_setgroups		 81
#define SYS_select		 82
#define SYS_symlink		 83
#define SYS_oldlstat		 84
#define SYS_readlink		 85
#define SYS_uselib		 86
#define SYS_swapon		 87
#define SYS_reboot		 88
#define SYS_readdir		 89
#define SYS_mmap		 90
#define SYS_munmap		 91
#define SYS_truncate		 92
#define SYS_ftruncate		 93
#define SYS_fchmod		 94
#define SYS_fchown		 95
#define SYS_getpriority	 96
#define SYS_setpriority	 97
#define SYS_profil		 98
#define SYS_statfs		99
#define SYS_fstatfs		100
#define SYS_ioperm		101
#define SYS_socketcall		102
#define SYS_syslog		103
#define SYS_setitimer		104
#define SYS_getitimer		105
#define SYS_stat		106
#define SYS_lstat		107
#define SYS_fstat		108
#define SYS_olduname		109
#define SYS_iopl		110
#define SYS_vhangup		111
#define SYS_idle		112
#define SYS_vm86old		113
#define SYS_wait4		114
#define SYS_swapoff		115
#define SYS_sysinfo		116
#define SYS_ipc		117
#define SYS_fsync		118
#define SYS_sigreturn		119
#define SYS_clone		120
#define SYS_setdomainname	121
#define SYS_uname		122
#define SYS_modify_ldt		123
#define SYS_adjtimex		124
#define SYS_mprotect		125
#define SYS_sigprocmask	126
#define SYS_create_module	127
#define SYS_init_module	128
#define SYS_delete_module	129
#define SYS_get_kernel_syms	130
#define SYS_quotactl		131
#define SYS_getpgid		132
#define SYS_fchdir		133
#define SYS_bdflush		134
#define SYS_sysfs		135
#define SYS_personality	136
#define SYS_afs_syscall	137
#define SYS_setfsuid		138
#define SYS_setfsgid		139
#define SYS__llseek		140
#define SYS_getdents		141
#define SYS__newselect		142
#define SYS_flock		143
#define SYS_msync		144
#define SYS_readv		145
#define SYS_writev		146
#define SYS_getsid		147
#define SYS_fdatasync		148
#define SYS__sysctl		149
#define SYS_mlock		150
#define SYS_munlock		151
#define SYS_mlockall		152
#define SYS_munlockall		153
#define SYS_sched_setparam		154
#define SYS_sched_getparam		155
#define SYS_sched_setscheduler		156
#define SYS_sched_getscheduler		157
#define SYS_sched_yield		158
#define SYS_sched_get_priority_max	159
#define SYS_sched_get_priority_min	160
#define SYS_sched_rr_get_interval	161
#define SYS_nanosleep		162
#define SYS_mremap		163
#define SYS_setresuid		164
#define SYS_getresuid		165
#define SYS_vm86		166
#define SYS_query_module	167
#define SYS_poll		168
#define SYS_nfsservctl		169
#define SYS_setresgid		170
#define SYS_getresgid		171
#define SYS_prctl              172
#define SYS_rt_sigreturn	173
#define SYS_rt_sigaction	174
#define SYS_rt_sigprocmask	175
#define SYS_rt_sigpending	176
#define SYS_rt_sigtimedwait	177
#define SYS_rt_sigqueueinfo	178
#define SYS_rt_sigsuspend	179
#define SYS_pread64		180
#define SYS_pwrite64		181
#define SYS_chown		182
#define SYS_getcwd		183
#define SYS_capget		184
#define SYS_capset		185
#define SYS_sigaltstack	186
#define SYS_sendfile		187
#define SYS_getpmsg		188
#define SYS_putpmsg		189
#define SYS_vfork		190
#define SYS_ugetrlimit		191
#define SYS_mmap2		192
#define SYS_truncate64		193
#define SYS_ftruncate64	194
#define SYS_stat64		195
#define SYS_lstat64		196
#define SYS_fstat64		197
#define SYS_lchown32		198
#define SYS_getuid32		199
#define SYS_getgid32		200
#define SYS_geteuid32		201
#define SYS_getegid32		202
#define SYS_setreuid32		203
#define SYS_setregid32		204
#define SYS_getgroups32	205
#define SYS_setgroups32	206
#define SYS_fchown32		207
#define SYS_setresuid32	208
#define SYS_getresuid32	209
#define SYS_setresgid32	210
#define SYS_getresgid32	211
#define SYS_chown32		212
#define SYS_setuid32		213
#define SYS_setgid32		214
#define SYS_setfsuid32		215
#define SYS_setfsgid32		216
#define SYS_pivot_root		217
#define SYS_mincore		218
#define SYS_madvise		219
#define SYS_getdents64		220
#define SYS_fcntl64		221
#define SYS_gettid		224
#define SYS_readahead		225
#define SYS_setxattr		226
#define SYS_lsetxattr		227
#define SYS_fsetxattr		228
#define SYS_getxattr		229
#define SYS_lgetxattr		230
#define SYS_fgetxattr		231
#define SYS_listxattr		232
#define SYS_llistxattr		233
#define SYS_flistxattr		234
#define SYS_removexattr	235
#define SYS_lremovexattr	236
#define SYS_fremovexattr	237
#define SYS_tkill		238
#define SYS_sendfile64		239
#define SYS_futex		240
#define SYS_sched_setaffinity	241
#define SYS_sched_getaffinity	242
#define SYS_set_thread_area	243
#define SYS_get_thread_area	244
#define SYS_io_setup		245
#define SYS_io_destroy		246
#define SYS_io_getevents	247
#define SYS_io_submit		248
#define SYS_io_cancel		249
#define SYS_fadvise64		250
#define SYS_exit_group		252
#define SYS_lookup_dcookie	253
#define SYS_epoll_create	254
#define SYS_epoll_ctl		255
#define SYS_epoll_wait		256
#define SYS_remap_file_pages	257
#define SYS_set_tid_address	258
#define SYS_timer_create	259
#define SYS_timer_settime32	260
#define SYS_timer_gettime32	261
#define SYS_timer_getoverrun	262
#define SYS_timer_delete	263
#define SYS_clock_settime32	264
#define SYS_clock_gettime32	265
#define SYS_clock_getres_time32	266
#define SYS_clock_nanosleep_time32	267
#define SYS_statfs64		268
#define SYS_fstatfs64		269
#define SYS_tgkill		270
#define SYS_utimes		271
#define SYS_fadvise64_64	272
#define SYS_vserver		273
#define SYS_mbind		274
#define SYS_get_mempolicy	275
#define SYS_set_mempolicy	276
#define SYS_mq_open 		277
#define SYS_mq_unlink		278
#define SYS_mq_timedsend	279
#define SYS_mq_timedreceive	280
#define SYS_mq_notify		281
#define SYS_mq_getsetattr	282
#define SYS_kexec_load		283
#define SYS_waitid		284
#define SYS_sys_setaltroot	285
#define SYS_add_key		286
#define SYS_request_key	287
#define SYS_keyctl		288
#define SYS_ioprio_set		289
#define SYS_ioprio_get		290
#define SYS_inotify_init	291
#define SYS_inotify_add_watch	292
#define SYS_inotify_rm_watch	293
#define SYS_migrate_pages	294
#define SYS_openat		295
#define SYS_mkdirat		296
#define SYS_mknodat		297
#define SYS_fchownat		298
#define SYS_futimesat		299
#define SYS_fstatat64		300
#define SYS_unlinkat		301
#define SYS_renameat		302
#define SYS_linkat		303
#define SYS_symlinkat		304
#define SYS_readlinkat		305
#define SYS_fchmodat		306
#define SYS_faccessat		307
#define SYS_pselect6		308
#define SYS_ppoll		309
#define SYS_unshare		310
#define SYS_set_robust_list	311
#define SYS_get_robust_list	312
#define SYS_splice		313
#define SYS_sync_file_range	314
#define SYS_tee		315
#define SYS_vmsplice		316
#define SYS_move_pages		317
#define SYS_getcpu		318
#define SYS_epoll_pwait	319
#define SYS_utimensat		320
#define SYS_signalfd		321
#define SYS_timerfd_create	322
#define SYS_eventfd		323
#define SYS_fallocate		324
#define SYS_timerfd_settime32	325
#define SYS_timerfd_gettime32	326
#define SYS_signalfd4		327
#define SYS_eventfd2		328
#define SYS_epoll_create1	329
#define SYS_dup3		330
#define SYS_pipe2		331
#define SYS_inotify_init1	332
#define SYS_preadv		333
#define SYS_pwritev		334
#define SYS_rt_tgsigqueueinfo	335
#define SYS_perf_event_open	336
#define SYS_recvmmsg		337
#define SYS_fanotify_init	338
#define SYS_fanotify_mark	339
#define SYS_prlimit64		340
#define SYS_name_to_handle_at	341
#define SYS_open_by_handle_at	342
#define SYS_clock_adjtime	343
#define SYS_syncfs		344
#define SYS_sendmmsg		345
#define SYS_setns		346
#define SYS_process_vm_readv	347
#define SYS_process_vm_writev	348
#define SYS_kcmp		349
#define SYS_finit_module	350
#define SYS_sched_setattr	351
#define SYS_sched_getattr	352
#define SYS_renameat2		353
#define SYS_seccomp		354
#define SYS_getrandom		355
#define SYS_memfd_create	356
#define SYS_bpf		357
#define SYS_execveat		358
#define SYS_socket		359
#define SYS_socketpair		360
#define SYS_bind		361
#define SYS_connect		362
#define SYS_listen		363
#define SYS_accept4		364
#define SYS_getsockopt		365
#define SYS_setsockopt		366
#define SYS_getsockname	367
#define SYS_getpeername	368
#define SYS_sendto		369
#define SYS_sendmsg		370
#define SYS_recvfrom		371
#define SYS_recvmsg		372
#define SYS_shutdown		373
#define SYS_userfaultfd	374
#define SYS_membarrier		375
#define SYS_mlock2		376
#define SYS_copy_file_range	377
#define SYS_preadv2		378
#define SYS_pwritev2		379
#define SYS_pkey_mprotect	380
#define SYS_pkey_alloc		381
#define SYS_pkey_free		382
#define SYS_statx		383
#define SYS_arch_prctl		384
#define SYS_io_pgetevents	385
#define SYS_rseq		386
#define SYS_semget		393
#define SYS_semctl		394
#define SYS_shmget		395
#define SYS_shmctl		396
#define SYS_shmat		397
#define SYS_shmdt		398
#define SYS_msgget		399
#define SYS_msgsnd		400
#define SYS_msgrcv		401
#define SYS_msgctl		402
#define SYS_clock_gettime64	403
#define SYS_clock_settime64	404
#define SYS_clock_adjtime64	405
#define SYS_clock_getres_time64 406
#define SYS_clock_nanosleep_time64 407
#define SYS_timer_gettime64	408
#define SYS_timer_settime64	409
#define SYS_timerfd_gettime64	410
#define SYS_timerfd_settime64	411
#define SYS_utimensat_time64	412
#define SYS_pselect6_time64	413
#define SYS_ppoll_time64	414
#define SYS_io_pgetevents_time64 416
#define SYS_recvmmsg_time64	417
#define SYS_mq_timedsend_time64 418
#define SYS_mq_timedreceive_time64 419
#define SYS_semtimedop_time64	420
#define SYS_rt_sigtimedwait_time64 421
#define SYS_futex_time64	422
#define SYS_sched_rr_get_interval_time64 423
#define SYS_pidfd_send_signal	424
#define SYS_io_uring_setup	425
#define SYS_io_uring_enter	426
#define SYS_io_uring_register	427
#define SYS_open_tree		428
#define SYS_move_mount		429
#define SYS_fsopen		430
#define SYS_fsconfig		431
#define SYS_fsmount		432
#define SYS_fspick		433
#define SYS_pidfd_open		434
#define SYS_clone3		435
#define SYS_close_range	436
#define SYS_openat2		437
#define SYS_pidfd_getfd	438
#define SYS_faccessat2		439
#define SYS_process_madvise	440
#define SYS_epoll_pwait2	441
#define SYS_mount_setattr	442
#define SYS_quotactl_fd	443
#define SYS_landlock_create_ruleset	444
#define SYS_landlock_add_rule	445
#define SYS_landlock_restrict_self	446
#define SYS_memfd_secret	447
#define SYS_process_mrelease	448
#define SYS_futex_waitv	449
#define SYS_set_mempolicy_home_node	450
#define SYS_cachestat		451
#define SYS_fchmodat2		452
#define SYS_map_shadow_stack	453
#define SYS_futex_wake		454
#define SYS_futex_wait		455
#define SYS_futex_requeue	456
#define SYS_statmount		457
#define SYS_listmount		458
#define SYS_lsm_get_self_attr	459
#define SYS_lsm_set_self_attr	460
#define SYS_lsm_list_modules	461
#define SYS_mseal		462
#define SYS_setxattrat		463
#define SYS_getxattrat		464
#define SYS_listxattrat	465
#define SYS_removexattrat	466
#define SYS_open_tree_attr	467
#define SYS_file_getattr	468
#define SYS_file_setattr	469
#define SYS_listns		470

#define __SYS_brk	SYS_brk
#define __SYS_mmap	SYS_mmap
#define __SYS_munmap	SYS_munmap
#define __SYS_alarm	SYS_alarm
#define __SYS_execve	SYS_execve
#define __SYS_exit	SYS_exit
#define __SYS_fork	SYS_fork
#define __SYS_getpid	SYS_getpid
#define __SYS_getppid	SYS_getppid
#define __SYS_nanosleep	SYS_nanosleep
#define __SYS_pause	SYS_pause
#define __SYS_waitpid	SYS_waitpid
#define __SYS_chdir	SYS_chdir
#define __SYS_fchdir	SYS_fchdir
#define __SYS_chroot	SYS_chroot
#define __SYS_close	SYS_close
#define __SYS_creat	SYS_creat
#define __SYS_dup	SYS_dup
#define __SYS_dup2	SYS_dup2
#define __SYS_getdents	SYS_getdents
#define __SYS_getdents64	SYS_getdents64
#define __SYS_ioctl	SYS_ioctl
#define __SYS_link	SYS_link
#define __SYS_lseek	SYS_lseek
#define __SYS_mkdir	SYS_mkdir
#define __SYS_mknod	SYS_mknod
#define __SYS_mount	SYS_mount
#define __SYS_open	SYS_open
#define __SYS_pipe	SYS_pipe
#define __SYS_read	SYS_read
#define __SYS_rename	SYS_rename
#define __SYS_rmdir	SYS_rmdir
#define __SYS_stat	SYS_stat
#define __SYS_fstat	SYS_fstat
#define __SYS_truncate	SYS_truncate
#define __SYS_ftruncate	SYS_ftruncate
#define __SYS_umount	SYS_umount
#define __SYS_unlink	SYS_unlink
#define __SYS_write	SYS_write
#define __SYS_time	SYS_time
#define __SYS_getitimer	SYS_getitimer
#define __SYS_setitimer	SYS_setitimer
#define __SYS_getpgid	SYS_getpgid
#define __SYS_getsid	SYS_getsid
#define __SYS_setpgid	SYS_setpgid
#define __SYS_setsid	SYS_setsid
#define __SYS_kill	SYS_kill
#define __SYS_signal	SYS_signal
#define __SYS_sigaction	SYS_sigaction
#define __SYS_sigsuspend	SYS_sigsuspend
#define __SYS_sigpending	SYS_sigpending
#define __SYS_gettimeofday_time32	SYS_gettimeofday_time32
#define __SYS_sigprocmask	SYS_sigprocmask
#define __SYS_sigreturn	SYS_sigreturn
#define __SYS_fcntl	SYS_fcntl
#define __SYS_getrlimit	SYS_getrlimit
#define __SYS_setrlimit	SYS_setrlimit

#define __syscall_return(type, __res) \
do { \
	if (__res >= 0) \
		return (type) __res; \
	errno = -__res; \
	return (type) -1; \
} while (0)

#define _syscall0(type,name) \
type name(void) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__SYS_##name) \
	: "memory"); \
__syscall_return(type, __res); \
}

#define _syscall1(type,name,type1,arg1) \
type name(type1 arg1) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__SYS_##name), "b" ((long)(arg1)) \
	: "memory"); \
__syscall_return(type, __res); \
}

#define _syscall2(type,name,type1,arg1,type2,arg2) \
type name(type1 arg1,type2 arg2) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__SYS_##name),"b" ((long)(arg1)),"c" ((long)(arg2)) \
	: "memory"); \
__syscall_return(type, __res); \
}

#define _syscall2v(type,name,type1,arg1,type2,arg2) \
type name(type1 arg1,type2 arg2,...) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__SYS_##name),"b" ((long)(arg1)),"c" ((long)(arg2)) \
	: "memory"); \
__syscall_return(type, __res); \
}

#define _syscall3(type,name,type1,arg1,type2,arg2,type3,arg3) \
type name(type1 arg1,type2 arg2,type3 arg3) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__SYS_##name),"b" ((long)(arg1)),"c" ((long)(arg2)),"d" ((long)(arg3)) \
	: "memory"); \
__syscall_return(type, __res); \
}

#define _syscall4(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4) \
type name (type1 arg1, type2 arg2, type3 arg3, type4 arg4) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__SYS_##name),"b" ((long)(arg1)),"c" ((long)(arg2)), \
	  "d" ((long)(arg3)),"S" ((long)(arg4)) \
	: "memory"); \
__syscall_return(type, __res); \
}

#define _syscall5(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,type5,arg5) \
type name (type1 arg1,type2 arg2,type3 arg3,type4 arg4,type5 arg5) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__SYS_##name),"b" ((long)(arg1)),"c" ((long)(arg2)), \
	  "d" ((long)(arg3)),"S" ((long)(arg4)),"D" ((long)(arg5)) \
	: "memory"); \
__syscall_return(type, __res); \
}

#define _syscall6(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,type5,arg5,type6,arg6) \
type name (type1 arg1,type2 arg2,type3 arg3,type4 arg4,type5 arg5,type6 arg6) \
{ \
long __res; \
__asm__ volatile ( \
	"pushl %7 ; push %%ebp ; mov 4(%%esp),%%ebp ; int $0x80 ; pop %%ebp ; add $4,%%esp" \
	: "=a" (__res) \
	: "0" (__SYS_##name),"b" ((long)(arg1)),"c" ((long)(arg2)), \
	  "d" ((long)(arg3)),"S" ((long)(arg4)),"D" ((long)(arg5)),"g" ((long)(arg5)) \
	: "memory"); \
__syscall_return(type, __res); \
}

#endif /* _SYSCALL_H */
