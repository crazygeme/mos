#ifndef MOS_ALSA_H
#define MOS_ALSA_H
#include <fs/fs.h>
void alsa_register(file *(*open)(struct super_block *, unsigned, int));
void alsa_disconnect(void);
void alsa_wake(void);
#endif
