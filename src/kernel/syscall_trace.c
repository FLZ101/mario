#include <syscall_trace.h>
#include <syscall.h>

#include <misc.h>
#include <sched.h>
#include <task.h>

#include <lib/string.h>

struct syscall_trace_stat syscall_trace[NR_SYSCALLS];
static pid_t trace_pid;

/*
 * Wall-clock profile state.  tsc_per_ms calibrates raw rdtsc cycles, while
 * utime/stime are the task tick counters already maintained by the PIT.
 */
static uint64_t trace_reset_tsc;
static uint64_t tsc_per_ms;
static long trace_start_utime;
static long trace_start_stime;
static long trace_end_utime;
static long trace_end_stime;
static int trace_task_exited;

const char *syscall_trace_name(unsigned int nr)
{
	static const char *const names[NR_SYSCALLS] = {
		[SYS_restart_syscall] = "restart_syscall",
		[SYS_exit] = "exit",
		[SYS_fork] = "fork",
		[SYS_read] = "read",
		[SYS_write] = "write",
		[SYS_open] = "open",
		[SYS_close] = "close",
		[SYS_waitpid] = "waitpid",
		[SYS_creat] = "creat",
		[SYS_link] = "link",
		[SYS_unlink] = "unlink",
		[SYS_execve] = "execve",
		[SYS_chdir] = "chdir",
		[SYS_time] = "time",
		[SYS_mknod] = "mknod",
		[SYS_chmod] = "chmod",
		[SYS_lchown] = "lchown",
		[SYS_break] = "break",
		[SYS_oldstat] = "oldstat",
		[SYS_lseek] = "lseek",
		[SYS_getpid] = "getpid",
		[SYS_mount] = "mount",
		[SYS_umount] = "umount",
		[SYS_setuid] = "setuid",
		[SYS_getuid] = "getuid",
		[SYS_stime] = "stime",
		[SYS_ptrace] = "ptrace",
		[SYS_alarm] = "alarm",
		[SYS_oldfstat] = "oldfstat",
		[SYS_pause] = "pause",
		[SYS_utime] = "utime",
		[SYS_stty] = "stty",
		[SYS_gtty] = "gtty",
		[SYS_access] = "access",
		[SYS_nice] = "nice",
		[SYS_ftime] = "ftime",
		[SYS_sync] = "sync",
		[SYS_kill] = "kill",
		[SYS_rename] = "rename",
		[SYS_mkdir] = "mkdir",
		[SYS_rmdir] = "rmdir",
		[SYS_dup] = "dup",
		[SYS_pipe] = "pipe",
		[SYS_times] = "times",
		[SYS_prof] = "prof",
		[SYS_brk] = "brk",
		[SYS_setgid] = "setgid",
		[SYS_getgid] = "getgid",
		[SYS_signal] = "signal",
		[SYS_geteuid] = "geteuid",
		[SYS_getegid] = "getegid",
		[SYS_acct] = "acct",
		[SYS_umount2] = "umount2",
		[SYS_lock] = "lock",
		[SYS_ioctl] = "ioctl",
		[SYS_fcntl] = "fcntl",
		[SYS_mpx] = "mpx",
		[SYS_setpgid] = "setpgid",
		[SYS_ulimit] = "ulimit",
		[SYS_oldolduname] = "oldolduname",
		[SYS_umask] = "umask",
		[SYS_chroot] = "chroot",
		[SYS_ustat] = "ustat",
		[SYS_dup2] = "dup2",
		[SYS_getppid] = "getppid",
		[SYS_getpgrp] = "getpgrp",
		[SYS_setsid] = "setsid",
		[SYS_sigaction] = "sigaction",
		[SYS_sgetmask] = "sgetmask",
		[SYS_ssetmask] = "ssetmask",
		[SYS_setreuid] = "setreuid",
		[SYS_setregid] = "setregid",
		[SYS_sigsuspend] = "sigsuspend",
		[SYS_sigpending] = "sigpending",
		[SYS_sethostname] = "sethostname",
		[SYS_setrlimit] = "setrlimit",
		[SYS_getrlimit] = "getrlimit",
		[SYS_getrusage] = "getrusage",
		[SYS_gettimeofday_time32] = "gettimeofday_time32",
		[SYS_settimeofday_time32] = "settimeofday_time32",
		[SYS_getgroups] = "getgroups",
		[SYS_setgroups] = "setgroups",
		[SYS_select] = "select",
		[SYS_symlink] = "symlink",
		[SYS_oldlstat] = "oldlstat",
		[SYS_readlink] = "readlink",
		[SYS_uselib] = "uselib",
		[SYS_swapon] = "swapon",
		[SYS_reboot] = "reboot",
		[SYS_readdir] = "readdir",
		[SYS_mmap] = "mmap",
		[SYS_munmap] = "munmap",
		[SYS_truncate] = "truncate",
		[SYS_ftruncate] = "ftruncate",
		[SYS_fchmod] = "fchmod",
		[SYS_fchown] = "fchown",
		[SYS_getpriority] = "getpriority",
		[SYS_setpriority] = "setpriority",
		[SYS_profil] = "profil",
		[SYS_statfs] = "statfs",
		[SYS_fstatfs] = "fstatfs",
		[SYS_ioperm] = "ioperm",
		[SYS_socketcall] = "socketcall",
		[SYS_syslog] = "syslog",
		[SYS_setitimer] = "setitimer",
		[SYS_getitimer] = "getitimer",
		[SYS_stat] = "stat",
		[SYS_lstat] = "lstat",
		[SYS_fstat] = "fstat",
		[SYS_olduname] = "olduname",
		[SYS_iopl] = "iopl",
		[SYS_vhangup] = "vhangup",
		[SYS_idle] = "idle",
		[SYS_vm86old] = "vm86old",
		[SYS_wait4] = "wait4",
		[SYS_swapoff] = "swapoff",
		[SYS_sysinfo] = "sysinfo",
		[SYS_ipc] = "ipc",
		[SYS_fsync] = "fsync",
		[SYS_sigreturn] = "sigreturn",
		[SYS_clone] = "clone",
		[SYS_setdomainname] = "setdomainname",
		[SYS_uname] = "uname",
		[SYS_modify_ldt] = "modify_ldt",
		[SYS_adjtimex] = "adjtimex",
		[SYS_mprotect] = "mprotect",
		[SYS_sigprocmask] = "sigprocmask",
		[SYS_create_module] = "create_module",
		[SYS_init_module] = "init_module",
		[SYS_delete_module] = "delete_module",
		[SYS_get_kernel_syms] = "get_kernel_syms",
		[SYS_quotactl] = "quotactl",
		[SYS_getpgid] = "getpgid",
		[SYS_fchdir] = "fchdir",
		[SYS_bdflush] = "bdflush",
		[SYS_sysfs] = "sysfs",
		[SYS_personality] = "personality",
		[SYS_afs_syscall] = "afs_syscall",
		[SYS_setfsuid] = "setfsuid",
		[SYS_setfsgid] = "setfsgid",
		[SYS__llseek] = "_llseek",
		[SYS_getdents] = "getdents",
		[SYS__newselect] = "_newselect",
		[SYS_flock] = "flock",
		[SYS_msync] = "msync",
		[SYS_readv] = "readv",
		[SYS_writev] = "writev",
		[SYS_getsid] = "getsid",
		[SYS_fdatasync] = "fdatasync",
		[SYS__sysctl] = "_sysctl",
		[SYS_mlock] = "mlock",
		[SYS_munlock] = "munlock",
		[SYS_mlockall] = "mlockall",
		[SYS_munlockall] = "munlockall",
		[SYS_sched_setparam] = "sched_setparam",
		[SYS_sched_getparam] = "sched_getparam",
		[SYS_sched_setscheduler] = "sched_setscheduler",
		[SYS_sched_getscheduler] = "sched_getscheduler",
		[SYS_sched_yield] = "sched_yield",
		[SYS_sched_get_priority_max] = "sched_get_priority_max",
		[SYS_sched_get_priority_min] = "sched_get_priority_min",
		[SYS_sched_rr_get_interval] = "sched_rr_get_interval",
		[SYS_nanosleep] = "nanosleep",
		[SYS_mremap] = "mremap",
		[SYS_setresuid] = "setresuid",
		[SYS_getresuid] = "getresuid",
		[SYS_vm86] = "vm86",
		[SYS_query_module] = "query_module",
		[SYS_poll] = "poll",
		[SYS_nfsservctl] = "nfsservctl",
		[SYS_setresgid] = "setresgid",
		[SYS_getresgid] = "getresgid",
		[SYS_prctl] = "prctl",
		[SYS_rt_sigreturn] = "rt_sigreturn",
		[SYS_rt_sigaction] = "rt_sigaction",
		[SYS_rt_sigprocmask] = "rt_sigprocmask",
		[SYS_rt_sigpending] = "rt_sigpending",
		[SYS_rt_sigtimedwait] = "rt_sigtimedwait",
		[SYS_rt_sigqueueinfo] = "rt_sigqueueinfo",
		[SYS_rt_sigsuspend] = "rt_sigsuspend",
		[SYS_pread64] = "pread64",
		[SYS_pwrite64] = "pwrite64",
		[SYS_chown] = "chown",
		[SYS_getcwd] = "getcwd",
		[SYS_capget] = "capget",
		[SYS_capset] = "capset",
		[SYS_sigaltstack] = "sigaltstack",
		[SYS_sendfile] = "sendfile",
		[SYS_getpmsg] = "getpmsg",
		[SYS_putpmsg] = "putpmsg",
		[SYS_vfork] = "vfork",
		[SYS_ugetrlimit] = "ugetrlimit",
		[SYS_mmap2] = "mmap2",
		[SYS_truncate64] = "truncate64",
		[SYS_ftruncate64] = "ftruncate64",
		[SYS_stat64] = "stat64",
		[SYS_lstat64] = "lstat64",
		[SYS_fstat64] = "fstat64",
		[SYS_lchown32] = "lchown32",
		[SYS_getuid32] = "getuid32",
		[SYS_getgid32] = "getgid32",
		[SYS_geteuid32] = "geteuid32",
		[SYS_getegid32] = "getegid32",
		[SYS_setreuid32] = "setreuid32",
		[SYS_setregid32] = "setregid32",
		[SYS_getgroups32] = "getgroups32",
		[SYS_setgroups32] = "setgroups32",
		[SYS_fchown32] = "fchown32",
		[SYS_setresuid32] = "setresuid32",
		[SYS_getresuid32] = "getresuid32",
		[SYS_setresgid32] = "setresgid32",
		[SYS_getresgid32] = "getresgid32",
		[SYS_chown32] = "chown32",
		[SYS_setuid32] = "setuid32",
		[SYS_setgid32] = "setgid32",
		[SYS_setfsuid32] = "setfsuid32",
		[SYS_setfsgid32] = "setfsgid32",
		[SYS_pivot_root] = "pivot_root",
		[SYS_mincore] = "mincore",
		[SYS_madvise] = "madvise",
		[SYS_getdents64] = "getdents64",
		[SYS_fcntl64] = "fcntl64",
		[SYS_gettid] = "gettid",
		[SYS_readahead] = "readahead",
		[SYS_setxattr] = "setxattr",
		[SYS_lsetxattr] = "lsetxattr",
		[SYS_fsetxattr] = "fsetxattr",
		[SYS_getxattr] = "getxattr",
		[SYS_lgetxattr] = "lgetxattr",
		[SYS_fgetxattr] = "fgetxattr",
		[SYS_listxattr] = "listxattr",
		[SYS_llistxattr] = "llistxattr",
		[SYS_flistxattr] = "flistxattr",
		[SYS_removexattr] = "removexattr",
		[SYS_lremovexattr] = "lremovexattr",
		[SYS_fremovexattr] = "fremovexattr",
		[SYS_tkill] = "tkill",
		[SYS_sendfile64] = "sendfile64",
		[SYS_futex] = "futex",
		[SYS_sched_setaffinity] = "sched_setaffinity",
		[SYS_sched_getaffinity] = "sched_getaffinity",
		[SYS_set_thread_area] = "set_thread_area",
		[SYS_get_thread_area] = "get_thread_area",
		[SYS_io_setup] = "io_setup",
		[SYS_io_destroy] = "io_destroy",
		[SYS_io_getevents] = "io_getevents",
		[SYS_io_submit] = "io_submit",
		[SYS_io_cancel] = "io_cancel",
		[SYS_fadvise64] = "fadvise64",
		[SYS_exit_group] = "exit_group",
		[SYS_lookup_dcookie] = "lookup_dcookie",
		[SYS_epoll_create] = "epoll_create",
		[SYS_epoll_ctl] = "epoll_ctl",
		[SYS_epoll_wait] = "epoll_wait",
		[SYS_remap_file_pages] = "remap_file_pages",
		[SYS_set_tid_address] = "set_tid_address",
		[SYS_timer_create] = "timer_create",
		[SYS_timer_settime32] = "timer_settime32",
		[SYS_timer_gettime32] = "timer_gettime32",
		[SYS_timer_getoverrun] = "timer_getoverrun",
		[SYS_timer_delete] = "timer_delete",
		[SYS_clock_settime32] = "clock_settime32",
		[SYS_clock_gettime32] = "clock_gettime32",
		[SYS_clock_getres_time32] = "clock_getres_time32",
		[SYS_clock_nanosleep_time32] = "clock_nanosleep_time32",
		[SYS_statfs64] = "statfs64",
		[SYS_fstatfs64] = "fstatfs64",
		[SYS_tgkill] = "tgkill",
		[SYS_utimes] = "utimes",
		[SYS_fadvise64_64] = "fadvise64_64",
		[SYS_vserver] = "vserver",
		[SYS_mbind] = "mbind",
		[SYS_get_mempolicy] = "get_mempolicy",
		[SYS_set_mempolicy] = "set_mempolicy",
		[SYS_mq_open] = "mq_open",
		[SYS_mq_unlink] = "mq_unlink",
		[SYS_mq_timedsend] = "mq_timedsend",
		[SYS_mq_timedreceive] = "mq_timedreceive",
		[SYS_mq_notify] = "mq_notify",
		[SYS_mq_getsetattr] = "mq_getsetattr",
		[SYS_kexec_load] = "kexec_load",
		[SYS_waitid] = "waitid",
		[SYS_sys_setaltroot] = "sys_setaltroot",
		[SYS_add_key] = "add_key",
		[SYS_request_key] = "request_key",
		[SYS_keyctl] = "keyctl",
		[SYS_ioprio_set] = "ioprio_set",
		[SYS_ioprio_get] = "ioprio_get",
		[SYS_inotify_init] = "inotify_init",
		[SYS_inotify_add_watch] = "inotify_add_watch",
		[SYS_inotify_rm_watch] = "inotify_rm_watch",
		[SYS_migrate_pages] = "migrate_pages",
		[SYS_openat] = "openat",
		[SYS_mkdirat] = "mkdirat",
		[SYS_mknodat] = "mknodat",
		[SYS_fchownat] = "fchownat",
		[SYS_futimesat] = "futimesat",
		[SYS_fstatat64] = "fstatat64",
		[SYS_unlinkat] = "unlinkat",
		[SYS_renameat] = "renameat",
		[SYS_linkat] = "linkat",
		[SYS_symlinkat] = "symlinkat",
		[SYS_readlinkat] = "readlinkat",
		[SYS_fchmodat] = "fchmodat",
		[SYS_faccessat] = "faccessat",
		[SYS_pselect6] = "pselect6",
		[SYS_ppoll] = "ppoll",
		[SYS_unshare] = "unshare",
		[SYS_set_robust_list] = "set_robust_list",
		[SYS_get_robust_list] = "get_robust_list",
		[SYS_splice] = "splice",
		[SYS_sync_file_range] = "sync_file_range",
		[SYS_tee] = "tee",
		[SYS_vmsplice] = "vmsplice",
		[SYS_move_pages] = "move_pages",
		[SYS_getcpu] = "getcpu",
		[SYS_epoll_pwait] = "epoll_pwait",
		[SYS_utimensat] = "utimensat",
		[SYS_signalfd] = "signalfd",
		[SYS_timerfd_create] = "timerfd_create",
		[SYS_eventfd] = "eventfd",
		[SYS_fallocate] = "fallocate",
		[SYS_timerfd_settime32] = "timerfd_settime32",
		[SYS_timerfd_gettime32] = "timerfd_gettime32",
		[SYS_signalfd4] = "signalfd4",
		[SYS_eventfd2] = "eventfd2",
		[SYS_epoll_create1] = "epoll_create1",
		[SYS_dup3] = "dup3",
		[SYS_pipe2] = "pipe2",
		[SYS_inotify_init1] = "inotify_init1",
		[SYS_preadv] = "preadv",
		[SYS_pwritev] = "pwritev",
		[SYS_rt_tgsigqueueinfo] = "rt_tgsigqueueinfo",
		[SYS_perf_event_open] = "perf_event_open",
		[SYS_recvmmsg] = "recvmmsg",
		[SYS_fanotify_init] = "fanotify_init",
		[SYS_fanotify_mark] = "fanotify_mark",
		[SYS_prlimit64] = "prlimit64",
		[SYS_name_to_handle_at] = "name_to_handle_at",
		[SYS_open_by_handle_at] = "open_by_handle_at",
		[SYS_clock_adjtime] = "clock_adjtime",
		[SYS_syncfs] = "syncfs",
		[SYS_sendmmsg] = "sendmmsg",
		[SYS_setns] = "setns",
		[SYS_process_vm_readv] = "process_vm_readv",
		[SYS_process_vm_writev] = "process_vm_writev",
		[SYS_kcmp] = "kcmp",
		[SYS_finit_module] = "finit_module",
		[SYS_sched_setattr] = "sched_setattr",
		[SYS_sched_getattr] = "sched_getattr",
		[SYS_renameat2] = "renameat2",
		[SYS_seccomp] = "seccomp",
		[SYS_getrandom] = "getrandom",
		[SYS_memfd_create] = "memfd_create",
		[SYS_bpf] = "bpf",
		[SYS_execveat] = "execveat",
		[SYS_socket] = "socket",
		[SYS_socketpair] = "socketpair",
		[SYS_bind] = "bind",
		[SYS_connect] = "connect",
		[SYS_listen] = "listen",
		[SYS_accept4] = "accept4",
		[SYS_getsockopt] = "getsockopt",
		[SYS_setsockopt] = "setsockopt",
		[SYS_getsockname] = "getsockname",
		[SYS_getpeername] = "getpeername",
		[SYS_sendto] = "sendto",
		[SYS_sendmsg] = "sendmsg",
		[SYS_recvfrom] = "recvfrom",
		[SYS_recvmsg] = "recvmsg",
		[SYS_shutdown] = "shutdown",
		[SYS_userfaultfd] = "userfaultfd",
		[SYS_membarrier] = "membarrier",
		[SYS_mlock2] = "mlock2",
		[SYS_copy_file_range] = "copy_file_range",
		[SYS_preadv2] = "preadv2",
		[SYS_pwritev2] = "pwritev2",
		[SYS_pkey_mprotect] = "pkey_mprotect",
		[SYS_pkey_alloc] = "pkey_alloc",
		[SYS_pkey_free] = "pkey_free",
		[SYS_statx] = "statx",
		[SYS_arch_prctl] = "arch_prctl",
		[SYS_io_pgetevents] = "io_pgetevents",
		[SYS_rseq] = "rseq",
		[SYS_semget] = "semget",
		[SYS_semctl] = "semctl",
		[SYS_shmget] = "shmget",
		[SYS_shmctl] = "shmctl",
		[SYS_shmat] = "shmat",
		[SYS_shmdt] = "shmdt",
		[SYS_msgget] = "msgget",
		[SYS_msgsnd] = "msgsnd",
		[SYS_msgrcv] = "msgrcv",
		[SYS_msgctl] = "msgctl",
		[SYS_clock_gettime64] = "clock_gettime64",
		[SYS_clock_settime64] = "clock_settime64",
		[SYS_clock_adjtime64] = "clock_adjtime64",
		[SYS_clock_getres_time64] = "clock_getres_time64",
		[SYS_clock_nanosleep_time64] = "clock_nanosleep_time64",
		[SYS_timer_gettime64] = "timer_gettime64",
		[SYS_timer_settime64] = "timer_settime64",
		[SYS_timerfd_gettime64] = "timerfd_gettime64",
		[SYS_timerfd_settime64] = "timerfd_settime64",
		[SYS_utimensat_time64] = "utimensat_time64",
		[SYS_pselect6_time64] = "pselect6_time64",
		[SYS_ppoll_time64] = "ppoll_time64",
		[SYS_io_pgetevents_time64] = "io_pgetevents_time64",
		[SYS_recvmmsg_time64] = "recvmmsg_time64",
		[SYS_mq_timedsend_time64] = "mq_timedsend_time64",
		[SYS_mq_timedreceive_time64] = "mq_timedreceive_time64",
		[SYS_semtimedop_time64] = "semtimedop_time64",
		[SYS_rt_sigtimedwait_time64] = "rt_sigtimedwait_time64",
		[SYS_futex_time64] = "futex_time64",
		[SYS_sched_rr_get_interval_time64] = "sched_rr_get_interval_time64",
		[SYS_pidfd_send_signal] = "pidfd_send_signal",
		[SYS_io_uring_setup] = "io_uring_setup",
		[SYS_io_uring_enter] = "io_uring_enter",
		[SYS_io_uring_register] = "io_uring_register",
		[SYS_open_tree] = "open_tree",
		[SYS_move_mount] = "move_mount",
		[SYS_fsopen] = "fsopen",
		[SYS_fsconfig] = "fsconfig",
		[SYS_fsmount] = "fsmount",
		[SYS_fspick] = "fspick",
		[SYS_pidfd_open] = "pidfd_open",
		[SYS_clone3] = "clone3",
		[SYS_close_range] = "close_range",
		[SYS_openat2] = "openat2",
		[SYS_pidfd_getfd] = "pidfd_getfd",
		[SYS_faccessat2] = "faccessat2",
		[SYS_process_madvise] = "process_madvise",
		[SYS_epoll_pwait2] = "epoll_pwait2",
		[SYS_mount_setattr] = "mount_setattr",
		[SYS_quotactl_fd] = "quotactl_fd",
		[SYS_landlock_create_ruleset] = "landlock_create_ruleset",
		[SYS_landlock_add_rule] = "landlock_add_rule",
		[SYS_landlock_restrict_self] = "landlock_restrict_self",
		[SYS_memfd_secret] = "memfd_secret",
		[SYS_process_mrelease] = "process_mrelease",
		[SYS_futex_waitv] = "futex_waitv",
		[SYS_set_mempolicy_home_node] = "set_mempolicy_home_node",
		[SYS_cachestat] = "cachestat",
		[SYS_fchmodat2] = "fchmodat2",
		[SYS_map_shadow_stack] = "map_shadow_stack",
		[SYS_futex_wake] = "futex_wake",
		[SYS_futex_wait] = "futex_wait",
		[SYS_futex_requeue] = "futex_requeue",
		[SYS_statmount] = "statmount",
		[SYS_listmount] = "listmount",
		[SYS_lsm_get_self_attr] = "lsm_get_self_attr",
		[SYS_lsm_set_self_attr] = "lsm_set_self_attr",
		[SYS_lsm_list_modules] = "lsm_list_modules",
		[SYS_mseal] = "mseal",
		[SYS_setxattrat] = "setxattrat",
		[SYS_getxattrat] = "getxattrat",
		[SYS_listxattrat] = "listxattrat",
		[SYS_removexattrat] = "removexattrat",
		[SYS_open_tree_attr] = "open_tree_attr",
		[SYS_file_getattr] = "file_getattr",
		[SYS_file_setattr] = "file_setattr",
		[SYS_listns] = "listns",
	};

	return nr < NR_SYSCALLS ? names[nr] : NULL;
}

static inline uint64_t syscall_trace_tsc(void)
{
	uint32_t lo, hi;

	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

void syscall_trace_start(unsigned int nr)
{
	uint64_t now;

	current->syscall_trace_active = 0;
	if (nr >= NR_SYSCALLS)
		return;
	if (trace_pid && current->pid != trace_pid)
		return;

	now = syscall_trace_tsc();
	current->syscall_trace_start = now;
	current->syscall_trace_running = 0;
	current->syscall_trace_last_tsc = now;
	current->syscall_trace_active = 1;
}

void syscall_trace_end(unsigned int nr)
{
	uint64_t now, delta;
	struct syscall_trace_stat *stat;

	if (nr >= NR_SYSCALLS)
		return;
	if (!current->syscall_trace_active)
		return;

	now = syscall_trace_tsc();
	delta = current->syscall_trace_running;
	delta += now - current->syscall_trace_last_tsc;
	current->syscall_trace_active = 0;
	current->syscall_trace_start = 0;
	current->syscall_trace_running = 0;
	current->syscall_trace_last_tsc = 0;
	stat = &syscall_trace[nr];

	stat->calls++;
	stat->cycles += delta;
	if (delta > stat->max_cycles)
		stat->max_cycles = delta;
}

/*
 * Calibrate rdtsc against the PIT so cycle counters can be displayed as ms.
 * This runs once at boot, after interrupts are enabled.
 */
void syscall_trace_calibrate(void)
{
	uint64_t start, end;
	unsigned long start_jiffies;
	unsigned long elapsed_ms;

	start = syscall_trace_tsc();
	start_jiffies = jiffies;
	while ((unsigned long)jiffies - start_jiffies < 10)
		;
	end = syscall_trace_tsc();
	elapsed_ms = ((unsigned long)jiffies - start_jiffies) * 1000 / HZ;
	if (elapsed_ms)
		tsc_per_ms = (end - start) / elapsed_ms;
}

/* Reset both the per-syscall counters and the wall-clock interval start. */
void syscall_trace_reset(void)
{
	memset(syscall_trace, 0, sizeof(syscall_trace));
	trace_reset_tsc = syscall_trace_tsc();
}

static struct task_struct *syscall_trace_find_task(pid_t pid)
{
	struct task_struct *task = &init_task;

	do {
		if (task->pid == pid)
			return task;
		task = task->next_task;
	} while (task != &init_task);

	return NULL;
}

/*
 * Start profiling when trace_pid is set.  utime/stime survive exec, so the
 * same task can be tracked across the shell-to-python transition.
 */
void syscall_trace_set_pid(pid_t pid)
{
	struct task_struct *task;

	trace_pid = pid;
	trace_task_exited = 0;
	if (!pid)
		return;

	task = syscall_trace_find_task(pid);
	if (task) {
		trace_start_utime = task->utime;
		trace_start_stime = task->stime;
	}
	trace_reset_tsc = syscall_trace_tsc();
}

/*
 * The traced task may exit before debugfs is read.  Save its final CPU
 * counters before it becomes a zombie and is later freed.
 */
void syscall_trace_task_exit(struct task_struct *task)
{
	if (!trace_pid || !task || task->pid != trace_pid)
		return;

	trace_end_utime = task->utime;
	trace_end_stime = task->stime;
	trace_task_exited = 1;
}

pid_t syscall_trace_get_pid(void)
{
	return trace_pid;
}

/* Wall-clock time since trace_pid was set, using calibrated rdtsc. */
unsigned long syscall_trace_elapsed_ms(void)
{
	uint64_t now;

	if (!tsc_per_ms)
		return 0;
	now = syscall_trace_tsc();
	if (now < trace_reset_tsc)
		return 0;
	return (unsigned long)((now - trace_reset_tsc) / tsc_per_ms);
}

uint64_t syscall_trace_cycles_per_ms(void)
{
	return tsc_per_ms;
}

/*
 * PIT-sampled user/kernel CPU time.  Prefer the live task and fall back to
 * the values captured at exit.
 */
unsigned long syscall_trace_user_ms(void)
{
	struct task_struct *task;
	long utime;

	if (!trace_pid)
		return 0;
	task = syscall_trace_find_task(trace_pid);
	if (task) {
		utime = task->utime;
	} else if (trace_task_exited) {
		utime = trace_end_utime;
	} else {
		return 0;
	}

	if (utime < trace_start_utime)
		return 0;
	return (unsigned long)(utime - trace_start_utime) * 1000 / HZ;
}

unsigned long syscall_trace_kernel_ms(void)
{
	struct task_struct *task;
	long stime;

	if (!trace_pid)
		return 0;
	task = syscall_trace_find_task(trace_pid);
	if (task) {
		stime = task->stime;
	} else if (trace_task_exited) {
		stime = trace_end_stime;
	} else {
		return 0;
	}

	if (stime < trace_start_stime)
		return 0;
	return (unsigned long)(stime - trace_start_stime) * 1000 / HZ;
}

/* Sum all traced syscall cycles and convert the total to milliseconds. */
unsigned long syscall_trace_total_ms(void)
{
	uint64_t total = 0;
	unsigned int nr;

	if (!tsc_per_ms)
		return 0;
	for (nr = 0; nr < NR_SYSCALLS; nr++)
		total += syscall_trace[nr].cycles;
	return (unsigned long)(total / tsc_per_ms);
}

void syscall_trace_context_switch_out(void)
{
	uint64_t now;

	if (!current->syscall_trace_active)
		return;

	now = syscall_trace_tsc();
	current->syscall_trace_running += now - current->syscall_trace_last_tsc;
	current->syscall_trace_last_tsc = now;
}

void syscall_trace_context_switch_in(void)
{
	if (!current->syscall_trace_active)
		return;

	current->syscall_trace_last_tsc = syscall_trace_tsc();
}
