#ifndef MOS_DRIVER_VIRTIO_GPU_H
#define MOS_DRIVER_VIRTIO_GPU_H

#include <fs/fs.h>
int gpu_device_address(unsigned *address);
file *gpu_open(super_block *sb, unsigned rdev, int flags);
#endif
