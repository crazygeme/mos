#include <hw/vga.h>

static const fb_drv_t *active_fb_drv;

void fb_init(void)
{
	fb_driver_ref_t *drv;

	for (drv = __fb_driver_start; drv < __fb_driver_end; drv++) {
		if (*drv && (*drv)->probe && (*drv)->probe()) {
			active_fb_drv = *drv;
			return;
		}
	}
}

void fb_get_char_dims(unsigned *cols, unsigned *rows)
{
	if (active_fb_drv)
		active_fb_drv->get_char_dims(cols, rows);
}

void fb_putcell(const tty_cell_t *cell, int col, int row)
{
	if (active_fb_drv)
		active_fb_drv->putcell(cell, col, row);
}

void fb_redraw(const tty_cell_t *cells, unsigned cols, unsigned rows,
	       unsigned cursor_pos)
{
	if (active_fb_drv)
		active_fb_drv->redraw(cells, cols, rows, cursor_pos);
}

void fb_cursor_update(unsigned old_pos, unsigned new_pos,
		      const tty_cell_t *cells, unsigned cols)
{
	if (active_fb_drv)
		active_fb_drv->cursor_update(old_pos, new_pos, cells, cols);
}

void fb_scroll_line_px(void)
{
	if (active_fb_drv)
		active_fb_drv->scroll_line_px();
}

void fb_scroll_region_px(unsigned top_row, unsigned bot_row)
{
	if (active_fb_drv)
		active_fb_drv->scroll_region_px(top_row, bot_row);
}

void fb_insert_lines_px(unsigned row, unsigned bot_row, unsigned n)
{
	if (active_fb_drv)
		active_fb_drv->insert_lines_px(row, bot_row, n);
}

void fb_delete_lines_px(unsigned row, unsigned bot_row, unsigned n)
{
	if (active_fb_drv)
		active_fb_drv->delete_lines_px(row, bot_row, n);
}

void fb_clear_screen(void)
{
	if (active_fb_drv)
		active_fb_drv->clear_screen();
}

void fb_change_font(const char *name)
{
	if (active_fb_drv)
		active_fb_drv->change_font(name);
}

void fb_sync_mode(void)
{
	if (active_fb_drv && active_fb_drv->sync_mode)
		active_fb_drv->sync_mode();
}

void fb_flush(void)
{
	if (active_fb_drv && active_fb_drv->flush)
		active_fb_drv->flush();
}

void fb_get_phys_window(unsigned *phys, unsigned *size)
{
	if (phys)
		*phys = 0;
	if (size)
		*size = 0;
	if (active_fb_drv && active_fb_drv->get_phys_window)
		active_fb_drv->get_phys_window(phys, size);
}

unsigned fb_snapshot_size(void)
{
	if (active_fb_drv && active_fb_drv->snapshot_size)
		return active_fb_drv->snapshot_size();
	return 0;
}

void fb_snapshot_save(void *dst, unsigned size)
{
	if (active_fb_drv && active_fb_drv->snapshot_save)
		active_fb_drv->snapshot_save(dst, size);
}

void fb_snapshot_restore(const void *src, unsigned size)
{
	if (active_fb_drv && active_fb_drv->snapshot_restore)
		active_fb_drv->snapshot_restore(src, size);
}

int fb_is_char_visiable(unsigned char c)
{
	if (active_fb_drv)
		return active_fb_drv->is_char_visible(c);
	return 0;
}

void fb_cursor_erase(unsigned pos, const tty_cell_t *cells, unsigned cols)
{
	if (active_fb_drv)
		active_fb_drv->cursor_erase(pos, cells, cols);
}
