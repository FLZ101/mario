#ifndef _PIPE_H
#define _PIPE_H

#include <lib/ring_buffer.h>
#include <lib/spinlock.h>
#include <wait.h>

struct pipe_inode_info {
    struct ring_buffer rb;
    wait_queue_t wait_read;
    wait_queue_t wait_write;
    unsigned n_reader; // number of processes that may read
    unsigned n_writer;
    spinlock_t lock;
};

int init_pipe_inode_info(struct pipe_inode_info *info);
void destroy_pipe_inode_info(struct pipe_inode_info *info);

#endif /* _PIPE_H */
