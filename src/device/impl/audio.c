#include <errno.h>
#include <device/audio.h>
#include <lib/klib.h>

static audio_dev audio_devices[MAX_AUDIO_DEV];
static unsigned audio_device_count;

audio_dev *audio_register_device(audio_dev *dev)
{
	if (!dev || !dev->ops || !dev->ops->init ||
	    audio_device_count == MAX_AUDIO_DEV)
		return NULL;

	if (dev->ops->init(dev) != 0)
		return NULL;

	audio_dev *registered = &audio_devices[audio_device_count++];
	*registered = *dev;
	if (registered->ops->on_register)
		registered->ops->on_register(registered);
	return registered;
}

audio_dev *audio_getdev(int index)
{
	if (index < 0 || index >= MAX_AUDIO_DEV)
		return NULL;
	if (audio_devices[index].ven == 0 && audio_devices[index].dev == 0)
		return NULL;
	return &audio_devices[index];
}

audio_dev *audio_default_device(void)
{
	return audio_getdev(0);
}

ssize_t audio_write(audio_dev *dev, const void *buf, size_t size)
{
	if (!dev || !dev->ops || !dev->ops->write)
		return -ENODEV;
	return dev->ops->write(dev, buf, size);
}

int audio_sync(audio_dev *dev)
{
	if (!dev || !dev->ops || !dev->ops->sync)
		return -ENODEV;
	return dev->ops->sync(dev);
}

int audio_reset(audio_dev *dev)
{
	if (!dev || !dev->ops || !dev->ops->reset)
		return -ENODEV;
	return dev->ops->reset(dev);
}

int audio_set_rate(audio_dev *dev, unsigned *rate)
{
	if (!dev || !dev->ops || !dev->ops->set_rate)
		return -ENODEV;
	return dev->ops->set_rate(dev, rate);
}

int audio_set_channels(audio_dev *dev, unsigned *channels)
{
	if (!dev || !dev->ops || !dev->ops->set_channels)
		return -ENODEV;
	return dev->ops->set_channels(dev, channels);
}

int audio_set_format(audio_dev *dev, unsigned *format)
{
	if (!dev || !dev->ops || !dev->ops->set_format)
		return -ENODEV;
	return dev->ops->set_format(dev, format);
}

int audio_get_volume(audio_dev *dev, unsigned control, unsigned *left,
		     unsigned *right)
{
	if (!dev || !dev->ops || !dev->ops->get_volume)
		return -ENODEV;
	return dev->ops->get_volume(dev, control, left, right);
}

int audio_set_volume(audio_dev *dev, unsigned control, unsigned *left,
		     unsigned *right)
{
	if (!dev || !dev->ops || !dev->ops->set_volume)
		return -ENODEV;
	return dev->ops->set_volume(dev, control, left, right);
}

unsigned audio_block_size(audio_dev *dev)
{
	if (!dev || !dev->ops || !dev->ops->block_size)
		return 0;
	return dev->ops->block_size(dev);
}
