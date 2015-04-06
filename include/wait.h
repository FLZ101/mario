#ifndef _WAIT_H
#define _WAIT_H

#include <task.h>

#include <lib/list.h>
#include <lib/stddef.h>
#include <lib/spinlock.h>

typedef struct {
	struct task_struct *p;
	struct list_head task_list;
} wait_queue_node_t;

typedef struct {
	spinlock_t lock;
	struct list_head task_list;
} wait_queue_t;

static inline void init_wait_queue_node(wait_queue_node_t *n, struct task_struct *p)
{
	n->p = p;
	INIT_LIST_HEAD(&n->task_list);
}

static inline void init_wait_queue(wait_queue_t *q)
{
	INIT_LOCK(&q->lock);
	INIT_LIST_HEAD(&q->task_list);
}

static inline void in_wait_queue(wait_queue_t *q, wait_queue_node_t *n)
{
	list_add_tail(&n->task_list, &q->task_list);
}

static inline void out_wait_queue(wait_queue_node_t *n)
{
	list_del(&n->task_list);
}

#endif /* _WAIT_H */