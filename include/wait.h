#ifndef _WAIT_H
#define _WAIT_H

#include <task.h>

#include <lib/list.h>
#include <lib/stddef.h>

typedef struct {
	struct task_struct *p;
	struct list_head task_list;
} wait_queue_node_t;

typedef struct {
	struct list_head task_list;
} wait_queue_t;

static inline void init_wait_queue_node(wait_queue_node_t *n, struct task_struct *p)
{
	n->p = p;
	INIT_LIST_HEAD(&n->task_list);
}

static inline void init_wait_queue(wait_queue_t *q)
{
	INIT_LIST_HEAD(&q->task_list);
}

void in_wait_queue(wait_queue_t *q, wait_queue_node_t *n);
void out_wait_queue(wait_queue_t *q, wait_queue_node_t *n);

#endif /* _WAIT_H */