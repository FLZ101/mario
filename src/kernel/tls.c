#include <ldt.h>
#include <task.h>

#include <mm/uaccess.h>

int sys_set_thread_area(struct user_desc *u_info)
{
	struct user_desc info;
	int err = verify_area(VERIFY_READ, (void *) u_info, sizeof (struct user_desc));
	if (err)
		return err;
	memcpy_fromfs(&info, u_info, sizeof (struct user_desc));

	struct user_desc *ud = current->thread.user_descs;

	int idx = info.entry_number;
	int empty = user_desc_empty(&info);

	if (!empty && idx == -1) {
		// Find a free slot
		for (idx = 0; idx < NR_USER_DESC; ++idx)
			if (user_desc_zero(ud + idx))
				break;
		if (idx == NR_USER_DESC)
			return -ESRCH;
		idx += GDT_ENTRY_TLS_MIN_IDX;

		err = verify_area(VERIFY_WRITE, (void *) u_info, sizeof (struct user_desc));
		if (err)
			return err;
		put_fs_long(idx, &u_info->entry_number);
	}

	if (idx < GDT_ENTRY_TLS_MIN_IDX || idx >= GDT_ENTRY_TLS_MAX_IDX)
		return -EINVAL;

	irq_save();

	struct desc_struct *tls_desc = gdt + GDT_ENTRY_TLS_MIN_IDX;
	if (empty) {
		zero_user_desc(&ud[idx - GDT_ENTRY_TLS_MIN_IDX]);
		zero_desc(tls_desc + idx);
	} else {
		ud[idx - GDT_ENTRY_TLS_MIN_IDX] = info;
		fill_desc(tls_desc + idx, &info);
	}

	irq_restore();
	return 0;
}

