#ifndef MOS_FS_ANONFD_H
#define MOS_FS_ANONFD_H
#include <fs/fs.h>
file *anonfd_get(int fd);
int anonfd_install(void *state, const file_operations *ops, int flags);
int anonfd_read_wait(file *fp, int (*check)(void *), void *context);
int anonfd_getattr(file *fp, struct stat *st);
loff_t anonfd_llseek(file *fp, loff_t offset, int whence);
void timerfd_poll_timers(void);
void signalfd_poll_signals(void);
#endif
