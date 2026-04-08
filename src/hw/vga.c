#include <hw/vga.h>

/*
 * Dispatcher layer.  fb_init() probes each known driver in priority order
 * and binds _drv to the first one that succeeds.  All public fb_* calls
 * forward through _drv.
 */

static const fb_drv_t *_drv;

void fb_init(void)
{
	if (vmsvga_drv.probe()) {
		_drv = &vmsvga_drv;
		return;
	}
	if (bochs_drv.probe()) {
		_drv = &bochs_drv;
		return;
	}
}

void fb_get_char_dims(unsigned *cols, unsigned *rows)
{
	if (_drv)
		_drv->get_char_dims(cols, rows);
}

void fb_putcell(const tty_cell_t *cell, int col, int row)
{
	if (_drv)
		_drv->putcell(cell, col, row);
}

void fb_redraw(const tty_cell_t *cells, unsigned cols, unsigned rows,
	       unsigned cursor_pos)
{
	if (_drv)
		_drv->redraw(cells, cols, rows, cursor_pos);
}

void fb_cursor_update(unsigned old_pos, unsigned new_pos,
		      const tty_cell_t *cells, unsigned cols)
{
	if (_drv)
		_drv->cursor_update(old_pos, new_pos, cells, cols);
}

void fb_scroll_line_px(void)
{
	if (_drv)
		_drv->scroll_line_px();
}

void fb_scroll_region_px(unsigned top_row, unsigned bot_row)
{
	if (_drv)
		_drv->scroll_region_px(top_row, bot_row);
}

void fb_insert_lines_px(unsigned row, unsigned bot_row, unsigned n)
{
	if (_drv)
		_drv->insert_lines_px(row, bot_row, n);
}

void fb_delete_lines_px(unsigned row, unsigned bot_row, unsigned n)
{
	if (_drv)
		_drv->delete_lines_px(row, bot_row, n);
}

void fb_clear_screen(void)
{
	if (_drv)
		_drv->clear_screen();
}

void fb_change_font(const char *name)
{
	if (_drv)
		_drv->change_font(name);
}

void fb_sync_mode(void)
{
	if (_drv && _drv->sync_mode)
		_drv->sync_mode();
}

void fb_flush(void)
{
	if (_drv && _drv->flush)
		_drv->flush();
}

unsigned fb_snapshot_size(void)
{
	if (_drv && _drv->snapshot_size)
		return _drv->snapshot_size();
	return 0;
}

void fb_snapshot_save(void *dst, unsigned size)
{
	if (_drv && _drv->snapshot_save)
		_drv->snapshot_save(dst, size);
}

void fb_snapshot_restore(const void *src, unsigned size)
{
	if (_drv && _drv->snapshot_restore)
		_drv->snapshot_restore(src, size);
}

int fb_is_char_visiable(unsigned char c)
{
	if (_drv)
		return _drv->is_char_visible(c);
	return 0;
}

void fb_cursor_erase(unsigned pos, const tty_cell_t *cells, unsigned cols)
{
	if (_drv)
		_drv->cursor_erase(pos, cells, cols);
}
