#include <fs/entries.h>
#include <fs/fs.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <lib/klib.h>
#include <driver/driver.h>
#include <lib/lock.h>
#include <lib/port.h>
#include <macro.h>
#include <mm/mm.h>
#include <ps/ps.h>
#include <unistd.h>
#include <device/devnode.h>
#include <lib/command.h>
#include <fs/vfs.h>
#include <device/devnums.h>

static void audio_dev_register(void);

#define AUDIO_FMT_U8 0x00000008
#define AUDIO_FMT_S16_LE 0x00000010

#define AUDIO_MIXER_VOLUME 0
#define AUDIO_MIXER_PCM 4
#define AUDIO_MIXER_SPEAKER 5

#define AC97_VENDOR_INTEL 0x8086
#define AC97_DEVICE_ICH 0x2415

#define AC97_NAM_RESET 0x00
#define AC97_NAM_MASTER_VOL 0x02
#define AC97_NAM_PCM_VOL 0x18
#define AC97_NAM_EXT_AUDIO_ID 0x28
#define AC97_NAM_EXT_AUDIO_CTRL 0x2a
#define AC97_NAM_PCM_FRONT_RATE 0x2c

#define AC97_PO_BDBAR 0x10
#define AC97_PO_LVI 0x15
#define AC97_PO_SR 0x16
#define AC97_PO_CR 0x1b

#define AC97_SR_DCH 0x0001
#define AC97_SR_CELV 0x0002
#define AC97_SR_LVBCI 0x0004
#define AC97_SR_BCIS 0x0008
#define AC97_SR_FIFOE 0x0010

#define AC97_CR_RPBM 0x01
#define AC97_CR_RR 0x02

#define AC97_BD_IOC (1u << 31)

#define AC97_PLAY_BYTES (64 * 1024)
#define AC97_PLAY_TIMEOUT_MS 10000

typedef struct {
	uint32_t addr;
	uint32_t ctl_len;
} __attribute__((packed)) ac97_bd;

typedef struct {
	uint32_t pci_dev;
	uint16_t nam;
	uint16_t nabm;
	ac97_bd *bd;
	uint8_t *play_buf;
	uint32_t play_phys;
	unsigned rate;
	unsigned channels;
	unsigned format;
	unsigned master_left;
	unsigned master_right;
	unsigned pcm_left;
	unsigned pcm_right;
	unsigned speaker_left;
	unsigned speaker_right;
	mutex_t stream_lock;
	int ready;
} ac97_dev;

static ac97_dev g_ac97;

static inline uint16_t ac97_mixer_read(uint16_t reg)
{
	return port_read_word(g_ac97.nam + reg);
}

static inline void ac97_mixer_write(uint16_t reg, uint16_t val)
{
	port_write_word(g_ac97.nam + reg, val);
}

static inline uint8_t ac97_bm_readb(uint16_t reg)
{
	return port_read_byte(g_ac97.nabm + reg);
}

static inline uint16_t ac97_bm_readw(uint16_t reg)
{
	return port_read_word(g_ac97.nabm + reg);
}

static inline void ac97_bm_writeb(uint16_t reg, uint8_t val)
{
	port_write_byte(g_ac97.nabm + reg, val);
}

static inline void ac97_bm_writew(uint16_t reg, uint16_t val)
{
	port_write_word(g_ac97.nabm + reg, val);
}

static inline void ac97_bm_writed(uint16_t reg, uint32_t val)
{
	port_write_dword(g_ac97.nabm + reg, val);
}

static void ac97_reset_stream(void)
{
	unsigned i;

	ac97_bm_writeb(AC97_PO_CR, 0);
	for (i = 0; i < 10000; i++) {
		if (ac97_bm_readw(AC97_PO_SR) & AC97_SR_DCH)
			break;
		PAUSE();
	}

	ac97_bm_writeb(AC97_PO_CR, AC97_CR_RR);
	for (i = 0; i < 10000; i++) {
		if (!(ac97_bm_readb(AC97_PO_CR) & AC97_CR_RR))
			break;
		PAUSE();
	}
	ac97_bm_writew(AC97_PO_SR, AC97_SR_BCIS | AC97_SR_LVBCI |
					   AC97_SR_FIFOE | AC97_SR_CELV);
}

static int ac97_wait_done(void)
{
	unsigned long long deadline = time_now_ms() + AC97_PLAY_TIMEOUT_MS;
	int ret = 0;

	for (;;) {
		uint16_t sr = ac97_bm_readw(AC97_PO_SR);
		if (sr & AC97_SR_FIFOE) {
			ret = -EIO;
			break;
		}
		if (sr & (AC97_SR_BCIS | AC97_SR_DCH))
			break;
		if (time_now_ms() >= deadline) {
			ret = -ETIMEDOUT;
			break;
		}
		/* Release the CPU while retaining ownership of the DMA buffer.
		 * Signal interruption stops the stream through the cleanup below.
		 */
		if (ps_prepare_interruptible_wait(current, NULL, 1, __func__) <
		    0) {
			ret = -EINTR;
			break;
		}
		task_sched();
		ps_finish_timed_wait(current);
		if (ps_interrupting_signals(current)) {
			ret = -EINTR;
			break;
		}
	}
	ac97_bm_writeb(AC97_PO_CR, 0);
	ac97_bm_writew(AC97_PO_SR, AC97_SR_BCIS | AC97_SR_LVBCI |
					   AC97_SR_FIFOE | AC97_SR_CELV);
	return ret;
}

static unsigned ac97_bytes_per_frame(void)
{
	unsigned bytes = g_ac97.format == AUDIO_FMT_U8 ? 1 : 2;
	return bytes * g_ac97.channels;
}

static int ac97_set_rate_value(unsigned *rate)
{
	uint16_t ext_id = ac97_mixer_read(AC97_NAM_EXT_AUDIO_ID);
	unsigned value;

	if (!rate)
		return -EINVAL;

	value = *rate;
	if (value < 4000)
		value = 4000;
	if (value > 48000)
		value = 48000;

	g_ac97.rate = value;
	if (ext_id & 1) {
		ac97_mixer_write(AC97_NAM_EXT_AUDIO_CTRL,
				 ac97_mixer_read(AC97_NAM_EXT_AUDIO_CTRL) | 1);
		ac97_mixer_write(AC97_NAM_PCM_FRONT_RATE, (uint16_t)value);
	}
	*rate = value;
	return 0;
}

static unsigned ac97_clamp_volume(unsigned level)
{
	return level > 100 ? 100 : level;
}

static uint16_t ac97_stereo_atten(unsigned left, unsigned right)
{
	unsigned l_att;
	unsigned r_att;

	left = ac97_clamp_volume(left);
	right = ac97_clamp_volume(right);
	if (left == 0 && right == 0)
		return 0x8000;

	l_att = ((100 - left) * 31 + 50) / 100;
	r_att = ((100 - right) * 31 + 50) / 100;
	return (uint16_t)((l_att << 8) | r_att);
}

static void ac97_apply_master(unsigned left, unsigned right)
{
	ac97_mixer_write(AC97_NAM_MASTER_VOL, ac97_stereo_atten(left, right));
}

static void ac97_apply_pcm(unsigned left, unsigned right)
{
	ac97_mixer_write(AC97_NAM_PCM_VOL, ac97_stereo_atten(left, right));
}

static const struct ac97_volume_control {
	unsigned *left, *right;
	void (*apply)(unsigned, unsigned);
} ac97_volume_controls[] = {
	[AUDIO_MIXER_VOLUME] = { &g_ac97.master_left, &g_ac97.master_right,
				 ac97_apply_master },
	[AUDIO_MIXER_PCM] = { &g_ac97.pcm_left, &g_ac97.pcm_right,
			      ac97_apply_pcm },
	[AUDIO_MIXER_SPEAKER] = { &g_ac97.speaker_left, &g_ac97.speaker_right,
				  NULL },
};

static const struct ac97_volume_control *ac97_volume_control(unsigned control)
{
	if (control >= sizeof(ac97_volume_controls) /
			       sizeof(ac97_volume_controls[0]) ||
	    !ac97_volume_controls[control].left)
		return NULL;
	return &ac97_volume_controls[control];
}

static int ac97_set_volume_values(unsigned control, unsigned *left,
				  unsigned *right)
{
	const struct ac97_volume_control *volume = ac97_volume_control(control);
	if (!left || !right || !volume)
		return -EINVAL;
	unsigned l = ac97_clamp_volume(*left), r = ac97_clamp_volume(*right);
	*volume->left = l;
	*volume->right = r;
	if (volume->apply)
		volume->apply(l, r);
	*left = l;
	*right = r;
	return 0;
}

static int ac97_get_volume_values(unsigned control, unsigned *left,
				  unsigned *right)
{
	const struct ac97_volume_control *volume = ac97_volume_control(control);
	if (!left || !right || !volume)
		return -EINVAL;
	*left = *volume->left;
	*right = *volume->right;
	return 0;
}

static ssize_t ac97_write(void *_dev, const void *buf, size_t size)
{
	size_t done = 0;
	unsigned frame_bytes;
	int ret = 0;

	(void)_dev;

	if (!g_ac97.ready)
		return -ENODEV;

	/* The descriptor and DMA buffer remain owned while the writer sleeps. */
	mutex_lock(&g_ac97.stream_lock);
	frame_bytes = ac97_bytes_per_frame();
	while (done < size) {
		size_t chunk = size - done;
		uint32_t samples;

		if (g_ac97.format == AUDIO_FMT_U8) {
			if (chunk > AC97_PLAY_BYTES / 2)
				chunk = AC97_PLAY_BYTES / 2;
		} else if (chunk > AC97_PLAY_BYTES) {
			chunk = AC97_PLAY_BYTES;
		}
		chunk -= chunk % frame_bytes;
		if (chunk == 0)
			break;

		if (g_ac97.format == AUDIO_FMT_U8) {
			const uint8_t *src = (const uint8_t *)buf + done;
			int16_t *dst = (int16_t *)g_ac97.play_buf;
			size_t i;

			for (i = 0; i < chunk; i++)
				dst[i] = (int16_t)(((int)src[i] - 128) << 8);
			samples = (uint32_t)chunk;
		} else {
			memcpy(g_ac97.play_buf, (const uint8_t *)buf + done,
			       chunk);
			samples = (uint32_t)(chunk / 2);
		}

		ac97_reset_stream();
		g_ac97.bd[0].addr = g_ac97.play_phys;
		g_ac97.bd[0].ctl_len = AC97_BD_IOC | samples;
		ac97_bm_writed(AC97_PO_BDBAR, VIRT_TO_PHY(g_ac97.bd));
		ac97_bm_writeb(AC97_PO_LVI, 0);
		ac97_bm_writeb(AC97_PO_CR, AC97_CR_RPBM);
		ret = ac97_wait_done();
		if (ret < 0)
			break;

		done += chunk;
	}

	mutex_unlock(&g_ac97.stream_lock);
	return done ? (ssize_t)done : ret;
}

static int ac97_sync(void *_dev)
{
	int ret;

	(void)_dev;
	if (!g_ac97.ready)
		return -ENODEV;
	mutex_lock(&g_ac97.stream_lock);
	ret = ac97_wait_done();
	mutex_unlock(&g_ac97.stream_lock);
	return ret;
}

static int ac97_reset(void *_dev)
{
	(void)_dev;
	if (!g_ac97.ready)
		return -ENODEV;
	mutex_lock(&g_ac97.stream_lock);
	ac97_reset_stream();
	mutex_unlock(&g_ac97.stream_lock);
	return 0;
}

static int ac97_set_rate(void *_dev, unsigned *rate)
{
	int ret;

	(void)_dev;
	if (!g_ac97.ready)
		return -ENODEV;
	mutex_lock(&g_ac97.stream_lock);
	ret = ac97_set_rate_value(rate);
	mutex_unlock(&g_ac97.stream_lock);
	return ret;
}

static int ac97_set_channels(void *_dev, unsigned *channels)
{
	(void)_dev;
	if (!g_ac97.ready)
		return -ENODEV;
	if (!channels)
		return -EINVAL;

	mutex_lock(&g_ac97.stream_lock);
	if (*channels != 1 && *channels != 2)
		*channels = 2;
	g_ac97.channels = *channels;
	mutex_unlock(&g_ac97.stream_lock);
	return 0;
}

static int ac97_set_format(void *_dev, unsigned *format)
{
	(void)_dev;
	if (!g_ac97.ready)
		return -ENODEV;
	if (!format)
		return -EINVAL;

	mutex_lock(&g_ac97.stream_lock);
	if (*format == AUDIO_FMT_U8 || *format == AUDIO_FMT_S16_LE)
		g_ac97.format = *format;
	*format = g_ac97.format;
	mutex_unlock(&g_ac97.stream_lock);
	return 0;
}

static unsigned ac97_block_size(void *_dev)
{
	(void)_dev;
	return AC97_PLAY_BYTES;
}

static int ac97_get_volume(void *_dev, unsigned control, unsigned *left,
			   unsigned *right)
{
	(void)_dev;
	if (!g_ac97.ready)
		return -ENODEV;
	return ac97_get_volume_values(control, left, right);
}

static int ac97_set_volume(void *_dev, unsigned control, unsigned *left,
			   unsigned *right)
{
	(void)_dev;
	if (!g_ac97.ready)
		return -ENODEV;
	return ac97_set_volume_values(control, left, right);
}

static int ac97_audio_init(uint32_t pci_device)
{
	uint32_t cmd;
	unsigned pages;
	unsigned rate;

	if (g_ac97.ready)
		return -EBUSY;

	memset(&g_ac97, 0, sizeof(g_ac97));
	mutex_init(&g_ac97.stream_lock);
	g_ac97.pci_dev = pci_device;
	g_ac97.nam = (uint16_t)(pci_read_field(pci_device, PCI_BAR0, 4) & ~3u);
	g_ac97.nabm = (uint16_t)(pci_read_field(pci_device, PCI_BAR1, 4) & ~3u);
	if (!g_ac97.nam || !g_ac97.nabm)
		return -ENODEV;

	cmd = pci_read_field(pci_device, PCI_COMMAND, 2);
	pci_write_field(pci_device, PCI_COMMAND, 2, cmd | 0x05);

	pages = (AC97_PLAY_BYTES + PAGE_SIZE - 1) / PAGE_SIZE;
	g_ac97.play_buf = (uint8_t *)vm_alloc_dma(pages);
	g_ac97.play_phys = VIRT_TO_PHY(g_ac97.play_buf);
	g_ac97.bd = (ac97_bd *)vm_alloc_dma(1);
	memset(g_ac97.bd, 0, PAGE_SIZE);

	g_ac97.channels = 2;
	g_ac97.format = AUDIO_FMT_S16_LE;
	g_ac97.master_left = 100;
	g_ac97.master_right = 100;
	g_ac97.pcm_left = 80;
	g_ac97.pcm_right = 80;
	g_ac97.speaker_left = 80;
	g_ac97.speaker_right = 80;

	ac97_mixer_write(AC97_NAM_RESET, 0);
	ac97_mixer_write(AC97_NAM_MASTER_VOL,
			 ac97_stereo_atten(g_ac97.master_left,
					   g_ac97.master_right));
	ac97_mixer_write(AC97_NAM_PCM_VOL,
			 ac97_stereo_atten(g_ac97.pcm_left, g_ac97.pcm_right));

	rate = 44100;
	ac97_set_rate_value(&rate);
	ac97_reset_stream();
	g_ac97.ready = 1;
	return 0;
}

static int ac97_probe_pci(uint32_t device, uint16_t v, uint16_t d,
			  const pci_device_id *id)
{
	(void)v;
	(void)d;
	(void)id;
	int error = ac97_audio_init(device);
	if (error)
		return error;
	audio_dev_register();
	return 0;
}

static const pci_device_id ac97_ids[] = {
	{ AC97_VENDOR_INTEL, AC97_DEVICE_ICH },
};

static driver_t ac97_driver = {
	.name = "intel-ich-ac97",
	.bus = DEVICE_BUS_PCI,
	.pci_ids = ac97_ids,
	.pci_id_count = sizeof(ac97_ids) / sizeof(ac97_ids[0]),
	.probe_pci = ac97_probe_pci,
};

DRIVER_REGISTER(ac97_driver);

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
#define OSS_MIXER_PRIVATE2 112
#define OSS_MIXER_PRIVATE3 113
#define OSS_MIXER_PRIVATE4 114
#define OSS_MIXER_PRIVATE5 115
#define OSS_MIXER_GETLEVELS 116
#define OSS_MIXER_SETLEVELS 117
#define OSS_GETVERSION 118
#define OSS_SOUND_VERSION 0x030802

#define OSS_MIXER_SUPPORTED                                 \
	((1u << OSS_MIXER_VOLUME) | (1u << OSS_MIXER_PCM) | \
	 (1u << OSS_MIXER_SPEAKER))

typedef struct {
	ac97_dev *dev;
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

static ac97_dev *oss_mixer_device(void)
{
	return (g_ac97.ready ? &g_ac97 : NULL);
}

static int oss_mixer_set_volume(unsigned nr, int *arg)
{
	ac97_dev *dev = oss_mixer_device();
	unsigned left;
	unsigned right;
	int ret;

	if (!arg)
		return -EINVAL;
	left = (unsigned)(*arg & 0xff);
	right = (unsigned)((*arg >> 8) & 0xff);
	ret = ac97_set_volume(dev, nr, &left, &right);
	if (ret < 0)
		return ret;
	*arg = oss_stereo_volume(left, right);
	return 0;
}

static int oss_mixer_get_volume(unsigned nr, int *arg)
{
	ac97_dev *dev = oss_mixer_device();
	unsigned left;
	unsigned right;
	int ret;

	if (!arg)
		return -EINVAL;
	ret = ac97_get_volume(dev, nr, &left, &right);
	if (ret < 0)
		return ret;
	*arg = oss_stereo_volume(left, right);
	return 0;
}

static int mixer_version(unsigned cmd __attribute__((unused)), void *buf)
{
	*(int *)buf = OSS_SOUND_VERSION;
	return 0;
}

static int mixer_get_info(unsigned cmd __attribute__((unused)), void *buf)
{
	oss_fill_mixer_info(buf, (cmd >> 16) & 0x3fff);
	return 0;
}

static int mixer_levels(unsigned cmd __attribute__((unused)), void *buf)
{
	oss_fill_mixer_levels(buf);
	return 0;
}

static int mixer_accept(unsigned cmd __attribute__((unused)),
			void *buf __attribute__((unused)))
{
	return 0;
}

static int mixer_zero(unsigned cmd __attribute__((unused)), void *buf)
{
	*(int *)buf = 0;
	return 0;
}

static int mixer_mask(unsigned cmd __attribute__((unused)), void *buf)
{
	*(int *)buf = OSS_MIXER_SUPPORTED;
	return 0;
}

static int oss_mixer_ioctl(file *fp, unsigned cmd, void *buf)
{
	static int (*const commands[256])(unsigned, void *) = {
		[OSS_GETVERSION] = mixer_version,
		[OSS_MIXER_INFO] = mixer_get_info,
		[OSS_MIXER_GETLEVELS] = mixer_levels,
		[OSS_MIXER_SETLEVELS] = mixer_accept,
		[OSS_MIXER_ACCESS] = mixer_zero,
		[OSS_MIXER_AGC] = mixer_zero,
		[OSS_MIXER_3DSE] = mixer_zero,
		[OSS_MIXER_PRIVATE1] = mixer_zero,
		[OSS_MIXER_PRIVATE2] = mixer_zero,
		[OSS_MIXER_PRIVATE3] = mixer_zero,
		[OSS_MIXER_PRIVATE4] = mixer_zero,
		[OSS_MIXER_PRIVATE5] = mixer_zero,
		[OSS_MIXER_DEVMASK] = mixer_mask,
		[OSS_MIXER_STEREODEVS] = mixer_mask,
		[OSS_MIXER_RECMASK] = mixer_zero,
		[OSS_MIXER_CAPS] = mixer_zero,
		[OSS_MIXER_RECSRC] = mixer_zero,
	};
	static int (*const volumes[2])(unsigned, int *) = {
		oss_mixer_get_volume,
		oss_mixer_set_volume,
	};
	unsigned nr = cmd & 0xff;
	(void)fp;
	if (!buf || ((cmd >> 8) & 0xff) != 'M')
		return -EINVAL;
	if (commands[nr])
		return commands[nr](cmd, buf);
	if (nr > 31 || !(OSS_MIXER_SUPPORTED & (1u << nr)))
		return -EINVAL;
	return volumes[(cmd >> 30) & 1](nr, buf);
}

static int mixer_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	mixer_file_ctx *ctx = node->i_private;

	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_rdev = ctx ? ctx->rdev : 0;
	s->st_blksize = PAGE_SIZE;
	s->st_atime = time_wall_sec();
	s->st_ctime = time_wall_sec();
	s->st_mtime = time_wall_sec();
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
	return ac97_write(ctx ? ctx->dev : NULL, buf, size);
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
	s->st_blksize = ctx ? ac97_block_size(ctx->dev) : PAGE_SIZE;
	s->st_atime = time_wall_sec();
	s->st_ctime = time_wall_sec();
	s->st_mtime = time_wall_sec();
	s->st_nlink = 1;
	return 0;
}

static int dsp_ioctl_sndctl_dsp_reset(void *context __attribute__((unused)),
				      unsigned cmd __attribute__((unused)),
				      void *buf __attribute__((unused)))
{
	file *fp = context;
	dsp_file_ctx *ctx = fp->f_inode->i_private;
	ac97_dev *dev = ctx ? ctx->dev : NULL;
	return ac97_reset(dev);
}

static int dsp_ioctl_sndctl_dsp_sync(void *context __attribute__((unused)),
				     unsigned cmd __attribute__((unused)),
				     void *buf __attribute__((unused)))
{
	file *fp = context;
	dsp_file_ctx *ctx = fp->f_inode->i_private;
	ac97_dev *dev = ctx ? ctx->dev : NULL;
	return ac97_sync(dev);
}

static int dsp_ioctl_sndctl_dsp_getblksize(void *context
					   __attribute__((unused)),
					   unsigned cmd __attribute__((unused)),
					   void *buf __attribute__((unused)))
{
	file *fp = context;
	dsp_file_ctx *ctx = fp->f_inode->i_private;
	ac97_dev *dev = ctx ? ctx->dev : NULL;
	int *arg = (int *)buf;
	if (arg)
		*arg = (int)ac97_block_size(dev);
	return 0;
}

static int dsp_ioctl_sndctl_dsp_getfmts(void *context __attribute__((unused)),
					unsigned cmd __attribute__((unused)),
					void *buf __attribute__((unused)))
{
	int *arg = (int *)buf;
	if (!arg)
		return -EINVAL;
	*arg = AUDIO_FMT_U8 | AUDIO_FMT_S16_LE;
	return 0;
}

static int dsp_ioctl_sndctl_dsp_speed(void *context __attribute__((unused)),
				      unsigned cmd __attribute__((unused)),
				      void *buf __attribute__((unused)))
{
	file *fp = context;
	dsp_file_ctx *ctx = fp->f_inode->i_private;
	ac97_dev *dev = ctx ? ctx->dev : NULL;
	int *arg = (int *)buf;
	unsigned val;
	int ret;
	if (!arg)
		return -EINVAL;
	val = (unsigned)*arg;
	ret = ac97_set_rate(dev, &val);
	*arg = (int)val;
	return ret;
}

static int dsp_ioctl_sndctl_dsp_stereo(void *context __attribute__((unused)),
				       unsigned cmd __attribute__((unused)),
				       void *buf __attribute__((unused)))
{
	file *fp = context;
	dsp_file_ctx *ctx = fp->f_inode->i_private;
	ac97_dev *dev = ctx ? ctx->dev : NULL;
	int *arg = (int *)buf;
	unsigned val;
	int ret;
	if (!arg)
		return -EINVAL;
	val = *arg ? 2u : 1u;
	ret = ac97_set_channels(dev, &val);
	*arg = val == 2;
	return ret;
}

static int dsp_ioctl_sndctl_dsp_channels(void *context __attribute__((unused)),
					 unsigned cmd __attribute__((unused)),
					 void *buf __attribute__((unused)))
{
	file *fp = context;
	dsp_file_ctx *ctx = fp->f_inode->i_private;
	ac97_dev *dev = ctx ? ctx->dev : NULL;
	int *arg = (int *)buf;
	unsigned val;
	int ret;
	if (!arg)
		return -EINVAL;
	val = (unsigned)*arg;
	ret = ac97_set_channels(dev, &val);
	*arg = (int)val;
	return ret;
}

static int dsp_ioctl_sndctl_dsp_setfmt(void *context __attribute__((unused)),
				       unsigned cmd __attribute__((unused)),
				       void *buf __attribute__((unused)))
{
	file *fp = context;
	dsp_file_ctx *ctx = fp->f_inode->i_private;
	ac97_dev *dev = ctx ? ctx->dev : NULL;
	int *arg = (int *)buf;
	unsigned val;
	int ret;
	if (!arg)
		return -EINVAL;
	val = (unsigned)*arg;
	ret = ac97_set_format(dev, &val);
	*arg = (int)val;
	return ret;
}

static const command_operation dsp_commands[256] = {
	[SNDCTL_DSP_RESET & 255] = { SNDCTL_DSP_RESET,
				     dsp_ioctl_sndctl_dsp_reset },
	[SNDCTL_DSP_SYNC & 255] = { SNDCTL_DSP_SYNC,
				    dsp_ioctl_sndctl_dsp_sync },
	[SNDCTL_DSP_GETBLKSIZE & 255] = { SNDCTL_DSP_GETBLKSIZE,
					  dsp_ioctl_sndctl_dsp_getblksize },
	[SNDCTL_DSP_GETFMTS & 255] = { SNDCTL_DSP_GETFMTS,
				       dsp_ioctl_sndctl_dsp_getfmts },
	[SNDCTL_DSP_SPEED & 255] = { SNDCTL_DSP_SPEED,
				     dsp_ioctl_sndctl_dsp_speed },
	[SNDCTL_DSP_STEREO & 255] = { SNDCTL_DSP_STEREO,
				      dsp_ioctl_sndctl_dsp_stereo },
	[SNDCTL_DSP_CHANNELS & 255] = { SNDCTL_DSP_CHANNELS,
					dsp_ioctl_sndctl_dsp_channels },
	[SNDCTL_DSP_SETFMT & 255] = { SNDCTL_DSP_SETFMT,
				      dsp_ioctl_sndctl_dsp_setfmt },
};

static const command_operation *const dsp_command_groups[256] = {
	[(SNDCTL_DSP_RESET >> 8) & 255] = dsp_commands,
};

static int dsp_ioctl(file *fp, unsigned cmd, void *buf)
{
	return command_dispatch(dsp_command_groups, fp, cmd, buf, -EINVAL);
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
	ac97_dev *dev = (g_ac97.ready ? &g_ac97 : NULL);
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

static void audio_dev_register(void)
{
	vfs_entry_device(devfs_entries(), "/mixer", S_IFCHR | 0666,
			 MKDEV(SOUND_MAJOR, SOUND_MIXER_MINOR), "sound",
			 mixer_cdev_open);
	vfs_entry_device(devfs_entries(), "/dsp", S_IFCHR | 0666,
			 MKDEV(SOUND_MAJOR, SOUND_DSP_MINOR), "sound",
			 dsp_cdev_open);
	vfs_entry_device(devfs_entries(), "/audio", S_IFCHR | 0666,
			 MKDEV(SOUND_MAJOR, SOUND_AUDIO_MINOR), "sound",
			 dsp_cdev_open);
}
