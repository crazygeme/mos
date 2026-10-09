#ifndef MOS_DEVICE_FRAMEBUFFER_H
#define MOS_DEVICE_FRAMEBUFFER_H
#include <stdint.h>

/* Linear XRGB8888 surface. Pitch is in bytes; pixels may be a shadow buffer. */
typedef struct {
	uint32_t *pixels;
	unsigned width, height, pitch;
} framebuffer_surface;

/* All operations use pixels or bytes, never terminal cells or fonts. */
typedef struct {
	void (*surface)(framebuffer_surface *surface);
	void (*sync_mode)(void);
	void (*damage)(unsigned begin, unsigned end);
	void (*present)(void);
	void (*refresh)(void);
	/* Return nonzero if hardware completed the operation; zero uses CPU fallback. */
	int (*copy_area)(unsigned sx, unsigned sy, unsigned dx, unsigned dy,
			 unsigned width, unsigned height);
	int (*fill_rect)(uint32_t color, unsigned x, unsigned y, unsigned width,
			 unsigned height);
	void (*get_phys_window)(unsigned *phys, unsigned *size);
	unsigned (*snapshot_size)(void);
	void (*snapshot_save)(void *dst, unsigned size);
	void (*snapshot_restore)(const void *src, unsigned size);
} framebuffer_ops;

int framebuffer_available(void);
void framebuffer_register(const framebuffer_ops *ops);
void framebuffer_get_surface(framebuffer_surface *surface);
void framebuffer_sync_mode(void);
void framebuffer_damage(unsigned begin, unsigned end);
void framebuffer_present(void);
void framebuffer_copy_area(unsigned sx, unsigned sy, unsigned dx, unsigned dy,
			   unsigned width, unsigned height);
void framebuffer_fill_rect(uint32_t color, unsigned x, unsigned y,
			   unsigned width, unsigned height);
int framebuffer_requires_refresh(void);
void framebuffer_refresh(void);
void framebuffer_get_phys_window(unsigned *phys, unsigned *size);
unsigned framebuffer_snapshot_size(void);
void framebuffer_snapshot_save(void *dst, unsigned size);
void framebuffer_snapshot_restore(const void *src, unsigned size);
#endif
