/* Linux ALSA playback ABI with a bounded PCM ring and completion-based position. */
#include <driver/audio/alsa.h>
#include <driver/audio/alsa_abi.h>
#include <device/devnode.h>
#include <fs/entries.h>
#include <fs/fcntl.h>
#include <lib/klib.h>
#include <ps/ps.h>
#include <proc/proc.h>
#include <errno.h>
#include <macro.h>

#define PCM_BYTES (256 * 1024)
#define PCM_FLAGS                                          \
	(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | \
	 SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BATCH)
struct pcm {
	mutex_t lock;
	spinlock_t wait_lock;
	list_entry waiters;
	file *backend;
	unsigned char *data;
	unsigned char transfer[1536 * 4];
	struct snd_pcm_mmap_status *status;
	struct snd_pcm_mmap_control *control;
	struct snd_pcm_sw_params sw;
	unsigned frames, period, access, closing, exited;
	unsigned long submitted, completed;
};
static file *(*backend_open)(super_block *, unsigned, int);
static mutex_t card_lock;
static unsigned card_initialized, online;
static struct pcm *owner;
static spinlock_t work_lock = SPINLOCK_INITIALIZER;
static task_struct *work_task;
static unsigned work_sequence;
void alsa_wake(void)
{
	int irq;
	spinlock_lock(&work_lock, &irq);
	__atomic_add_fetch(&work_sequence, 1, __ATOMIC_RELEASE);
	if (work_task) {
		ps_put_to_ready_queue(work_task);
		if (ps_enabled() && current != work_task)
			current->sched->remain_ticks = 0;
	}
	spinlock_unlock(&work_lock, irq);
}

static void pcm_sleep(void)
{
	ps_prepare_interruptible_wait(current, NULL, 1);
	task_sched();
	ps_finish_timed_wait(current);
}
static void pcm_notify(struct pcm *p)
{
	int irq;
	spinlock_lock(&p->wait_lock, &irq);
	poll_notify(&p->waiters);
	spinlock_unlock(&p->wait_lock, irq);
}
static unsigned long pcm_distance(struct pcm *p, unsigned long a,
				  unsigned long b)
{
	return a >= b ? a - b : p->sw.boundary - b + a;
}
static unsigned long pcm_advance(struct pcm *p, unsigned long ptr,
				 unsigned long n)
{
	return (ptr + n) % p->sw.boundary;
}
static void pcm_time(struct snd_abi_timespec *t)
{
	unsigned long long us = time_now_us();
	t->tv_sec = us / 1000000;
	t->tv_nsec = us % 1000000 * 1000;
}
static unsigned pcm_available(struct pcm *p)
{
	unsigned long used =
		pcm_distance(p, p->control->appl_ptr, p->status->hw_ptr);
	return used <= p->frames ? p->frames - used : 0;
}
static void pcm_update(struct pcm *p)
{
	int delay = 0;
	int ret = p->backend->f_fop->ioctl(p->backend, 0x80045017, &delay);
	if (ret < 0 || !online) {
		p->status->state = SNDRV_PCM_STATE_DISCONNECTED;
	} else {
		unsigned long pending = delay > 0 ? (unsigned)delay / 4 : 0;
		unsigned long done =
			p->submitted >= pending ? p->submitted - pending : 0;
		p->completed = done;
		p->status->hw_ptr = done % p->sw.boundary;
	}
	pcm_time(&p->status->tstamp);
}
static void pcm_worker(void *arg)
{
	struct pcm *p = arg;
	int irq;
	spinlock_lock(&work_lock, &irq);
	work_task = current;
	spinlock_unlock(&work_lock, irq);
	for (;;) {
		unsigned seen =
			__atomic_load_n(&work_sequence, __ATOMIC_ACQUIRE);
		mutex_lock(&p->lock);
		if (p->closing) {
			mutex_unlock(&p->lock);
			break;
		}
		int old_state = p->status->state;
		unsigned long old_hw = p->status->hw_ptr;
		pcm_update(p);
		int state = p->status->state;
		if (state == SNDRV_PCM_STATE_PREPARED &&
		    pcm_distance(p, p->control->appl_ptr, p->status->hw_ptr) >=
			    p->sw.start_threshold) {
			p->status->state = state = SNDRV_PCM_STATE_RUNNING;
		}
		if (state == SNDRV_PCM_STATE_RUNNING ||
		    state == SNDRV_PCM_STATE_DRAINING) {
			unsigned long queued =
				pcm_distance(p, p->control->appl_ptr,
					     p->submitted % p->sw.boundary);
			if (queued > p->frames) {
				p->status->state = SNDRV_PCM_STATE_XRUN;
			} else if (queued) {
				/* USB packets remain whole across PCM ring wrap boundaries. */
				unsigned n = queued > 1536 ? 1536 : queued;
				if (state != SNDRV_PCM_STATE_DRAINING)
					n -= n % 48;
				unsigned offset = p->submitted % p->frames;
				unsigned first = n > p->frames - offset ?
							 p->frames - offset :
							 n;
				memcpy(p->transfer, p->data + offset * 4,
				       first * 4);
				if (n > first)
					memcpy(p->transfer + first * 4, p->data,
					       (n - first) * 4);
				loff_t pos = 0;
				ssize_t ret = n ? p->backend->f_fop->write(
							  p->backend,
							  p->transfer, n * 4,
							  &pos) :
						  0;
				if (ret > 0)
					p->submitted += ret / 4;
				else if (ret < 0 && ret != -EAGAIN)
					p->status->state =
						ret == -ENODEV ?
							SNDRV_PCM_STATE_DISCONNECTED :
							SNDRV_PCM_STATE_XRUN;
			} else if (state == SNDRV_PCM_STATE_DRAINING) {
				int ret = p->backend->f_fop->ioctl(
					p->backend, 0x5001, NULL);
				pcm_update(p);
				p->status->state =
					ret == -ENODEV ?
						SNDRV_PCM_STATE_DISCONNECTED :
					ret < 0 ? SNDRV_PCM_STATE_XRUN :
						  SNDRV_PCM_STATE_SETUP;
			} else if (p->completed == p->submitted &&
				   p->sw.stop_threshold <= p->frames) {
				p->status->state = SNDRV_PCM_STATE_XRUN;
			}
		}
		if (old_state != p->status->state ||
		    old_hw != p->status->hw_ptr)
			pcm_notify(p);
		mutex_unlock(&p->lock);
		spinlock_lock(&work_lock, &irq);
		ps_prepare_interruptible_wait(current, NULL, 1);
		if (__atomic_load_n(&work_sequence, __ATOMIC_ACQUIRE) != seen)
			ps_put_to_ready_queue(current);
		spinlock_unlock(&work_lock, irq);
		task_sched();
		ps_finish_timed_wait(current);
	}
	spinlock_lock(&work_lock, &irq);
	work_task = NULL;
	spinlock_unlock(&work_lock, irq);
	__atomic_store_n(&p->exited, 1, __ATOMIC_RELEASE);
}
static void pcm_info(struct snd_pcm_info *i)
{
	memset(i, 0, sizeof(*i));
	strcpy((char *)i->id, "MOSUSB");
	strcpy((char *)i->name, "MOS USB Audio");
	strcpy((char *)i->subname, "Playback");
	i->subdevices_count = 1;
	i->subdevices_avail = owner ? 0 : 1;
}
static int interval_limit(struct snd_pcm_hw_params *h, unsigned n, unsigned lo,
			  unsigned hi)
{
	struct snd_interval *i =
		&h->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
	unsigned a = i->min, b = i->max;
	if (i->empty || a > hi || b < lo)
		return -EINVAL;
	if (a < lo) {
		a = lo;
		i->openmin = 0;
	}
	if (b > hi) {
		b = hi;
		i->openmax = 0;
	}
	if (i->openmin) {
		if (a == ~0u)
			return -EINVAL;
		a++;
	}
	if (i->openmax) {
		if (!b)
			return -EINVAL;
		b--;
	}
	if (a > b)
		return -EINVAL;
	i->openmin = i->openmax = 0;
	i->min = a;
	i->max = b;
	i->integer = 1;
	h->cmask |= 1u << n;
	return 0;
}
static int mask_limit(struct snd_pcm_hw_params *h, unsigned n, unsigned mask)
{
	struct snd_mask *m = &h->masks[n];
	unsigned bits = m->bits[0] & mask;
	memset(m, 0, sizeof(*m));
	m->bits[0] = bits;
	h->cmask |= 1u << n;
	return bits ? 0 : -EINVAL;
}
static int pcm_refine(struct snd_pcm_hw_params *h)
{
	h->cmask = 0;
	if (mask_limit(h, 0, (1u << 0) | (1u << 3)) ||
	    mask_limit(h, 1, 1u << 2) || mask_limit(h, 2, 1))
		return -EINVAL;
	static const unsigned limits[][3] = {
		{ 8, 16, 16 },		{ 9, 32, 32 },
		{ 10, 2, 2 },		{ 11, 48000, 48000 },
		{ 12, 1000, 1000000 },	{ 13, 48, 48000 },
		{ 14, 192, 192000 },	{ 15, 2, 1024 },
		{ 16, 2000, 1365334 },	{ 17, 96, PCM_BYTES / 4 },
		{ 18, 384, PCM_BYTES }, { 19, 0, 0 }
	};
	for (unsigned i = 0; i < sizeof(limits) / sizeof(limits[0]); i++)
		if (interval_limit(h, limits[i][0], limits[i][1], limits[i][2]))
			return -EINVAL;
	/* Propagate frame, byte, time and period-count constraints to a fixed point. */
	for (unsigned pass = 0; pass < 8; pass++) {
		struct snd_interval *v = h->intervals;
		if (interval_limit(h, 13, (v[6].min + 3) / 4, v[6].max / 4) ||
		    interval_limit(h, 13, (v[4].min * 48 + 999) / 1000,
				   (uint64_t)v[4].max * 48 / 1000) ||
		    interval_limit(h, 14, v[5].min * 4, v[5].max * 4) ||
		    interval_limit(h, 12, (uint64_t)v[5].min * 1000 / 48,
				   ((uint64_t)v[5].max * 1000 + 47) / 48) ||
		    interval_limit(h, 17, (v[10].min + 3) / 4, v[10].max / 4) ||
		    interval_limit(h, 17, (v[8].min * 48 + 999) / 1000,
				   (uint64_t)v[8].max * 48 / 1000) ||
		    interval_limit(h, 18, v[9].min * 4, v[9].max * 4) ||
		    interval_limit(h, 16, (uint64_t)v[9].min * 1000 / 48,
				   ((uint64_t)v[9].max * 1000 + 47) / 48) ||
		    interval_limit(h, 17, v[5].min * v[7].min,
				   (uint64_t)v[5].max * v[7].max >
						   PCM_BYTES / 4 ?
					   PCM_BYTES / 4 :
					   v[5].max * v[7].max) ||
		    interval_limit(h, 15, (v[9].min + v[5].max - 1) / v[5].max,
				   v[9].max / v[5].min) ||
		    interval_limit(h, 13, (v[9].min + v[7].max - 1) / v[7].max,
				   v[9].max / v[7].min))
			return -EINVAL;
	}
	h->info = PCM_FLAGS;
	h->msbits = 16;
	h->rate_num = 48000;
	h->rate_den = 1;
	h->fifo_size = 0;
	h->rmask = 0;
	return 0;
}
static int pcm_ioctl(file *fp, unsigned cmd, void *arg)
{
	struct pcm *p = fp->f_inode->i_private;
	if (((cmd >> 8) & 255) != 'A')
		return -ENOTTY;
	unsigned nr = cmd & 255;
	if (((cmd >> 16) & 0x3fff) && !arg)
		return -EFAULT;
	mutex_lock(&p->lock);
	int ret = 0;
	switch (nr) {
	case 0:
		*(int *)arg = SNDRV_PROTOCOL_VERSION(2, 0, 15);
		break;
	case 1:
		pcm_info(arg);
		break;
	case 2:
	case 3:
	case 4:
		break;
	case 0x10:
		ret = pcm_refine(arg);
		break;
	case 0x11: {
		if (p->status->state == SNDRV_PCM_STATE_RUNNING ||
		    p->status->state == SNDRV_PCM_STATE_DRAINING) {
			ret = -EBUSY;
			break;
		}
		struct snd_pcm_hw_params *h = arg;
		ret = pcm_refine(h);
		if (ret)
			break;
		p->period = h->intervals[13 - 8].min;
		p->frames = h->intervals[17 - 8].min;
		if (p->frames < p->period * 2 || p->frames > PCM_BYTES / 4) {
			ret = -EINVAL;
			break;
		}
		p->access = h->masks[0].bits[0] & 1 ? 0 : 3;
		h->masks[0].bits[0] = 1u << p->access;
		for (unsigned n = 8; n <= 19; n++) {
			unsigned val = h->intervals[n - 8].min;
			switch (n) {
			case 12:
				val = p->period * 1000 / 48;
				break;
			case 13:
				val = p->period;
				break;
			case 14:
				val = p->period * 4;
				break;
			case 15:
				val = p->frames / p->period;
				break;
			case 16:
				val = p->frames * 1000 / 48;
				break;
			case 17:
				val = p->frames;
				break;
			case 18:
				val = p->frames * 4;
				break;
			}
			h->intervals[n - 8] = (struct snd_interval){
				.min = val, .max = val, .integer = 1
			};
		}
		p->status->state = SNDRV_PCM_STATE_SETUP;
		p->sw.boundary = p->frames;
		while (p->sw.boundary <= (~0ul >> 2))
			p->sw.boundary *= 2;
		p->sw.start_threshold = p->period;
		p->sw.stop_threshold = p->frames;
		p->control->avail_min = p->period;
		break;
	}
	case 0x12:
		p->backend->f_fop->ioctl(p->backend, 0x5000, NULL);
		p->status->state = SNDRV_PCM_STATE_OPEN;
		p->frames = p->period = 0;
		break;
	case 0x13: {
		struct snd_pcm_sw_params *s = arg;
		if (!p->frames || s->boundary < p->frames ||
		    s->boundary % p->frames || !s->avail_min ||
		    s->avail_min > p->frames) {
			ret = -EINVAL;
			break;
		}
		p->sw = *s;
		p->control->avail_min = s->avail_min;
		break;
	}
	case 0x20:
	case 0x24: {
		pcm_update(p);
		struct snd_pcm_status *s = arg;
		memset(s, 0, sizeof(*s));
		s->state = p->status->state;
		s->tstamp = p->status->tstamp;
		s->appl_ptr = p->control->appl_ptr;
		s->hw_ptr = p->status->hw_ptr;
		s->avail = s->avail_max = pcm_available(p);
		s->delay = p->frames - s->avail;
		break;
	}
	case 0x21:
		pcm_update(p);
		*(long *)arg = p->frames - pcm_available(p);
		break;
	case 0x22:
		pcm_update(p);
		break;
	case 0x23: {
		struct snd_pcm_sync_ptr *s = arg;
		if (!(s->flags & SNDRV_PCM_SYNC_PTR_APPL)) {
			if (s->c.control.appl_ptr >= p->sw.boundary) {
				ret = -EINVAL;
				break;
			}
			p->control->appl_ptr = s->c.control.appl_ptr;
		}
		if (!(s->flags & SNDRV_PCM_SYNC_PTR_AVAIL_MIN))
			p->control->avail_min = s->c.control.avail_min;
		pcm_update(p);
		s->s.status = *p->status;
		s->c.control = *p->control;
		break;
	}
	case 0x32: {
		struct snd_pcm_channel_info *c = arg;
		if (c->channel >= 2) {
			ret = -EINVAL;
			break;
		}
		c->offset = 0;
		c->first = c->channel * 16;
		c->step = 32;
		break;
	}
	case 0x40:
	case 0x41:
		if (!p->frames) {
			ret = -EBADFD;
			break;
		}
		p->backend->f_fop->ioctl(p->backend, 0x5000, NULL);
		p->submitted = p->completed = p->control->appl_ptr =
			p->status->hw_ptr = 0;
		p->status->state = SNDRV_PCM_STATE_PREPARED;
		break;
	case 0x42:
		if (p->status->state != SNDRV_PCM_STATE_PREPARED)
			ret = -EBADFD;
		else
			p->status->state = SNDRV_PCM_STATE_RUNNING;
		break;
	case 0x43:
		p->backend->f_fop->ioctl(p->backend, 0x5000, NULL);
		p->status->state = SNDRV_PCM_STATE_SETUP;
		break;
	case 0x44:
		if (p->status->state == SNDRV_PCM_STATE_SETUP)
			break;
		if (p->status->state == SNDRV_PCM_STATE_XRUN) {
			ret = -EPIPE;
			break;
		}
		if (p->status->state == SNDRV_PCM_STATE_DISCONNECTED) {
			ret = -ENODEV;
			break;
		}
		if (p->status->state == SNDRV_PCM_STATE_OPEN) {
			ret = -EBADFD;
			break;
		}
		p->status->state = SNDRV_PCM_STATE_DRAINING;
		if (fp->f_flag & O_NONBLOCK) {
			ret = -EAGAIN;
			break;
		}
		mutex_unlock(&p->lock);
		for (;;) {
			pcm_sleep();
			mutex_lock(&p->lock);
			if (p->status->state != SNDRV_PCM_STATE_DRAINING)
				break;
			if (ps_interrupting_signals(current)) {
				ret = -EINTR;
				break;
			}
			mutex_unlock(&p->lock);
		}
		if (p->status->state == SNDRV_PCM_STATE_XRUN)
			ret = -EPIPE;
		if (p->status->state == SNDRV_PCM_STATE_DISCONNECTED)
			ret = -ENODEV;
		break;
	case 0x46: {
		unsigned long n = *(unsigned long *)arg;
		unsigned long queued = pcm_distance(
			p, p->control->appl_ptr, p->submitted % p->sw.boundary);
		if (queued > p->frames) {
			ret = -EPIPE;
			break;
		}
		if (n > queued)
			n = queued;
		p->control->appl_ptr = pcm_advance(p, p->control->appl_ptr,
						   p->sw.boundary - n);
		*(unsigned long *)arg = n;
		break;
	}
	case 0x49: {
		unsigned long n = *(unsigned long *)arg;
		unsigned available = pcm_available(p);
		if (n > available)
			n = available;
		p->control->appl_ptr = pcm_advance(p, p->control->appl_ptr, n);
		*(unsigned long *)arg = n;
		break;
	}
	case 0x48:
		p->status->state = SNDRV_PCM_STATE_XRUN;
		break;
	case 0x50: {
		struct snd_xferi *x = arg;
		if (!x->buf && x->frames) {
			ret = -EFAULT;
			break;
		}
		unsigned long done = 0;
		if (p->access != 3 || !p->frames) {
			ret = -EBADFD;
			break;
		}
		while (done < x->frames) {
			if (p->status->state == SNDRV_PCM_STATE_XRUN) {
				ret = -EPIPE;
				break;
			}
			if (p->status->state != SNDRV_PCM_STATE_PREPARED &&
			    p->status->state != SNDRV_PCM_STATE_RUNNING) {
				ret = -EBADFD;
				break;
			}
			pcm_update(p);
			unsigned n = pcm_available(p),
				 offset = p->control->appl_ptr % p->frames;
			if (n > x->frames - done)
				n = x->frames - done;
			if (n > p->frames - offset)
				n = p->frames - offset;
			if (!n) {
				if (fp->f_flag & O_NONBLOCK) {
					ret = -EAGAIN;
					break;
				}
				mutex_unlock(&p->lock);
				pcm_sleep();
				mutex_lock(&p->lock);
				if (ps_interrupting_signals(current)) {
					ret = -EINTR;
					break;
				}
				continue;
			}
			memcpy(p->data + offset * 4, (char *)x->buf + done * 4,
			       n * 4);
			p->control->appl_ptr =
				pcm_advance(p, p->control->appl_ptr, n);
			done += n;
		}
		x->result = done ? (long)done : ret;
		if (done)
			ret = 0;
		break;
	}
	default:
		ret = -ENOTTY;
	}
	pcm_notify(p);
	mutex_unlock(&p->lock);
	alsa_wake();
	return ret;
}
static unsigned pcm_poll(file *fp, unsigned events, poll_table *pt)
{
	struct pcm *p = fp->f_inode->i_private;
	mutex_lock(&p->lock);
	if (pt)
		poll_subscribe(pt, &p->waiters, &p->wait_lock);
	pcm_update(p);
	unsigned ready = p->status->state == SNDRV_PCM_STATE_XRUN || !online ?
				 FS_POLL_ERR :
			 pcm_available(p) >= p->control->avail_min ?
				 FS_POLL_WRITE :
				 0;
	mutex_unlock(&p->lock);
	return events & ready;
}
static int pcm_map(file *fp, uint64_t *offset, size_t size, unsigned prot,
		   unsigned flags, file **backing)
{
	if ((prot & PROT_EXEC) || (flags & MAP_TYPE) != MAP_SHARED)
		return -EINVAL;
	struct pcm *p = fp->f_inode->i_private;
	if (!size || (*offset % PAGE_SIZE))
		return -EINVAL;
	if (*offset == SNDRV_PCM_MMAP_OFFSET_STATUS && (prot & PROT_WRITE))
		return -EACCES;
	if (*offset == SNDRV_PCM_MMAP_OFFSET_STATUS ||
	    *offset == SNDRV_PCM_MMAP_OFFSET_CONTROL) {
		if (size > PAGE_SIZE)
			return -EINVAL;
	} else if (*offset >= p->frames * 4 ||
		   size > ((p->frames * 4 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)) -
				   *offset)
		return -EINVAL;
	fs_get_file(fp);
	*backing = fp;
	return 0;
}
static paddr_t pcm_page(file *fp, uint64_t offset)
{
	struct pcm *p = fp->f_inode->i_private;
	if (offset == SNDRV_PCM_MMAP_OFFSET_STATUS)
		return VIRT_TO_PHY(p->status);
	if (offset == SNDRV_PCM_MMAP_OFFSET_CONTROL)
		return VIRT_TO_PHY(p->control);
	if (offset < PCM_BYTES)
		return VIRT_TO_PHY(p->data + offset);
	return 0;
}
static int pcm_stat(file *fp, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = S_IFCHR | 0666;
	s->st_rdev = MKDEV(116, fp->f_fop->ioctl == pcm_ioctl ? 16 : 0);
	s->st_nlink = 1;
	s->st_blksize = PAGE_SIZE;
	return 0;
}
static int pcm_close(file *fp)
{
	struct pcm *p = fp->f_inode->i_private;
	mutex_lock(&p->lock);
	p->closing = 1;
	mutex_unlock(&p->lock);
	alsa_wake();
	while (!__atomic_load_n(&p->exited, __ATOMIC_ACQUIRE))
		pcm_sleep();
	fs_put_file(p->backend);
	mutex_lock(&card_lock);
	if (owner == p)
		owner = NULL;
	mutex_unlock(&card_lock);
	vm_free((vaddr_t)p->data, PCM_BYTES / PAGE_SIZE);
	vm_free((vaddr_t)p->status, 1);
	vm_free((vaddr_t)p->control, 1);
	free(p);
	free(fp->f_inode);
	free(fp);
	return 0;
}
static ssize_t pcm_write(file *fp, const void *buffer, size_t size, loff_t *pos)
{
	(void)pos;
	if (size % 4)
		return -EINVAL;
	if (!buffer && size)
		return -EFAULT;
	struct snd_xferi transfer = { .buf = (void *)buffer,
				      .frames = size / 4 };
	int ret = pcm_ioctl(fp, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &transfer);
	return ret < 0		   ? ret :
	       transfer.result < 0 ? transfer.result :
				     transfer.result * 4;
}
static const file_operations pcm_ops = { .write = pcm_write,
					 .ioctl = pcm_ioctl,
					 .poll = pcm_poll,
					 .getattr = pcm_stat,
					 .mmap_file = pcm_map,
					 .map_page = pcm_page,
					 .release = pcm_close };
static file *pcm_open(super_block *sb, unsigned dev, int flags)
{
	(void)sb;
	(void)dev;
	mutex_lock(&card_lock);
	if (!online || owner) {
		mutex_unlock(&card_lock);
		return NULL;
	}
	file *backend = backend_open(NULL, 0, O_WRONLY | O_NONBLOCK);
	if (!backend) {
		mutex_unlock(&card_lock);
		return NULL;
	}
	struct pcm *p = zalloc(sizeof(*p));
	file *fp = zalloc(sizeof(*fp));
	inode *node = zalloc(sizeof(*node));
	if (!p || !fp || !node) {
		free(p);
		free(fp);
		free(node);
		fs_put_file(backend);
		mutex_unlock(&card_lock);
		return NULL;
	}
	p->backend = backend;
	mutex_init(&p->lock);
	spinlock_init(&p->wait_lock);
	list_init(&p->waiters);
	p->data = (void *)vm_alloc(PCM_BYTES / PAGE_SIZE);
	p->status = (void *)vm_alloc(1);
	p->control = (void *)vm_alloc(1);
	if (!p->data || !p->status || !p->control) {
		if (p->data)
			vm_free((vaddr_t)p->data, PCM_BYTES / PAGE_SIZE);
		if (p->status)
			vm_free((vaddr_t)p->status, 1);
		if (p->control)
			vm_free((vaddr_t)p->control, 1);
		free(p);
		free(fp);
		free(node);
		fs_put_file(backend);
		mutex_unlock(&card_lock);
		return NULL;
	}
	memset(p->data, 0, PCM_BYTES);
	memset(p->status, 0, PAGE_SIZE);
	memset(p->control, 0, PAGE_SIZE);
	p->sw.boundary = 1ul << 30;
	p->control->avail_min = 1;
	p->status->state = SNDRV_PCM_STATE_OPEN;
	node->i_mode = S_IFCHR | 0666;
	node->i_private = p;
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_flag = flags;
	fp->f_mode = flags & O_ACCMODE;
	fp->f_fop = &pcm_ops;
	owner = p;
	int tid = ps_create(pcm_worker, p, ps_deferred, ps_kernel);
	if (tid < 0) {
		owner = NULL;
		vm_free((vaddr_t)p->data, PCM_BYTES / PAGE_SIZE);
		vm_free((vaddr_t)p->status, 1);
		vm_free((vaddr_t)p->control, 1);
		free(p);
		free(node);
		free(fp);
		fs_put_file(backend);
		mutex_unlock(&card_lock);
		return NULL;
	}
	mutex_unlock(&card_lock);
	return fp;
}
struct ctl_file {
	unsigned subscribed, seen[2];
};
static unsigned mixer_volume[2] = { 100, 100 }, mixer_switch[2] = { 1, 1 };
static unsigned mixer_sequence[2];
static spinlock_t mixer_wait_lock;
static list_entry mixer_waiters;
static const char *const mixer_names[] = { "PCM Playback Volume",
					   "PCM Playback Switch" };
static void mixer_id(struct snd_ctl_elem_id *id, unsigned n)
{
	memset(id, 0, sizeof(*id));
	id->numid = n + 1;
	id->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strcpy((char *)id->name, mixer_names[n]);
}
static int mixer_find(struct snd_ctl_elem_id *id)
{
	if (id->numid >= 1 && id->numid <= 2)
		return id->numid - 1;
	if (id->numid || id->iface != SNDRV_CTL_ELEM_IFACE_MIXER ||
	    id->device || id->subdevice || id->index)
		return -ENOENT;
	for (unsigned n = 0; n < 2; n++)
		if (!strcmp((char *)id->name, mixer_names[n]))
			return n;
	return -ENOENT;
}
static int mixer_ioctl(file *fp, unsigned nr, void *arg)
{
	struct ctl_file *ctx = fp->f_inode->i_private;
	mutex_lock(&card_lock);
	int ret = 0;
	if (nr == 0x10) {
		struct snd_ctl_elem_list *l = arg;
		l->count = 2;
		l->used = 0;
		if (l->space && !l->pids)
			ret = -EFAULT;
		else
			for (unsigned n = l->offset;
			     n < 2 && l->used < l->space; n++)
				mixer_id(&l->pids[l->used++], n);
	} else if (nr == 0x16) {
		int v = *(int *)arg;
		if (v < 0)
			*(int *)arg = ctx->subscribed;
		else if (v <= 1) {
			ctx->subscribed = v;
			memcpy(ctx->seen, mixer_sequence, sizeof(ctx->seen));
		} else
			ret = -EINVAL;
	} else {
		struct snd_ctl_elem_id *id = arg;
		int n = mixer_find(id);
		if (n < 0) {
			ret = n;
			goto out;
		}
		mixer_id(id, n);
		if (nr == 0x11) {
			struct snd_ctl_elem_info *i = arg;
			memset((char *)i + sizeof(i->id), 0,
			       sizeof(*i) - sizeof(i->id));
			i->type = n ? SNDRV_CTL_ELEM_TYPE_BOOLEAN :
				      SNDRV_CTL_ELEM_TYPE_INTEGER;
			i->access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
			i->count = 2;
			i->value.integer.min = 0;
			i->value.integer.max = n ? 1 : 100;
			i->value.integer.step = 1;
		} else if (nr == 0x12 || nr == 0x13) {
			struct snd_ctl_elem_value *v = arg;
			unsigned *values = n ? mixer_switch : mixer_volume;
			if (v->indirect) {
				ret = -EINVAL;
				goto out;
			}
			if (nr == 0x13) {
				long left = v->value.integer.value[0],
				     right = v->value.integer.value[1];
				if (left < 0 || right < 0 ||
				    left > (n ? 1 : 100) ||
				    right > (n ? 1 : 100)) {
					ret = -EINVAL;
					goto out;
				}
				unsigned old[2] = { values[0], values[1] };
				values[0] = left;
				values[1] = right;
				file *f = online ? backend_open(NULL, 0,
								O_WRONLY) :
						   NULL;
				if (!f)
					ret = -ENODEV;
				else {
					unsigned volume =
						(mixer_switch[0] ?
							 mixer_volume[0] :
							 0) |
						((mixer_switch[1] ?
							  mixer_volume[1] :
							  0)
						 << 8);
					ret = f->f_fop->ioctl(f, 0xc0044d04,
							      &volume);
					fs_put_file(f);
				}
				if (ret < 0) {
					values[0] = old[0];
					values[1] = old[1];
					goto out;
				}
				if (values[0] != old[0] ||
				    values[1] != old[1]) {
					mixer_sequence[n]++;
					int irq;
					spinlock_lock(&mixer_wait_lock, &irq);
					poll_notify(&mixer_waiters);
					spinlock_unlock(&mixer_wait_lock, irq);
				}
			}
			v->value.integer.value[0] = values[0];
			v->value.integer.value[1] = values[1];
		} else
			ret = -ENOTTY;
	}
out:
	mutex_unlock(&card_lock);
	return ret;
}
static ssize_t ctl_read(file *fp, void *buffer, size_t size, loff_t *pos)
{
	(void)pos;
	if (size < sizeof(struct snd_ctl_event))
		return -EINVAL;
	struct ctl_file *ctx = fp->f_inode->i_private;
	for (;;) {
		mutex_lock(&card_lock);
		if (!ctx->subscribed) {
			mutex_unlock(&card_lock);
			return -EBADFD;
		}
		for (unsigned n = 0; n < 2; n++)
			if (ctx->seen[n] != mixer_sequence[n]) {
				struct snd_ctl_event e = { 0 };
				e.type = SNDRV_CTL_EVENT_ELEM;
				e.data.elem.mask = SNDRV_CTL_EVENT_MASK_VALUE;
				mixer_id(&e.data.elem.id, n);
				ctx->seen[n] = mixer_sequence[n];
				memcpy(buffer, &e, sizeof(e));
				mutex_unlock(&card_lock);
				return sizeof(e);
			}
		mutex_unlock(&card_lock);
		if (!online)
			return -ENODEV;
		if (fp->f_flag & O_NONBLOCK)
			return -EAGAIN;
		if (ps_interrupting_signals(current))
			return -EINTR;
		pcm_sleep();
	}
}
static unsigned ctl_poll(file *fp, unsigned events, poll_table *pt)
{
	struct ctl_file *c = fp->f_inode->i_private;
	mutex_lock(&card_lock);
	if (pt)
		poll_subscribe(pt, &mixer_waiters, &mixer_wait_lock);
	unsigned ready = !online ? FS_POLL_ERR :
			 c->subscribed && (c->seen[0] != mixer_sequence[0] ||
					   c->seen[1] != mixer_sequence[1]) ?
				   FS_POLL_READ :
				   0;
	mutex_unlock(&card_lock);
	return events & ready;
}

static int ctl_ioctl(file *fp, unsigned cmd, void *arg)
{
	if (((cmd >> 8) & 255) != 'U')
		return -ENOTTY;
	if (!arg)
		return -EFAULT;
	unsigned nr = cmd & 255;
	if (nr >= 0x10 && nr <= 0x16)
		return mixer_ioctl(fp, nr, arg);
	switch (nr) {
	case 0:
		*(int *)arg = SNDRV_CTL_VERSION;
		return 0;
	case 1: {
		struct snd_ctl_card_info *i = arg;
		memset(i, 0, sizeof(*i));
		strcpy((char *)i->id, "MOSUSB");
		strcpy((char *)i->driver, "MOSUSB");
		strcpy((char *)i->name, "MOS USB Audio");
		strcpy((char *)i->longname, "MOS USB Audio Class 1 playback");
		strcpy((char *)i->mixername, "MOS USB Mixer");
		return 0;
	}
	case 0x20:
	case 0x40:
		*(int *)arg = -1;
		return 0;
	case 0x30:
		*(int *)arg = *(int *)arg < 0 ? 0 : -1;
		return 0;
	case 0x31: {
		struct snd_pcm_info *i = arg;
		if (i->device || i->subdevice || i->stream)
			return -ENOENT;
		pcm_info(i);
		return 0;
	}
	case 0x32:
		return 0;
	case 0xd1:
		*(int *)arg = 0;
		return 0;
	default:
		return -ENOTTY;
	}
}
static int ctl_close(file *fp)
{
	free(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}
static const file_operations ctl_ops = { .read = ctl_read,
					 .poll = ctl_poll,
					 .ioctl = ctl_ioctl,
					 .getattr = pcm_stat,
					 .release = ctl_close };
static file *ctl_open(super_block *sb, unsigned dev, int flags)
{
	(void)sb;
	(void)dev;
	if (!online)
		return NULL;
	file *fp = zalloc(sizeof(*fp));
	inode *node = zalloc(sizeof(*node));
	if (!fp || !node) {
		free(fp);
		free(node);
		return NULL;
	}
	node->i_private = zalloc(sizeof(struct ctl_file));
	if (!node->i_private) {
		free(fp);
		free(node);
		return NULL;
	}
	node->i_mode = S_IFCHR | 0666;
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_flag = flags;
	fp->f_fop = &ctl_ops;
	return fp;
}
void alsa_register(file *(*open)(super_block *, unsigned, int))
{
	if (!card_initialized) {
		mutex_init(&card_lock);
		spinlock_init(&mixer_wait_lock);
		list_init(&mixer_waiters);
		card_initialized = 1;
	}
	mutex_lock(&card_lock);
	backend_open = open;
	online = 1;
	mutex_unlock(&card_lock);
	vfs_entry_device(devfs_entries(), "/snd/controlC0", S_IFCHR | 0666,
			 MKDEV(116, 0), "sound", ctl_open);
	vfs_entry_device(devfs_entries(), "/snd/pcmC0D0p", S_IFCHR | 0666,
			 MKDEV(116, 16), "sound", pcm_open);
}
void alsa_disconnect(void)
{
	online = 0;
	vfs_entry_remove(devfs_entries(), "/snd/pcmC0D0p");
	vfs_entry_remove(devfs_entries(), "/snd/controlC0");
}

static int alsa_proc_show(void *data, unsigned tag, char *buf,
			  unsigned capacity)
{
	(void)data;
	if (capacity < 1024)
		return -ENOSPC;
	switch (tag) {
	case 0:
		return online ?
			       sprintf(buf,
				       " 0 [MOSUSB         ]: MOSUSB - MOS USB Audio\n                      MOS USB Audio Class 1 playback\n") :
			       sprintf(buf, "--- no soundcards ---\n");
	case 1:
		return online ?
			       sprintf(buf,
				       "  0: [ 0]   : control\n 16: [ 0- 0]: digital audio playback\n") :
			       0;
	case 2:
		return online ?
			       sprintf(buf,
				       "00-00: MOS USB Audio : USB Audio Class 1 : playback 1\n") :
			       0;
	case 3:
		return sprintf(buf, "MOS ALSA PCM protocol 2.0.15\n");
	case 4:
		return sprintf(buf, "MOSUSB\n");
	case 5:
		return sprintf(
			buf,
			"card: 0\ndevice: 0\nsubdevice: 0\nstream: PLAYBACK\nid: MOSUSB\nname: MOS USB Audio\nsubname: Playback\nclass: 0\nsubclass: 0\nsubdevices_count: 1\nsubdevices_avail: %u\n",
			owner ? 0 : 1);
	}
	if (!card_initialized)
		return sprintf(buf, "closed\n");
	mutex_lock(&card_lock);
	struct pcm *p = owner;
	int len;
	if (!p)
		len = sprintf(buf, "closed\n");
	else {
		mutex_lock(&p->lock);
		if (tag == 6)
			len = sprintf(
				buf,
				"state: %d\nhw_ptr: %llu\nappl_ptr: %llu\ndelay: %u\navail: %u\n",
				p->status->state,
				(unsigned long long)p->status->hw_ptr,
				(unsigned long long)p->control->appl_ptr,
				p->frames - pcm_available(p), pcm_available(p));
		else if (tag == 7)
			len = p->frames ?
				      sprintf(buf,
					      "access: %s\nformat: S16_LE\nsubformat: STD\nchannels: 2\nrate: 48000 (48000/1)\nperiod_size: %u\nbuffer_size: %u\n",
					      p->access == 0 ?
						      "MMAP_INTERLEAVED" :
						      "RW_INTERLEAVED",
					      p->period, p->frames) :
				      sprintf(buf, "no setup\n");
		else
			len = sprintf(
				buf,
				"avail_min: %llu\nstart_threshold: %llu\nstop_threshold: %llu\nboundary: %llu\n",
				(unsigned long long)p->control->avail_min,
				(unsigned long long)p->sw.start_threshold,
				(unsigned long long)p->sw.stop_threshold,
				(unsigned long long)p->sw.boundary);
		mutex_unlock(&p->lock);
	}
	mutex_unlock(&card_lock);
	return len;
}
static const vfs_entry_attribute_ops alsa_proc_ops = { .show = alsa_proc_show };
static void alsa_proc_register(void)
{
	vfs_entry_node *root = procfs_entries();
	if (!root)
		return;
	vfs_entry_node *sound = vfs_entry_directory(root, "asound");
	const char *names[] = { "cards", "devices", "pcm", "version" };
	for (unsigned n = 0; n < 4; n++)
		vfs_entry_attribute(sound, names[n], 0444, &alsa_proc_ops, NULL,
				    n);
	vfs_entry_node *card = vfs_entry_directory(sound, "card0");
	vfs_entry_attribute(card, "id", 0444, &alsa_proc_ops, NULL, 4);
	vfs_entry_node *pcm = vfs_entry_directory(card, "pcm0p");
	vfs_entry_attribute(pcm, "info", 0444, &alsa_proc_ops, NULL, 5);
	vfs_entry_node *sub = vfs_entry_directory(pcm, "sub0");
	vfs_entry_attribute(sub, "info", 0444, &alsa_proc_ops, NULL, 5);
	vfs_entry_attribute(sub, "status", 0444, &alsa_proc_ops, NULL, 6);
	vfs_entry_attribute(sub, "hw_params", 0444, &alsa_proc_ops, NULL, 7);
	vfs_entry_attribute(sub, "sw_params", 0444, &alsa_proc_ops, NULL, 8);
}
KERNEL_INIT(4, alsa_proc_register);
