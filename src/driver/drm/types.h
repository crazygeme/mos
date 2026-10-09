#ifndef MOS_DRM_TYPES_H
#define MOS_DRM_TYPES_H

#include <stdint.h>
#include <stddef.h>
typedef int8_t __s8;
typedef uint8_t __u8;
typedef int16_t __s16;
typedef uint16_t __u16;
typedef int32_t __s32;
typedef uint32_t __u32;
typedef int64_t __s64;
typedef uint64_t __u64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef size_t __kernel_size_t;
#define __user
#define __aligned_u64 __u64 __attribute__((aligned(8)))
#define _IOC(dir, type, nr, size) \
	(((dir) << 30) | ((size) << 16) | ((type) << 8) | (nr))
#define _IO(type, nr) _IOC(0U, type, nr, 0U)
#define _IOR(type, nr, arg) _IOC(2U, type, nr, sizeof(arg))
#define _IOW(type, nr, arg) _IOC(1U, type, nr, sizeof(arg))
#define _IOWR(type, nr, arg) _IOC(3U, type, nr, sizeof(arg))
#endif
