#include <device/chardev.h>
#include <device/blockdev.h>
/*
 * /proc/devices — registered character and block device majors.
 *
 * Linux reports one line per registered major, grouped by device class.  MOS
 * registers handlers by minor range, so the iterator below collapses duplicate
 * ranges that share the same class and major.
 */
#include <fs/entries.h>
#include "common.h"

#define PROC_DEVICES_MAX 64

typedef struct {
	unsigned major;
	const char *name;
} proc_device_entry;

typedef struct {
	unsigned mode_type;
	proc_device_entry entries[PROC_DEVICES_MAX];
	unsigned count;
} proc_devices_ctx;

static void collect_major(unsigned mode_type, unsigned major, const char *name,
			  void *data)
{
	proc_devices_ctx *ctx = data;
	unsigned i, pos;

	if (mode_type != ctx->mode_type || ctx->count >= PROC_DEVICES_MAX)
		return;

	for (i = 0; i < ctx->count; i++) {
		if (ctx->entries[i].major == major)
			return;
	}

	pos = ctx->count++;
	while (pos > 0 && ctx->entries[pos - 1].major > major) {
		ctx->entries[pos] = ctx->entries[pos - 1];
		pos--;
	}

	ctx->entries[pos].major = major;
	ctx->entries[pos].name = name ? name : "unknown";
}

static void print_group(proc_buf_t *pb, unsigned mode_type, const char *title)
{
	proc_devices_ctx *ctx = zalloc(sizeof(*ctx));
	unsigned i;

	if (!ctx) return;
	ctx->mode_type = mode_type;
	if (mode_type == S_IFCHR)
		chardev_for_each_class(collect_major, ctx);
	else
		blockdev_for_each_class(collect_major, ctx);

	proc_buf_printf(pb, "%s:\n", title);
	for (i = 0; i < ctx->count; i++)
		proc_buf_printf(pb, "%3u %s\n", ctx->entries[i].major,
				ctx->entries[i].name);
	free(ctx);
}

static void fill(proc_buf_t *pb)
{
	print_group(pb, S_IFCHR, "Character devices");
	proc_buf_printf(pb, "\n");
	print_group(pb, S_IFBLK, "Block devices");
}

DEFINE_PROC_FILE(devices, fill);
