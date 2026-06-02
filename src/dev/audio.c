#include <dev/dev.h>
#include <errno.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <hw/audio.h>
#include <hw/time.h>
#include <lib/klib.h>
#include <macro.h>
#include <stdint.h>
#include <unistd.h>

#include "devnums.h"

#define SNDCTL_DSP_RESET 0x00005000
#define SNDCTL_DSP_SYNC 0x00005001
#define SNDCTL_DSP_SPEED 0xc0045002
#define SNDCTL_DSP_STEREO 0xc0045003
#define SNDCTL_DSP_GETBLKSIZE 0xc0045004
#define SNDCTL_DSP_SETFMT 0xc0045005
#define SNDCTL_DSP_CHANNELS 0xc0045006
#define SNDCTL_DSP_GETFMTS 0x8004500b

#define OSS_MIXER_VOLUME AUDIO_MIXER_VOLUME
#define OSS_MIXER_PCM AUDIO_MIXER_PCM
#define OSS_MIXER_SPEAKER AUDIO_MIXER_SPEAKER
#define OSS_MIXER_RECSRC 0xff
#define OSS_MIXER_DEVMASK 0xfe
#define OSS_MIXER_RECMASK 0xfd
#define OSS_MIXER_CAPS 0xfc
#define OSS_MIXER_STEREODEVS 0xfb
#define OSS_MIXER_INFO 101
#define OSS_MIXER_ACCESS 102
#define OSS_MIXER_AGC 103
#define OSS_MIXER_3DSE 104
#define OSS_MIXER_PRIVATE1 111
#define OSS_MIXER_PRIVATE5 115
#define OSS_MIXER_GETLEVELS 116
#define OSS_MIXER_SETLEVELS 117
#define OSS_GETVERSION 118
#define OSS_SOUND_VERSION 0x030802

#define OSS_MIXER_SUPPORTED                                 \
	((1u << OSS_MIXER_VOLUME) | (1u << OSS_MIXER_PCM) | \
	 (1u << OSS_MIXER_SPEAKER))

typedef struct {
	audio_dev *dev;
	unsigned rdev;
} dsp_file_ctx;

typedef struct {
	unsigned rdev;
} mixer_file_ctx;

typedef struct {
	char id[16];
	char name[32];
	int modify_counter;
	int fillers[10];
} mixer_info;

typedef struct {
	int num;
	char name[32];
	int levels[32];
} mixer_vol_table;

static int oss_stereo_volume(unsigned left, unsigned right)
{
	if (left > 100)
		left = 100;
	if (right > 100)
		right = 100;
	return (int)(left | (right << 8));
}

static void oss_fill_mixer_info(void *buf, unsigned size)
{
	mixer_info info;

	memset(&info, 0, sizeof(info));
	strcpy(info.id, "MOSAC97");
	strcpy(info.name, "MOS AC97 Mixer");
	info.modify_counter = 0;
	memcpy(buf, &info, size < sizeof(info) ? size : sizeof(info));
}

static void oss_fill_mixer_levels(mixer_vol_table *tbl)
{
	int i;

	if (!tbl)
		return;
	memset(tbl, 0, sizeof(*tbl));
	tbl->num = 32;
	strcpy(tbl->name, "MOS AC97 Mixer");
	for (i = 0; i < 32; i++)
		tbl->levels[i] = oss_stereo_volume(80, 80);
}

static audio_dev *oss_mixer_device(void)
{
	return audio_default_device();
}

static int oss_mixer_set_volume(unsigned nr, int *arg)
{
	audio_dev *dev = oss_mixer_device();
	unsigned left;
	unsigned right;
	int ret;

	if (!arg)
		return -EINVAL;
	left = (unsigned)(*arg & 0xff);
	right = (unsigned)((*arg >> 8) & 0xff);
	ret = audio_set_volume(dev, nr, &left, &right);
	if (ret < 0)
		return ret;
	*arg = oss_stereo_volume(left, right);
	return 0;
}

static int oss_mixer_get_volume(unsigned nr, int *arg)
{
	audio_dev *dev = oss_mixer_device();
	unsigned left;
	unsigned right;
	int ret;

	if (!arg)
		return -EINVAL;
	ret = audio_get_volume(dev, nr, &left, &right);
	if (ret < 0)
		return ret;
	*arg = oss_stereo_volume(left, right);
	return 0;
}

static int oss_mixer_ioctl(file *fp, unsigned cmd, void *buf)
{
	int *arg = (int *)buf;
	unsigned dir = cmd >> 30;
	unsigned type = (cmd >> 8) & 0xff;
	unsigned nr = cmd & 0xff;
	unsigned size = (cmd >> 16) & 0x3fff;

	(void)fp;

	if (!buf || type != 'M')
		return -EINVAL;

	if (nr == OSS_GETVERSION) {
		*arg = OSS_SOUND_VERSION;
		return 0;
	}
	if (nr == OSS_MIXER_INFO) {
		oss_fill_mixer_info(buf, size);
		return 0;
	}
	if (nr == OSS_MIXER_GETLEVELS) {
		oss_fill_mixer_levels((mixer_vol_table *)buf);
		return 0;
	}
	if (nr == OSS_MIXER_SETLEVELS)
		return 0;
	if (nr == OSS_MIXER_ACCESS || nr == OSS_MIXER_AGC ||
	    nr == OSS_MIXER_3DSE ||
	    (nr >= OSS_MIXER_PRIVATE1 && nr <= OSS_MIXER_PRIVATE5)) {
		*arg = 0;
		return 0;
	}
	if (nr == OSS_MIXER_DEVMASK || nr == OSS_MIXER_STEREODEVS) {
		*arg = OSS_MIXER_SUPPORTED;
		return 0;
	}
	if (nr == OSS_MIXER_RECMASK || nr == OSS_MIXER_CAPS ||
	    nr == OSS_MIXER_RECSRC) {
		*arg = 0;
		return 0;
	}
	if (nr > 31 || !(OSS_MIXER_SUPPORTED & (1u << nr)))
		return -EINVAL;

	if (dir & 1)
		return oss_mixer_set_volume(nr, arg);
	return oss_mixer_get_volume(nr, arg);
}

static int mixer_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	mixer_file_ctx *ctx = node->i_private;

	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_rdev = ctx ? ctx->rdev : 0;
	s->st_blksize = PAGE_SIZE;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_nlink = 1;
	return 0;
}

static int mixer_release(file *fp)
{
	free(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations mixer_fops = {
	.release = mixer_release,
	.getattr = mixer_getattr,
	.ioctl = oss_mixer_ioctl,
};

static file *mixer_cdev_open(super_block *dev_sb, unsigned rdev, int flag)
{
	mixer_file_ctx *ctx;
	inode *node;
	file *fp;

	(void)dev_sb;
	(void)flag;

	node = zalloc(sizeof(*node));
	ctx = zalloc(sizeof(*ctx));
	ctx->rdev = rdev;

	node->i_mode = S_IFCHR | 0666;
	node->i_private = ctx;

	fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &mixer_fops;
	return fp;
}

static ssize_t dsp_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	dsp_file_ctx *ctx = fp->f_inode->i_private;

	(void)pos;
	return audio_write(ctx ? ctx->dev : NULL, buf, size);
}

static ssize_t dsp_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	(void)fp;
	(void)buf;
	(void)size;
	(void)pos;
	return -ENODEV;
}

static unsigned dsp_poll(file *fp, unsigned events, poll_table *pt)
{
	dsp_file_ctx *ctx = fp->f_inode->i_private;

	(void)pt;
	if (!ctx || !ctx->dev)
		return events & FS_POLL_ERR;
	return events & FS_POLL_WRITE;
}

static int dsp_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	dsp_file_ctx *ctx = node->i_private;

	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_rdev = ctx ? ctx->rdev : 0;
	s->st_blksize = ctx ? audio_block_size(ctx->dev) : PAGE_SIZE;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_nlink = 1;
	return 0;
}

static int dsp_ioctl(file *fp, unsigned cmd, void *buf)
{
	dsp_file_ctx *ctx = fp->f_inode->i_private;
	audio_dev *dev = ctx ? ctx->dev : NULL;
	int *arg = (int *)buf;
	unsigned val;
	int ret;

	switch (cmd) {
	case SNDCTL_DSP_RESET:
		return audio_reset(dev);
	case SNDCTL_DSP_SYNC:
		return audio_sync(dev);
	case SNDCTL_DSP_GETBLKSIZE:
		if (arg)
			*arg = (int)audio_block_size(dev);
		return 0;
	case SNDCTL_DSP_GETFMTS:
		if (!arg)
			return -EINVAL;
		*arg = AUDIO_FMT_U8 | AUDIO_FMT_S16_LE;
		return 0;
	case SNDCTL_DSP_SPEED:
		if (!arg)
			return -EINVAL;
		val = (unsigned)*arg;
		ret = audio_set_rate(dev, &val);
		*arg = (int)val;
		return ret;
	case SNDCTL_DSP_STEREO:
		if (!arg)
			return -EINVAL;
		val = *arg ? 2u : 1u;
		ret = audio_set_channels(dev, &val);
		*arg = val == 2;
		return ret;
	case SNDCTL_DSP_CHANNELS:
		if (!arg)
			return -EINVAL;
		val = (unsigned)*arg;
		ret = audio_set_channels(dev, &val);
		*arg = (int)val;
		return ret;
	case SNDCTL_DSP_SETFMT:
		if (!arg)
			return -EINVAL;
		val = (unsigned)*arg;
		ret = audio_set_format(dev, &val);
		*arg = (int)val;
		return ret;
	default:
		return -EINVAL;
	}
}

static int dsp_release(file *fp)
{
	free(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations dsp_fops = {
	.release = dsp_release,
	.getattr = dsp_getattr,
	.read = dsp_read,
	.write = dsp_write,
	.poll = dsp_poll,
	.ioctl = dsp_ioctl,
};

static file *dsp_cdev_open(super_block *dev_sb, unsigned rdev, int flag)
{
	audio_dev *dev = audio_default_device();
	dsp_file_ctx *ctx;
	inode *node;
	file *fp;

	(void)dev_sb;
	(void)flag;

	if (!dev)
		return NULL;

	node = zalloc(sizeof(*node));
	ctx = zalloc(sizeof(*ctx));
	ctx->dev = dev;
	ctx->rdev = rdev;

	node->i_mode = S_IFCHR | 0666;
	node->i_private = ctx;

	fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &dsp_fops;
	return fp;
}

static void audio_dev_register(super_block *dev_sb)
{
	cdev_register_named(S_IFCHR, SOUND_MAJOR, SOUND_MIXER_MINOR, 1, "sound",
			    mixer_cdev_open);
	cdev_register_named(S_IFCHR, SOUND_MAJOR, SOUND_DSP_MINOR, 1, "sound",
			    dsp_cdev_open);
	cdev_register_named(S_IFCHR, SOUND_MAJOR, SOUND_AUDIO_MINOR, 1, "sound",
			    dsp_cdev_open);
	vfs_mknod(dev_sb, "/mixer", S_IFCHR | 0666,
		  MKDEV(SOUND_MAJOR, SOUND_MIXER_MINOR));
	vfs_mknod(dev_sb, "/dsp", S_IFCHR | 0666,
		  MKDEV(SOUND_MAJOR, SOUND_DSP_MINOR));
	vfs_mknod(dev_sb, "/audio", S_IFCHR | 0666,
		  MKDEV(SOUND_MAJOR, SOUND_AUDIO_MINOR));
	printk("dev: registered /dev/mixer, /dev/dsp, and /dev/audio\n");
}

DEV_INIT(audio_dev_register);
