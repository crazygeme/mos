#!/bin/sh
# Validate VirtIO-GPU modes in a MOS guest; --modeset requires inactive Xorg.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-virtgpu_modes.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC O_CLOEXEC
#define SOCK_NONBLOCK O_NONBLOCK
#endif
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC 1030
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0x40000000
#endif
#define CHECK(x)                                                            \
	do {                                                                \
		if (!(x)) {                                                 \
			fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, \
				__LINE__, #x, errno);                       \
			exit(1);                                            \
		}                                                           \
	} while (0)

#include <math.h>
struct mode {
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew, vdisplay,
		vsync_start, vsync_end, vtotal, vscan;
	uint32_t vrefresh, flags, type;
	char name[32];
};
struct resources {
	uint64_t fbs, crtcs, connectors, encoders;
	uint32_t count_fbs, count_crtcs, count_connectors, count_encoders,
		min_width, max_width, min_height, max_height;
};
struct connector {
	uint64_t encoders, modes, props, values;
	uint32_t count_modes, count_props, count_encoders, encoder_id, id, type,
		type_id, connection, mm_width, mm_height, subpixel, pad;
};
struct crtc {
	uint64_t connectors;
	uint32_t count, id, fb, x, y, gamma, valid;
	struct mode mode;
};
struct dumb {
	uint32_t height, width, bpp, flags, handle, pitch;
	uint64_t size;
};
struct fb {
	uint32_t id, width, height, pitch, bpp, depth, handle;
};
static int request(int fd, unsigned nr, void *p, size_t n)
{
	return ioctl(fd,
		     (unsigned long)('d' << 8) | nr |
			     (p ? (3U << 30) | (n << 16) : 0),
		     p);
}
static void call(int fd, unsigned nr, void *p, size_t n)
{
	CHECK(!request(fd, nr, p, n));
}
static struct crtc current(int fd, unsigned id)
{
	struct crtc c;
	memset(&c, 0, sizeof(c));
	c.id = id;
	call(fd, 0xa1, &c, sizeof(c));
	return c;
}
static int same(struct crtc *a, struct crtc *b)
{
	return a->fb == b->fb && a->x == b->x && a->y == b->y &&
	       a->valid == b->valid &&
	       !memcmp(&a->mode, &b->mode, sizeof(a->mode));
}
static void modeset(int fd, unsigned connector, unsigned id, struct mode *m,
		    unsigned count)
{
	struct dumb d;
	struct fb f;
	struct crtc c, actual, bad, baseline;
	unsigned widths[] = { 640, 1280, 800 }, heights[] = { 480, 720, 600 },
		 i, j;
	call(fd, 0x1e, 0, 0);
	actual = current(fd, id);
	CHECK(!actual.valid);
	memset(&d, 0, sizeof(d));
	d.width = 1600;
	d.height = 1200;
	d.bpp = 32;
	call(fd, 0xb2, &d, sizeof(d));
	memset(&f, 0, sizeof(f));
	f.width = d.width;
	f.height = d.height;
	f.pitch = d.pitch;
	f.bpp = 32;
	f.depth = 24;
	f.handle = d.handle;
	call(fd, 0xae, &f, sizeof(f));
	memset(&c, 0, sizeof(c));
	c.connectors = (uintptr_t)&connector;
	c.count = 1;
	c.id = id;
	c.fb = f.id;
	c.x = 16;
	c.y = 24;
	c.valid = 1;
	for (i = 0; i < 3; i++) {
		for (j = 0; j < count; j++)
			if (m[j].hdisplay == widths[i] &&
			    m[j].vdisplay == heights[i])
				break;
		CHECK(j < count);
		c.mode = m[j];
		call(fd, 0xa2, &c, sizeof(c));
		actual = current(fd, id);
		CHECK(same(&actual, &c));
	}
	memset(&c.mode, 0, sizeof(c.mode));
	c.mode.clock = 52000;
	c.mode.hdisplay = 997;
	c.mode.hsync_start = 1045;
	c.mode.hsync_end = 1077;
	c.mode.htotal = 1157;
	c.mode.vdisplay = 701;
	c.mode.vsync_start = 704;
	c.mode.vsync_end = 709;
	c.mode.vtotal = 761;
	c.mode.vrefresh = 59;
	c.mode.flags = 5;
	strcpy(c.mode.name, "997x701");
	call(fd, 0xa2, &c, sizeof(c));
	actual = current(fd, id);
	CHECK(same(&actual, &c));
	baseline = actual;
	for (i = 0; i < 9; i++) {
		bad = c;
		switch (i) {
		case 0:
			bad.x = UINT32_MAX;
			break;
		case 1:
			bad.y = 1200;
			break;
		case 2:
			bad.count = 0;
			break;
		case 3:
			bad.fb = 0;
			break;
		case 4:
			bad.mode.hdisplay = 0;
			break;
		case 5:
			bad.mode.hdisplay = 4097;
			break;
		case 6:
			bad.mode.clock = 0;
			break;
		case 7:
			bad.mode.flags = 16;
			break;
		case 8:
			bad.mode.hsync_end = 1;
			break;
		}
		CHECK(request(fd, 0xa2, &bad, sizeof(bad)) == -1 &&
		      errno == EINVAL);
		actual = current(fd, id);
		CHECK(same(&actual, &baseline));
	}
	memset(&c, 0, sizeof(c));
	c.id = id;
	call(fd, 0xa2, &c, sizeof(c));
	actual = current(fd, id);
	CHECK(same(&actual, &c));
	call(fd, 0xaf, &f.id, sizeof(f.id));
	call(fd, 0xb4, &d.handle, sizeof(d.handle));
	call(fd, 0x1f, 0, 0);
}
int main(int argc, char **argv)
{
	const char *device = "/dev/dri/card0", *preferred = 0;
	int do_modeset = 0, fd, i, j, npreferred = 0, required[4] = { 0 };
	unsigned connector_id = 0, crtc_id = 0;
	struct resources r;
	struct connector c, shortc;
	struct mode sentinel, *m;
	char size[32];
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc)
			device = argv[++i];
		else if (!strcmp(argv[i], "--expect-preferred") && i + 1 < argc)
			preferred = argv[++i];
		else if (!strcmp(argv[i], "--modeset"))
			do_modeset = 1;
		else
			CHECK(0);
	}
	CHECK(sizeof(struct mode) == 68 && sizeof(r) == 64 && sizeof(c) == 80 &&
	      sizeof(struct crtc) == 104 && sizeof(struct dumb) == 32 &&
	      sizeof(struct fb) == 28);
	fd = open(device, O_RDWR);
	if (fd < 0 && errno == ENOENT && argc == 1) {
		puts("SKIP: VirtIO-GPU mode checks require a VirtIO-GPU device");
		return 0;
	}
	CHECK(fd >= 0);
	memset(&r, 0, sizeof(r));
	call(fd, 0xa0, &r, sizeof(r));
	CHECK(r.count_connectors == 1 && r.count_crtcs == 1);
	r.connectors = (uintptr_t)&connector_id;
	r.crtcs = (uintptr_t)&crtc_id;
	r.count_encoders = r.count_fbs = 0;
	call(fd, 0xa0, &r, sizeof(r));
	memset(&c, 0, sizeof(c));
	c.id = connector_id;
	call(fd, 0xa7, &c, sizeof(c));
	CHECK(c.count_modes > 1);
	memset(&sentinel, 0xa5, sizeof(sentinel));
	memset(&shortc, 0, sizeof(shortc));
	shortc.id = connector_id;
	shortc.count_modes = 1;
	shortc.modes = (uintptr_t)&sentinel;
	call(fd, 0xa7, &shortc, sizeof(shortc));
	for (i = 0; i < sizeof(sentinel); i++)
		CHECK(((unsigned char *)&sentinel)[i] == 0xa5);
	CHECK((m = calloc(c.count_modes, sizeof(*m))));
	c.modes = (uintptr_t)m;
	c.count_encoders = 0;
	call(fd, 0xa7, &c, sizeof(c));
	for (i = 0; i < c.count_modes; i++) {
		struct mode *v = &m[i];
		for (j = 0; j < i; j++)
			CHECK(v->hdisplay != m[j].hdisplay ||
			      v->vdisplay != m[j].vdisplay ||
			      v->vrefresh != m[j].vrefresh);
		if (v->type & 8) {
			npreferred++;
			if (preferred) {
				snprintf(size, sizeof(size), "%ux%u",
					 v->hdisplay, v->vdisplay);
				CHECK(!strcmp(size, preferred));
			}
		}
		if (v->hdisplay == 640 && v->vdisplay == 480 &&
		    v->vrefresh == 60)
			required[0] = 1;
		if (v->hdisplay == 1920 && v->vdisplay == 1080 &&
		    v->vrefresh == 120)
			required[1] = 1;
		if (v->hdisplay == 2560 && v->vdisplay == 1440 &&
		    v->vrefresh == 120)
			required[2] = 1;
		if (v->hdisplay == 3840 && v->vdisplay == 2160 &&
		    v->vrefresh == 60)
			required[3] = 1;
		CHECK(v->hdisplay > 0 && v->hdisplay < v->hsync_start &&
		      v->hsync_start < v->hsync_end &&
		      v->hsync_end <= v->htotal);
		CHECK(v->vdisplay > 0 && v->vdisplay < v->vsync_start &&
		      v->vsync_start < v->vsync_end &&
		      v->vsync_end <= v->vtotal);
		CHECK(fabs((double)v->clock * 1000 / v->htotal / v->vtotal -
			   v->vrefresh) < 0.01);
	}
	CHECK(npreferred == 1 && required[0] && required[1] && required[2] &&
	      required[3]);
	if (do_modeset)
		modeset(fd, connector_id, crtc_id, m, c.count_modes);
	free(m);
	close(fd);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
