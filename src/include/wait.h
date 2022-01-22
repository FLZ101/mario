#ifndef _WAIT_H
#define _WAIT_H

#include <lib/list.h>
#include <lib/stddef.h>

#define WNOHANG		0x00000001
#define WUNTRACED	0x00000002

struct task_struct;
typedef struct wait_queue_node {
	struct task_struct *p;
	struct list_head task_list;
} wait_queue_node_t;

typedef struct wait_queue {
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