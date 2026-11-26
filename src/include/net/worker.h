#ifndef MARIO_NET_WORKER_H
#define MARIO_NET_WORKER_H

struct net_work {
	struct net_work *next;
	void (*run)(struct net_work *);
};

void net_start(void);
void net_tick(void);
void net_submit(struct net_work *work);
int net_call(int (*run)(void *), void *data);
int net_call_interruptible(int (*run)(void *), void *data);

#endif
