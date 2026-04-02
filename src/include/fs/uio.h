#ifndef _FS_UIO_H
#define _FS_UIO_H

#include <types.h>

struct iovec {
    void *iov_base;
    size_t iov_len;
};

#endif
