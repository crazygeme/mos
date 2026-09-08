#ifndef _HW_AUDIO_H
#define _HW_AUDIO_H

#include <fs/fs.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_AUDIO_DEV 4

#define AUDIO_FMT_U8 0x00000008
#define AUDIO_FMT_S16_LE 0x00000010

#define AUDIO_MIXER_VOLUME 0
#define AUDIO_MIXER_PCM 4
#define AUDIO_MIXER_SPEAKER 5

struct _audio_dev;

typedef struct {
	int (*init)(void *dev);
	void (*on_register)(struct _audio_dev *permanent);
	ssize_t (*write)(void *dev, const void *buf, size_t size);
	int (*sync)(void *dev);
	int (*reset)(void *dev);
	int (*set_rate)(void *dev, unsigned *rate);
	int (*set_channels)(void *dev, unsigned *channels);
	int (*set_format)(void *dev, unsigned *format);
	int (*get_volume)(void *dev, unsigned control, unsigned *left,
			  unsigned *right);
	int (*set_volume)(void *dev, unsigned control, unsigned *left,
			  unsigned *right);
	unsigned (*block_size)(void *dev);
} audio_ops;

typedef struct _audio_dev {
	uint32_t pci_dev;
	uint16_t ven;
	uint16_t dev;
	const audio_ops *ops;
	void *ctx;
} audio_dev;

audio_dev *audio_register_device(audio_dev *dev);
audio_dev *audio_getdev(int index);
audio_dev *audio_default_device(void);
ssize_t audio_write(audio_dev *dev, const void *buf, size_t size);
int audio_sync(audio_dev *dev);
int audio_reset(audio_dev *dev);
int audio_set_rate(audio_dev *dev, unsigned *rate);
int audio_set_channels(audio_dev *dev, unsigned *channels);
int audio_set_format(audio_dev *dev, unsigned *format);
int audio_get_volume(audio_dev *dev, unsigned control, unsigned *left,
		     unsigned *right);
int audio_set_volume(audio_dev *dev, unsigned control, unsigned *left,
		     unsigned *right);
unsigned audio_block_size(audio_dev *dev);

#endif
