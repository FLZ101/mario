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

static struct mm_struct init_mm;
static struct vm_area_struct init_mmap = {
	&init_mm,
	0, 0,
	NULL,
	VM_READ | VM_WRITE | VM_EXEC,
	NULL,
	NULL
};

static struct mm_struct init_mm = {
	mmap:		&init_mmap,
	mm_count:	1,
	pd:		swapper_pg_dir,
	mmlist:		LIST_HEAD_INIT(init_mm.mmlist)
};

union task_union init_task_union __attribute__((__section__(".init_task_union"))) = 
{{
	state: 0, 
	counter: DEF_COUNTER, 
	pid: 0,
	run_list: LIST_HEAD_INIT(init_task.run_list),
	next_task: &init_task,
	prev_task: &init_task,
	p_pptr: &init_task,
	real_timer: {fun: it_real_fun},
	fs: &init_fs,
	files: &init_files,
	mm: &init_mm,
	thread: {(unsigned long)&init_task + KSTACK_SIZE, 0, }
}};