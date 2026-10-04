#include <dev/tty.h>
#include <fs/ioctl.h>
#include <lib/klib.h>
#include <errno.h>

static int font_accept(struct console_font_op *operation)
{
	(void)operation;
	return 0;
}

static int font_get(struct console_font_op *operation)
{
	operation->width = 8;
	operation->height = 16;
	operation->charcount = 256;
	if (operation->data)
		memset(operation->data, 0,
		       operation->charcount * ((operation->width + 7) / 8) *
			       operation->height);
	return 0;
}

int tty_font_ioctl(struct console_font_op *operation)
{
	static int (*const handlers[])(struct console_font_op *) = {
		[KD_FONT_OP_SET] = font_accept,
		[KD_FONT_OP_GET] = font_get,
		[KD_FONT_OP_SET_DEFAULT] = font_accept,
		[KD_FONT_OP_COPY] = font_accept,
	};
	if (operation->op >= sizeof(handlers) / sizeof(handlers[0]))
		return -EINVAL;
	return handlers[operation->op](operation);
}
