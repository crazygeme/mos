#ifndef _FS_SYSLOG_H
#define _FS_SYSLOG_H

#include <fs/fs.h>

/* Append one record without waiting for readers. Priority includes facility. */
void syslog_emit(unsigned priority, const char *text, unsigned length);

/* Shared file operations; character devices use independent record cursors. */
file *syslog_open(unsigned mode, unsigned rdev);

/* READ consumes the proc stream. READ_ALL snapshots retained records.
 * READ_CLEAR and CLEAR advance the snapshot marker without deleting records. */
int sys_syslog(int type, char *buf, int len);

#endif
