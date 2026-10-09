#include <device/framebuffer.h>
#include <lib/klib.h>

static const framebuffer_ops *active_fb_drv;
int framebuffer_available(void)
{
	return !active_fb_drv;
}
void framebuffer_register(const framebuffer_ops *ops)
{
	if (!active_fb_drv && ops && ops->surface)
		active_fb_drv = ops;
}
void framebuffer_get_surface(framebuffer_surface *surface)
{
	memset(surface, 0, sizeof(*surface));
	if (active_fb_drv)
		active_fb_drv->surface(surface);
}
void framebuffer_sync_mode(void)
{
	if (active_fb_drv && active_fb_drv->sync_mode)
		active_fb_drv->sync_mode();
}
void framebuffer_damage(unsigned begin, unsigned end)
{
	if (active_fb_drv && active_fb_drv->damage && begin < end)
		active_fb_drv->damage(begin, end);
}
int framebuffer_requires_refresh(void)
{
	return active_fb_drv && active_fb_drv->refresh;
}

void framebuffer_refresh(void)
{
	if (active_fb_drv && active_fb_drv->refresh)
		active_fb_drv->refresh();
}

void framebuffer_get_phys_window(unsigned *phys, unsigned *size)
{
	if (phys)
		*phys = 0;
	if (size)
		*size = 0;
	if (active_fb_drv && active_fb_drv->get_phys_window)
		active_fb_drv->get_phys_window(phys, size);
}

void framebuffer_present(void)
{
	if (active_fb_drv && active_fb_drv->present)
		active_fb_drv->present();
}

unsigned framebuffer_snapshot_size(void)
{
	if (active_fb_drv && active_fb_drv->snapshot_size)
		return active_fb_drv->snapshot_size();
	return 0;
}

void framebuffer_snapshot_save(void *dst, unsigned size)
{
	if (active_fb_drv && active_fb_drv->snapshot_save)
		active_fb_drv->snapshot_save(dst, size);
}

void framebuffer_snapshot_restore(const void *src, unsigned size)
{
	if (active_fb_drv && active_fb_drv->snapshot_restore)
		active_fb_drv->snapshot_restore(src, size);
}

static int surface_rect(const framebuffer_surface *surface, unsigned x,
			unsigned y, unsigned width, unsigned height)
{
	return surface->pixels && width && height && x < surface->width &&
	       y < surface->height && width <= surface->width - x &&
	       height <= surface->height - y &&
	       surface->width <= ~0U / sizeof(uint32_t) &&
	       surface->pitch >= surface->width * sizeof(uint32_t) &&
	       surface->pitch % sizeof(uint32_t) == 0 &&
	       surface->pitch <= ~0U / surface->height;
}
void framebuffer_copy_area(unsigned sx, unsigned sy, unsigned dx, unsigned dy,
			   unsigned width, unsigned height)
{
	framebuffer_surface surface;
	framebuffer_get_surface(&surface);
	if (!surface_rect(&surface, sx, sy, width, height) ||
	    !surface_rect(&surface, dx, dy, width, height))
		return;
	if (!active_fb_drv->copy_area ||
	    !active_fb_drv->copy_area(sx, sy, dx, dy, width, height)) {
		unsigned row, pitch = surface.pitch / sizeof(uint32_t);
		for (row = 0; row < height; row++) {
			unsigned offset = dy > sy ? height - row - 1 : row;
			memmove(surface.pixels + (dy + offset) * pitch + dx,
				surface.pixels + (sy + offset) * pitch + sx,
				width * sizeof(uint32_t));
		}
	}
	framebuffer_damage(dy * surface.pitch + dx * sizeof(uint32_t),
			   (dy + height - 1) * surface.pitch +
				   (dx + width) * sizeof(uint32_t));
}
void framebuffer_fill_rect(uint32_t color, unsigned x, unsigned y,
			   unsigned width, unsigned height)
{
	framebuffer_surface surface;
	framebuffer_get_surface(&surface);
	if (!surface_rect(&surface, x, y, width, height))
		return;
	if (!active_fb_drv->fill_rect ||
	    !active_fb_drv->fill_rect(color, x, y, width, height)) {
		unsigned row, col, pitch = surface.pitch / sizeof(uint32_t);
		for (row = 0; row < height; row++)
			for (col = 0; col < width; col++)
				surface.pixels[(y + row) * pitch + x + col] =
					color;
	}
	framebuffer_damage(y * surface.pitch + x * sizeof(uint32_t),
			   (y + height - 1) * surface.pitch +
				   (x + width) * sizeof(uint32_t));
}
