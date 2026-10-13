#ifndef MOS_FS_TIMERFD_H
#define MOS_FS_TIMERFD_H
#include <arch/types.h>
/* Four signed 64-bit fields match AMD64 and i386 time64 itimerspec. */
struct timerfd_spec {
	int64_t interval_sec, interval_nsec, value_sec, value_nsec;
};
#endif
