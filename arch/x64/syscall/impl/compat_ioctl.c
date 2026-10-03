/* Socket ioctl wire layouts for IA-32 processes on the AMD64 kernel. */
#include <arch/abi/i386/compat.h>
#include <net/socket.h>
#include <fs/fs.h>
#include <lib/klib.h>
#include <errno.h>

int sys_ioctl(int, int, char *);

struct ifconf32 {
	int32_t len;
	uint32_t buffer;
};

struct ifreq32 {
	char name[IFNAMSIZ];
	unsigned char value[16];
};

_Static_assert(sizeof(struct ifconf32) == 8, "IA-32 ifconf layout");
_Static_assert(sizeof(struct ifreq32) == 32, "IA-32 ifreq layout");
_Static_assert(sizeof(struct ifconf) == 16, "AMD64 ifconf layout");
_Static_assert(sizeof(struct ifreq) == 40, "AMD64 ifreq layout");

int compat_ioctl(int fd, int request, char *arg)
{
	if (request != SIOCGIFCONF)
		return sys_ioctl(fd, request, arg);
	if (!arg)
		return -EFAULT;

	struct ifconf32 *wire = (void *)arg;
	struct ifconf native = { 0 };
	/* Query first to bound the allocation by the available interfaces,
	 * rather than by an arbitrary userspace buffer length. */
	int ret = fs_ioctl(fd, request, &native);
	if (ret < 0)
		return ret;
	int count = native.ifc_len / (int)sizeof(struct ifreq);
	if (!wire->buffer) {
		wire->len = count * sizeof(struct ifreq32);
		return 0;
	}
	if (wire->len < 0)
		return -EINVAL;
	int capacity = wire->len / (int)sizeof(struct ifreq32);
	if (count > capacity)
		count = capacity;
	if (!count) {
		wire->len = 0;
		return 0;
	}
	native.ifc_len = count * sizeof(struct ifreq);
	native.ifc_req = kmalloc(native.ifc_len);
	if (!native.ifc_req)
		return -ENOMEM;
	ret = sys_ioctl(fd, request, (void *)&native);
	if (!ret) {
		struct ifreq32 *out = (void *)(uintptr_t)wire->buffer;
		count = native.ifc_len / (int)sizeof(struct ifreq);
		for (int i = 0; i < count; i++)
			/* SIOCGIFCONF returns only the name and sockaddr. */
			memcpy(&out[i], &native.ifc_req[i], sizeof(*out));
		wire->len = count * sizeof(struct ifreq32);
	}
	kfree(native.ifc_req);
	return ret;
}
