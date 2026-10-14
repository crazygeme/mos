#include <fs/mount.h>
#include <ps/namespace.h>
#include <macro.h>

static super_block *mqueue_get_sb(const char *dev, const char *target,
				  int flags, void *data)
{
	return ps_namespace_mqueue_root();
}

static fs_type mqueue_type = { .name = "mqueue", .get_sb = mqueue_get_sb };
static void mqueue_register(void)
{
	fs_register_type(&mqueue_type);
}
KERNEL_INIT(5, mqueue_register);
