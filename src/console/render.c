#include <console/render.h>
#include <device/framebuffer.h>
#include <lib/klib.h>

static uintptr_t _fb_buffer;
static unsigned _hw_resolution_x, _hw_resolution_y, _pitch;
static unsigned _window_char_width, _window_char_height;
static const console_font *_font;
static const unsigned char bit_mask[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };

static int renderer_surface(void)
{
	framebuffer_surface surface;
	framebuffer_get_surface(&surface);
	if (!_font)
		_font = font_get("vga16");
	if (!_font || _font->width <= 0 || _font->width > 8 ||
	    _font->height <= 0 || !surface.pixels || !surface.width ||
	    !surface.height || surface.width > (~0U / sizeof(uint32_t)) ||
	    surface.pitch > (~0U / surface.height) ||
	    surface.pitch < surface.width * sizeof(uint32_t) ||
	    surface.pitch % sizeof(uint32_t))
		return 0;
	_fb_buffer = (uintptr_t)surface.pixels;
	_hw_resolution_x = surface.width;
	_hw_resolution_y = surface.height;
	_pitch = surface.pitch;
	_window_char_width = surface.width / _font->width;
	_window_char_height = surface.height / _font->height;
	return _window_char_width && _window_char_height;
}

static void render_cell(const console_cell *cell, int col, int row)
{
	if (!cell || col < 0 || row < 0 ||
	    (unsigned)col >= _window_char_width ||
	    (unsigned)row >= _window_char_height)
		return;

	unsigned *disp = (unsigned *)(uintptr_t)_fb_buffer;
	int px = col * (int)_font->width;
	int py = row * (int)_font->height;
	int i, j;
	framebuffer_damage(
		(py * _pitch + px * 4),
		((py + _font->height - 1) * _pitch + (px + _font->width) * 4));

	for (i = 0; i < _font->height; i++) {
		unsigned char bits =
			_font->glyphs[(unsigned char)cell->ch * _font->height +
				      i];
		unsigned *rowp = disp + (py + i) * (int)(_pitch / 4) + px;
		for (j = 0; j < _font->width; j++)
			rowp[j] = (bits & bit_mask[j]) ? cell->fg : cell->bg;
	}
}

static void render_cursor_cell(int col, int row, char ch, unsigned fg,
			       unsigned bg, unsigned cursor_color)
{
	if (col < 0 || row < 0 || (unsigned)col >= _window_char_width ||
	    (unsigned)row >= _window_char_height)
		return;

	unsigned *disp = (unsigned *)(uintptr_t)_fb_buffer;
	int px = col * (int)_font->width;
	int py = row * (int)_font->height;
	int i, j;
	framebuffer_damage(
		(py * _pitch + px * 4),
		((py + _font->height - 1) * _pitch + (px + _font->width) * 4));

	for (i = 0; i < _font->height; i++) {
		unsigned char bits_ch =
			_font->glyphs[(unsigned char)ch * _font->height + i];
		unsigned char bits_cur = _font->cursor_glyphs[i];
		unsigned *rowp = disp + (py + i) * (int)(_pitch / 4) + px;
		for (j = 0; j < _font->width; j++) {
			unsigned char m = bit_mask[j];
			if (bits_cur & m)
				rowp[j] = cursor_color;
			else if (bits_ch & m)
				rowp[j] = fg;
			else
				rowp[j] = bg;
		}
	}
}

/* Text rendering on the active pixel surface; caller holds the console lock. */

void console_get_char_dims(unsigned *cols, unsigned *rows)
{
	if (!renderer_surface()) {
		*cols = *rows = 0;
		return;
	}
	*cols = _window_char_width;
	*rows = _window_char_height;
}

void console_putcell(const console_cell *cell, int col, int row)
{
	if (!renderer_surface())
		return;
	render_cell(cell, col, row);
}

void console_cursor_update(unsigned old_pos, unsigned new_pos,
			   const console_cell *cells, unsigned cols)
{
	if (!renderer_surface() || !cells || !cols ||
	    cols > _window_char_width || new_pos / cols >= _window_char_height)
		return;
	if (old_pos / cols < _window_char_height)
		render_cell(&cells[old_pos], (int)(old_pos % cols),
			    (int)(old_pos / cols));

	int new_col = (int)(new_pos % cols);
	int new_row = (int)(new_pos / cols);
	const console_cell *nc = &cells[new_pos];

	if (nc->ch == '\0' || nc->ch == ' ')
		render_cursor_cell(new_col, new_row, ' ', VGA_COLOR_WHITE,
				   nc->bg, VGA_COLOR_WHITE);
	else
		render_cursor_cell(new_col, new_row, nc->ch, nc->fg, nc->bg,
				   VGA_COLOR_WHITE);
}

void console_redraw(const console_cell *cells, unsigned cols, unsigned rows,
		    unsigned cursor_pos)
{
	if (!renderer_surface() || !cells || !cols || !rows ||
	    cols > _window_char_width || rows > _window_char_height)
		return;
	unsigned i;
	unsigned total = cols * rows;
	framebuffer_damage(0, _pitch * _hw_resolution_y);

	memset((char *)(uintptr_t)_fb_buffer, 0, _pitch * _hw_resolution_y);

	for (i = 0; i < total; i++)
		render_cell(&cells[i], (int)(i % cols), (int)(i / cols));

	if (cursor_pos < total) {
		unsigned nc = cursor_pos % cols;
		unsigned nr = cursor_pos / cols;
		const console_cell *c = &cells[cursor_pos];

		if (c->ch == '\0' || c->ch == ' ')
			render_cursor_cell((int)nc, (int)nr, ' ',
					   VGA_COLOR_WHITE, c->bg,
					   VGA_COLOR_WHITE);
		else
			render_cursor_cell((int)nc, (int)nr, c->ch, c->fg,
					   c->bg, VGA_COLOR_WHITE);
	}
	framebuffer_present();
}

void console_scroll_line(void)
{
	if (!renderer_surface())
		return;
	console_scroll_region(0, _window_char_height - 1);
}
void console_scroll_region(unsigned top_row, unsigned bot_row)
{
	console_delete_lines(top_row, bot_row, 1);
}
void console_insert_lines(unsigned row, unsigned bot_row, unsigned n)
{
	if (!renderer_surface() || row > bot_row ||
	    bot_row >= _window_char_height || !n)
		return;
	unsigned fh = _font->height;
	unsigned count = bot_row - row + 1;
	if (n > count)
		n = count;
	framebuffer_copy_area(0, row * fh, 0, (row + n) * fh, _hw_resolution_x,
			      (count - n) * fh);
	framebuffer_fill_rect(0, 0, row * fh, _hw_resolution_x, n * fh);
}
void console_delete_lines(unsigned row, unsigned bot_row, unsigned n)
{
	if (!renderer_surface() || row > bot_row ||
	    bot_row >= _window_char_height || !n)
		return;
	unsigned fh = _font->height;
	unsigned count = bot_row - row + 1;
	if (n > count)
		n = count;
	framebuffer_copy_area(0, (row + n) * fh, 0, row * fh, _hw_resolution_x,
			      (count - n) * fh);
	framebuffer_fill_rect(0, 0, (bot_row - n + 1) * fh, _hw_resolution_x,
			      n * fh);
}

void console_clear_screen(void)
{
	if (!renderer_surface())
		return;
	framebuffer_damage(0, _pitch * _hw_resolution_y);
	memset((char *)(uintptr_t)_fb_buffer, 0, _pitch * _hw_resolution_y);
}

void console_cursor_erase(unsigned pos, const console_cell *cells,
			  unsigned cols)
{
	if (!renderer_surface() || !cells || !cols ||
	    cols > _window_char_width || pos / cols >= _window_char_height)
		return;
	render_cell(&cells[pos], (int)(pos % cols), (int)(pos / cols));
}

void console_change_font(const char *name)
{
	const console_font *f = font_get(name);
	if (f)
		_font = f;
}

int console_is_char_visible(unsigned char c)
{
	if (!renderer_surface())
		return 0;
	if (isprint(c))
		return 1;
	if (!_font)
		return 0;
	return (unsigned)c < (unsigned)_font->charcount;
}

void console_sync_mode(void)
{
	framebuffer_sync_mode();
	renderer_surface();
}
void console_present(void)
{
	framebuffer_present();
}
