#ifndef _DEVICE_BOOT_VIDEO_H
#define _DEVICE_BOOT_VIDEO_H

#include <config.h>

/* ── Cell type ────────────────────────────────────────────────────────────── */

typedef struct {
	char ch;
	unsigned fg;
	unsigned bg;
} tty_cell_t;

/* ── Driver ops ───────────────────────────────────────────────────────────── */

/* Framebuffer operations are installed by the bound PCI driver. */
typedef struct {
	void (*get_char_dims)(unsigned *cols, unsigned *rows);
	void (*putcell)(const tty_cell_t *cell, int col, int row);
	void (*redraw)(const tty_cell_t *cells, unsigned cols, unsigned rows,
		       unsigned cursor_pos);
	void (*cursor_update)(unsigned old_pos, unsigned new_pos,
			      const tty_cell_t *cells, unsigned cols);
	void (*scroll_line_px)(void);
	void (*scroll_region_px)(unsigned top_row, unsigned bot_row);
	void (*insert_lines_px)(unsigned row, unsigned bot_row, unsigned n);
	void (*delete_lines_px)(unsigned row, unsigned bot_row, unsigned n);
	void (*clear_screen)(void);
	void (*change_font)(const char *name);
	void (*sync_mode)(void);
	void (*flush)(void);
	void (*flush_text)(void);
	void (*get_phys_window)(unsigned *phys, unsigned *size);
	unsigned (*snapshot_size)(void);
	void (*snapshot_save)(void *dst, unsigned size);
	void (*snapshot_restore)(const void *src, unsigned size);
	int (*is_char_visible)(unsigned char c);
	void (*cursor_erase)(unsigned pos, const tty_cell_t *cells,
			     unsigned cols);
} fb_drv_t;

/* Returns zero if another framebuffer is already active. */
int fb_available(void);
void fb_activate(const fb_drv_t *driver);

void fb_get_char_dims(unsigned *cols, unsigned *rows);
void fb_putcell(const tty_cell_t *cell, int col, int row);
void fb_redraw(const tty_cell_t *cells, unsigned cols, unsigned rows,
	       unsigned cursor_pos);
void fb_cursor_update(unsigned old_pos, unsigned new_pos,
		      const tty_cell_t *cells, unsigned cols);
void fb_scroll_line_px(void);
void fb_scroll_region_px(unsigned top_row, unsigned bot_row);
void fb_insert_lines_px(unsigned row, unsigned bot_row, unsigned n);
void fb_delete_lines_px(unsigned row, unsigned bot_row, unsigned n);
void fb_clear_screen(void);
void fb_change_font(const char *name);
void fb_sync_mode(void);
/* Nonzero when mapped framebuffer writes require an explicit update. */
int fb_requires_flush(void);
void fb_flush(void);
void fb_flush_text(void);
void fb_get_phys_window(unsigned *phys, unsigned *size);
unsigned fb_snapshot_size(void);
void fb_snapshot_save(void *dst, unsigned size);
void fb_snapshot_restore(const void *src, unsigned size);
int fb_is_char_visiable(unsigned char c);
void fb_cursor_erase(unsigned pos, const tty_cell_t *cells, unsigned cols);

/* ── Colors ───────────────────────────────────────────────────────────────── */

#define ARGB(a, r, g, b)                                    \
	((0xFF * 0x1000000ULL) + ((0xFF & (r)) * 0x10000) + \
	 ((0xFF & (g)) * 0x100) + ((0xFF & (b)) * 0x1))

#define VGA_COLOR_BLACK ARGB(0xff, 0x00, 0x00, 0x00)
#define VGA_COLOR_WHITE ARGB(0xff, 0xff, 0xff, 0xff)
#define VGA_COLOR_RED ARGB(0xff, 0xff, 0x00, 0x00)
#define VGA_COLOR_GREEN ARGB(0xff, 0x00, 0xff, 0x00)
#define VGA_COLOR_BLUE ARGB(0xff, 0x00, 0x00, 0xff)
#define VGA_COLOR_GRAY ARGB(0xff, 0xaa, 0xaa, 0xaa)
#define VGA_COLOR_YELLOW ARGB(0xff, 0xff, 0xff, 0x00)
#define VGA_COLOR_CYAN ARGB(0xff, 0x00, 0xff, 0xff)
#define VGA_COLOR_MAGENTA ARGB(0xff, 0xff, 0x00, 0xff)

#endif /* _DEVICE_BOOT_VIDEO_H */
