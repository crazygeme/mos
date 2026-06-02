#include <errno.h>
#include <hw/audio.h>
#include <hw/driver.h>
#include <hw/pci.h>
#include <lib/klib.h>
#include <lib/port.h>
#include <macro.h>
#include <mm/mm.h>
#include <stdint.h>
#include <unistd.h>

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
		HLT();
	}

	ac97_bm_writeb(AC97_PO_CR, AC97_CR_RR);
	for (i = 0; i < 10000; i++) {
		if (!(ac97_bm_readb(AC97_PO_CR) & AC97_CR_RR))
			break;
		HLT();
	}
	ac97_bm_writew(AC97_PO_SR, AC97_SR_BCIS | AC97_SR_LVBCI |
					   AC97_SR_FIFOE | AC97_SR_CELV);
}

static void ac97_wait_done(void)
{
	unsigned i;

	for (i = 0; i < 2000000; i++) {
		uint16_t sr = ac97_bm_readw(AC97_PO_SR);
		if (sr & (AC97_SR_BCIS | AC97_SR_DCH | AC97_SR_FIFOE))
			break;
		HLT();
	}
	ac97_bm_writeb(AC97_PO_CR, 0);
	ac97_bm_writew(AC97_PO_SR, AC97_SR_BCIS | AC97_SR_LVBCI |
					   AC97_SR_FIFOE | AC97_SR_CELV);
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

static int ac97_set_volume_values(unsigned control, unsigned *left,
				  unsigned *right)
{
	unsigned l;
	unsigned r;

	if (!left || !right)
		return -EINVAL;

	l = ac97_clamp_volume(*left);
	r = ac97_clamp_volume(*right);

	switch (control) {
	case AUDIO_MIXER_VOLUME:
		g_ac97.master_left = l;
		g_ac97.master_right = r;
		ac97_mixer_write(AC97_NAM_MASTER_VOL, ac97_stereo_atten(l, r));
		break;
	case AUDIO_MIXER_PCM:
		g_ac97.pcm_left = l;
		g_ac97.pcm_right = r;
		ac97_mixer_write(AC97_NAM_PCM_VOL, ac97_stereo_atten(l, r));
		break;
	case AUDIO_MIXER_SPEAKER:
		g_ac97.speaker_left = l;
		g_ac97.speaker_right = r;
		break;
	default:
		return -EINVAL;
	}

	*left = l;
	*right = r;
	return 0;
}

static int ac97_get_volume_values(unsigned control, unsigned *left,
				  unsigned *right)
{
	if (!left || !right)
		return -EINVAL;

	switch (control) {
	case AUDIO_MIXER_VOLUME:
		*left = g_ac97.master_left;
		*right = g_ac97.master_right;
		return 0;
	case AUDIO_MIXER_PCM:
		*left = g_ac97.pcm_left;
		*right = g_ac97.pcm_right;
		return 0;
	case AUDIO_MIXER_SPEAKER:
		*left = g_ac97.speaker_left;
		*right = g_ac97.speaker_right;
		return 0;
	default:
		return -EINVAL;
	}
}

static ssize_t ac97_write(void *_dev, const void *buf, size_t size)
{
	size_t done = 0;
	unsigned frame_bytes;

	(void)_dev;

	if (!g_ac97.ready)
		return -ENODEV;

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
		ac97_wait_done();

		done += chunk;
	}

	return (ssize_t)done;
}

static int ac97_sync(void *_dev)
{
	(void)_dev;
	if (!g_ac97.ready)
		return -ENODEV;
	ac97_wait_done();
	return 0;
}

static int ac97_reset(void *_dev)
{
	(void)_dev;
	if (!g_ac97.ready)
		return -ENODEV;
	ac97_reset_stream();
	return 0;
}

static int ac97_set_rate(void *_dev, unsigned *rate)
{
	(void)_dev;
	if (!g_ac97.ready)
		return -ENODEV;
	return ac97_set_rate_value(rate);
}

static int ac97_set_channels(void *_dev, unsigned *channels)
{
	(void)_dev;
	if (!g_ac97.ready)
		return -ENODEV;
	if (!channels)
		return -EINVAL;

	if (*channels != 1 && *channels != 2)
		*channels = 2;
	g_ac97.channels = *channels;
	return 0;
}

static int ac97_set_format(void *_dev, unsigned *format)
{
	(void)_dev;
	if (!g_ac97.ready)
		return -ENODEV;
	if (!format)
		return -EINVAL;

	if (*format == AUDIO_FMT_U8 || *format == AUDIO_FMT_S16_LE)
		g_ac97.format = *format;
	*format = g_ac97.format;
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

static int ac97_audio_init(void *_dev)
{
	audio_dev *dev = (audio_dev *)_dev;
	uint32_t cmd;
	unsigned pages;
	unsigned rate;

	if (!dev)
		return -ENODEV;
	if (g_ac97.ready) {
		dev->ctx = &g_ac97;
		return 0;
	}

	memset(&g_ac97, 0, sizeof(g_ac97));
	g_ac97.pci_dev = dev->pci_dev;
	g_ac97.nam =
		(uint16_t)(pci_read_field(dev->pci_dev, PCI_BAR0, 4) & ~3u);
	g_ac97.nabm =
		(uint16_t)(pci_read_field(dev->pci_dev, PCI_BAR1, 4) & ~3u);
	if (!g_ac97.nam || !g_ac97.nabm)
		return -ENODEV;

	cmd = pci_read_field(dev->pci_dev, PCI_COMMAND, 2);
	pci_write_field(dev->pci_dev, PCI_COMMAND, 2, cmd | 0x05);

	pages = (AC97_PLAY_BYTES + PAGE_SIZE - 1) / PAGE_SIZE;
	g_ac97.play_buf = (uint8_t *)vm_alloc(pages);
	g_ac97.play_phys = VIRT_TO_PHY(g_ac97.play_buf);
	g_ac97.bd = (ac97_bd *)vm_alloc(1);
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
	dev->ctx = &g_ac97;

	printk("audio: Intel ICH AC97 initialized (NAM=0x%x NABM=0x%x)\n",
	       g_ac97.nam, g_ac97.nabm);
	return 0;
}

static const audio_ops ac97_audio_ops = {
	.init = ac97_audio_init,
	.on_register = NULL,
	.write = ac97_write,
	.sync = ac97_sync,
	.reset = ac97_reset,
	.set_rate = ac97_set_rate,
	.set_channels = ac97_set_channels,
	.set_format = ac97_set_format,
	.get_volume = ac97_get_volume,
	.set_volume = ac97_set_volume,
	.block_size = ac97_block_size,
};

static int ac97_probe_pci(uint32_t device, uint16_t v, uint16_t d,
			  const hw_pci_id *id)
{
	audio_dev dev;

	(void)id;

	memset(&dev, 0, sizeof(dev));
	dev.pci_dev = device;
	dev.ven = v;
	dev.dev = d;
	dev.ops = &ac97_audio_ops;

	return audio_register_device(&dev) ? 0 : -ENODEV;
}

static const hw_pci_id ac97_ids[] = {
	{ AC97_VENDOR_INTEL, AC97_DEVICE_ICH },
};

static hw_driver ac97_driver = {
	.name = "intel-ich-ac97",
	.type = HW_TYPE_AUDIO,
	.bus = HW_BUS_PCI,
	.pci_ids = ac97_ids,
	.pci_id_count = sizeof(ac97_ids) / sizeof(ac97_ids[0]),
	.ops = NULL,
	.probe_pci = ac97_probe_pci,
};

static void ac97_register(void)
{
	hw_driver_register(&ac97_driver);
}

KERNEL_INIT(5, ac97_register);
