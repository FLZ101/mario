#include <sched.h>
#include <misc.h>

#include <fs/fs.h>

static struct fs_struct init_fs = {
	0,
	NULL, NULL
};

static struct files_struct init_files = {
	0,
	{{0, }},
	{NULL, }
};

static struct mm_struct init_mm = {
	mmap:		NULL,
	pd:		swapper_pg_dir,
};

#define INIT_RLIMITS \
{ \
	{RLIM_INFINITY, RLIM_INFINITY},	\
	{RLIM_INFINITY, RLIM_INFINITY}	\
}

union task_union init_task_union __attribute__((__section__(".init_task_union"))) = 
{{
	state: 0, 
	wait_chldexit: { LIST_HEAD_INIT(init_task.wait_chldexit.task_list) },
	counter: DEF_COUNTER, 
	pid: 0, pgrp: 0, session: 0, leader: 0, did_exec: 0,
	rlim: INIT_RLIMITS,
	run_list: {NULL, NULL},
	next_task: &init_task,
	prev_task: &init_task,
	p_pptr: &init_task,
	real_timer: {fun: it_real_fun},
	fs: &init_fs,
	files: &init_files,
	mm: &init_mm,
	thread: { (unsigned long)&init_task + KSTACK_SIZE, 0, }
}};
