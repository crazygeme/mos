#ifndef _DEV_PTS_INTERNAL_H_
#define _DEV_PTS_INTERNAL_H_

#include <lib/lock.h>
#include <lib/cyclebuf.h>
#include <fs/ioctl.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include "tty_ldisc.h"

#define TIOCGPTN 0x80045430 /* get pty number */
#define TIOCSPTLCK 0x40045431 /* lock/unlock slave */
#define TIOCGPTLCK 0x80045439 /* query slave lock state */

typedef struct {
	int idx;
	int used;
	spinlock_t lock;
	uint32_t slave_mode;
	uint32_t slave_uid;
	uint32_t slave_gid;
	int pt_locked;
	int pkt_mode;
	unsigned char pkt_status;

	struct termios termios;
	struct winsize winsize;
	unsigned pgrp;

	/*
	 * m2s: master is writer, slave is reader
	 * s2m: slave is writer, master is reader
	 */
	cy_buf *m2s;
	cy_buf *s2m;

	tty_canon_t canon;

	int master_open;
	int slave_count;
} pts_pair;

void pts_pair_check_free(pts_pair *p, spinlock_t *lock);

int pts_slave_setattr(inode *node, uint32_t mode);

int pts_slave_chown(inode *node, uint32_t uid, uint32_t gid);

int pts_master_ioctl(file *fp, unsigned type, void *buf);

ssize_t pts_master_read(file *fp, void *buf, size_t size, loff_t *pos);

ssize_t pts_master_write(file *fp, const void *buf, size_t size, loff_t *pos);

unsigned pts_master_poll(file *fp, unsigned events, poll_table *pt);

int pts_slave_ioctl(file *fp, unsigned cmd, void *buf);

int pts_slave_path_release(file *fp);

ssize_t pts_slave_read(file *fp, void *buf, size_t size, loff_t *pos);

ssize_t pts_slave_write(file *fp, const void *buf, size_t size, loff_t *pos);

unsigned pts_slave_poll(file *fp, unsigned events, poll_table *pt);

#endif
