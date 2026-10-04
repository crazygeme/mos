#ifndef MOS_IOVEC_H
#define MOS_IOVEC_H

#include <stddef.h>

struct iovec {
	void *iov_base;
	size_t iov_len;
};

#endif
