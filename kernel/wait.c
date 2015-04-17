#include <wait.h>
#include <misc.h>

void in_wait_queue(wait_queue_t *q, wait_queue_node_t *n)
{
	irq_save();
	list_add_tail(&n->task_list, &q->task_list);
	irq_restore();
}

void out_wait_queue(wait_queue_t *q, wait_queue_node_t *n)
{
	irq_save();
	if (!list_empty(&n->task_list))
		list_del_init(&n->task_list);
	irq_restore();
}
