#include <int/int.h>
#include <boot/multiboot.h>
#include <hw/pci.h>
#include <hw/vga.h>

/* Bochs VBE / QEMU stdvga register interface */
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA 0x01CF
#define VBE_DISPI_ID5 0xB0C5
#define VBE_DISPI_DISABLED 0x00
#define VBE_DISPI_ENABLED 0x01
#define VBE_DISPI_LFB_ENABLED 0x40
#define VBE_DISPI_NOCLEARMEM 0x80
#define VBE_DISPI_INDEX_ID 0
#define VBE_DISPI_INDEX_XRES 1
#define VBE_DISPI_INDEX_YRES 2
#define VBE_DISPI_INDEX_BPP 3
#define VBE_DISPI_INDEX_ENABLE 4
#include <hw/font.h>
#include <mm/mm.h>
#include <macro.h>
#include <lib/port.h>
#include <lib/klib.h>

static const unsigned char bit_mask[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };

static unsigned _fb_buffer;
static unsigned _fb_phys;
static unsigned _fb_mapped_bytes;
static unsigned _hw_resolution_x;
static unsigned _hw_resolution_y;
static unsigned _window_char_width;
static unsigned _window_char_height;
static const fb_font_t *_font = NULL;

static void bochs_ensure_fb_mapping(unsigned width, unsigned height)
{
	unsigned need_bytes;
	unsigned a;

	if (width == 0 || height == 0)
		return;

	need_bytes = width * height * (VGA_COLOR_DEPTH / 8);
	if (need_bytes <= _fb_mapped_bytes)
		return;

	for (a = _fb_phys + _fb_mapped_bytes; a < _fb_phys + need_bytes;
	     a += PAGE_SIZE)
		mm_map_io(a);
	RELOAD_CR3();
	_fb_mapped_bytes = need_bytes;
}

static unsigned bochs_snapshot_size(void)
{
	return _hw_resolution_x * _hw_resolution_y * (VGA_COLOR_DEPTH / 8);
}

static void bochs_snapshot_save(void *dst, unsigned size)
{
	unsigned need = bochs_snapshot_size();

	if (!dst || size < need)
		return;
	memcpy(dst, (const void *)_fb_buffer, need);
}

static void bochs_snapshot_restore(const void *src, unsigned size)
{
	unsigned need = bochs_snapshot_size();

	if (!src || size < need)
		return;
	memcpy((void *)_fb_buffer, src, need);
}

static unsigned short bga_read_register(unsigned short idx);

/* ── Pixel rendering ──────────────────────────────────────────────────────── */

static void render_cell(const tty_cell_t *cell, int col, int row)
{
	if (!_font)
		return;

	unsigned *disp = (unsigned *)_fb_buffer;
	int px = col * (int)_font->width;
	int py = row * (int)_font->height;
	int i, j;

	for (i = 0; i < _font->height; i++) {
		unsigned char bits =
			_font->glyphs[(unsigned char)cell->ch * _font->height +
				      i];
		unsigned *rowp = disp + (py + i) * (int)_hw_resolution_x + px;
		for (j = 0; j < _font->width; j++)
			rowp[j] = (bits & bit_mask[j]) ? cell->fg : cell->bg;
	}
}

static void render_cursor_cell(int col, int row, char ch, unsigned fg,
			       unsigned bg, unsigned cursor_color)
{
	if (!_font)
		return;

	unsigned *disp = (unsigned *)_fb_buffer;
	int px = col * (int)_font->width;
	int py = row * (int)_font->height;
	int i, j;

	for (i = 0; i < _font->height; i++) {
		unsigned char bits_ch =
			_font->glyphs[(unsigned char)ch * _font->height + i];
		unsigned char bits_cur = _font->cursor_glyphs[i];
		unsigned *rowp = disp + (py + i) * (int)_hw_resolution_x + px;
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

/* ── Driver ops ───────────────────────────────────────────────────────────── */

static void bochs_get_char_dims(unsigned *cols, unsigned *rows)
{
	*cols = _window_char_width;
	*rows = _window_char_height;
}

static void bochs_putcell(const tty_cell_t *cell, int col, int row)
{
	render_cell(cell, col, row);
}

static void bochs_cursor_update(unsigned old_pos, unsigned new_pos,
				const tty_cell_t *cells, unsigned cols)
{
	render_cell(&cells[old_pos], (int)(old_pos % cols),
		    (int)(old_pos / cols));

	int new_col = (int)(new_pos % cols);
	int new_row = (int)(new_pos / cols);
	const tty_cell_t *nc = &cells[new_pos];

	if (nc->ch == '\0' || nc->ch == ' ')
		render_cursor_cell(new_col, new_row, ' ', VGA_COLOR_WHITE,
				   nc->bg, VGA_COLOR_WHITE);
	else
		render_cursor_cell(new_col, new_row, nc->ch, nc->fg, nc->bg,
				   VGA_COLOR_WHITE);
}

static void bochs_redraw(const tty_cell_t *cells, unsigned cols, unsigned rows,
			 unsigned cursor_pos)
{
	unsigned i;
	unsigned total = cols * rows;

	memset((char *)_fb_buffer, 0,
	       _hw_resolution_x * _hw_resolution_y * (VGA_COLOR_DEPTH / 8));

	for (i = 0; i < total; i++)
		render_cell(&cells[i], (int)(i % cols), (int)(i / cols));

	{
		unsigned nc = cursor_pos % cols;
		unsigned nr = cursor_pos / cols;
		const tty_cell_t *c = &cells[cursor_pos];

		if (c->ch == '\0' || c->ch == ' ')
			render_cursor_cell((int)nc, (int)nr, ' ',
					   VGA_COLOR_WHITE, c->bg,
					   VGA_COLOR_WHITE);
		else
			render_cursor_cell((int)nc, (int)nr, c->ch, c->fg,
					   c->bg, VGA_COLOR_WHITE);
	}
}

static void bochs_scroll_line_px(void)
{
	if (!_font)
		return;

	unsigned bpr = _hw_resolution_x * (unsigned)_font->height *
		       (VGA_COLOR_DEPTH / 8);
	unsigned copy_size = bpr * (_window_char_height - 1);
	char *fb = (char *)_fb_buffer;

	memmove(fb, fb + bpr, copy_size);
	memset(fb + copy_size, 0, bpr);
}

static void bochs_scroll_region_px(unsigned top_row, unsigned bot_row)
{
	if (!_font)
		return;

	unsigned bpr = _hw_resolution_x * (unsigned)_font->height *
		       (VGA_COLOR_DEPTH / 8);
	char *fb = (char *)_fb_buffer;

	memmove(fb + top_row * bpr, fb + (top_row + 1) * bpr,
		(bot_row - top_row) * bpr);
	memset(fb + bot_row * bpr, 0, bpr);
}

static void bochs_insert_lines_px(unsigned row, unsigned bot_row, unsigned n)
{
	if (!_font)
		return;

	unsigned bpr = _hw_resolution_x * (unsigned)_font->height *
		       (VGA_COLOR_DEPTH / 8);
	char *fb = (char *)_fb_buffer;

	if (n >= bot_row - row + 1) {
		memset(fb + row * bpr, 0, (bot_row - row + 1) * bpr);
		return;
	}
	unsigned move_rows = bot_row - row + 1 - n;
	memmove(fb + (row + n) * bpr, fb + row * bpr, move_rows * bpr);
	memset(fb + row * bpr, 0, n * bpr);
}

static void bochs_delete_lines_px(unsigned row, unsigned bot_row, unsigned n)
{
	if (!_font)
		return;

	unsigned bpr = _hw_resolution_x * (unsigned)_font->height *
		       (VGA_COLOR_DEPTH / 8);
	char *fb = (char *)_fb_buffer;

	if (n >= bot_row - row + 1) {
		memset(fb + row * bpr, 0, (bot_row - row + 1) * bpr);
		return;
	}
	unsigned move_rows = bot_row - row + 1 - n;
	memmove(fb + row * bpr, fb + (row + n) * bpr, move_rows * bpr);
	memset(fb + (bot_row - n + 1) * bpr, 0, n * bpr);
}

static void bochs_clear_screen(void)
{
	memset((char *)_fb_buffer, 0,
	       _hw_resolution_x * _hw_resolution_y * (VGA_COLOR_DEPTH / 8));
}

static void bochs_cursor_erase(unsigned pos, const tty_cell_t *cells,
			       unsigned cols)
{
	render_cell(&cells[pos], (int)(pos % cols), (int)(pos / cols));
}

static void bochs_change_font(const char *name)
{
	const fb_font_t *f = font_get(name);
	if (f)
		_font = f;
}

static void bochs_sync_mode(void)
{
	unsigned width;
	unsigned height;

	width = bga_read_register(VBE_DISPI_INDEX_XRES);
	height = bga_read_register(VBE_DISPI_INDEX_YRES);
	if (width > 0 && height > 0)
		bochs_ensure_fb_mapping(width, height);
	if (width > 0)
		_hw_resolution_x = width;
	if (height > 0)
		_hw_resolution_y = height;
	if (_font) {
		_window_char_width = _hw_resolution_x / (unsigned)_font->width;
		_window_char_height =
			_hw_resolution_y / (unsigned)_font->height;
	}
}

static void bochs_flush(void)
{
}

static int bochs_is_char_visible(unsigned char c)
{
	if (isprint(c))
		return 1;
	if (!_font)
		return 0;
	return (unsigned)c < (unsigned)_font->charcount;
}

/* ── BGA / VBE hardware ───────────────────────────────────────────────────── */

static void bga_write_register(unsigned short idx, unsigned short val)
{
	port_write_word(VBE_DISPI_IOPORT_INDEX, idx);
	port_write_word(VBE_DISPI_IOPORT_DATA, val);
}

static unsigned short bga_read_register(unsigned short idx)
{
	port_write_word(VBE_DISPI_IOPORT_INDEX, idx);
	return port_read_word(VBE_DISPI_IOPORT_DATA);
}

static int bga_is_available(void)
{
	if (bga_read_register(VBE_DISPI_INDEX_ID) != VBE_DISPI_ID5) {
		bga_write_register(VBE_DISPI_INDEX_ID, VBE_DISPI_ID5);
		return bga_read_register(VBE_DISPI_INDEX_ID) == VBE_DISPI_ID5;
	}
	return 1;
}

static void bga_set_video_mode(unsigned int Width, unsigned int Height,
			       unsigned int BitDepth, int UseLinearFrameBuffer,
			       int ClearVideoMemory)
{
	bga_write_register(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
	bga_write_register(VBE_DISPI_INDEX_XRES, Width);
	bga_write_register(VBE_DISPI_INDEX_YRES, Height);
	bga_write_register(VBE_DISPI_INDEX_BPP, BitDepth);
	bga_write_register(
		VBE_DISPI_INDEX_ENABLE,
		VBE_DISPI_ENABLED |
			(UseLinearFrameBuffer ? VBE_DISPI_LFB_ENABLED : 0) |
			(ClearVideoMemory ? 0 : VBE_DISPI_NOCLEARMEM));
}

static void bochs_scan_pci(uint32_t device, uint16_t v, uint16_t d, void *extra)
{
	if (v == 0x1234 && d == 0x1111) {
		unsigned t = pci_read_field(device, PCI_BAR0, 4);
		if (t > 0)
			*((unsigned *)extra) = t & 0xFFFFFFF0u;
	}
}

/* ── Probe / init ─────────────────────────────────────────────────────────── */

static int bochs_probe(void)
{
	if (!bga_is_available())
		return 0;

	bga_set_video_mode(VGA_RESOLUTION_X, VGA_RESOLUTION_Y, VGA_COLOR_DEPTH,
			   1, 1);

	unsigned fb_phys = 0;
	pci_scan(bochs_scan_pci, -1, &fb_phys);

	if (!fb_phys)
		return 0;

	unsigned fb_size =
		VGA_RESOLUTION_X * VGA_RESOLUTION_Y * (VGA_COLOR_DEPTH / 8);
	unsigned a;
	for (a = fb_phys; a < fb_phys + fb_size; a += PAGE_SIZE)
		mm_map_io(a);
	RELOAD_CR3();
	_fb_phys = fb_phys;
	_fb_buffer = fb_phys;
	_fb_mapped_bytes = fb_size;

	_hw_resolution_x = VGA_RESOLUTION_X;
	_hw_resolution_y = VGA_RESOLUTION_Y;

	bochs_change_font("vga16");

	_window_char_width = _hw_resolution_x / (unsigned)_font->width;
	_window_char_height = _hw_resolution_y / (unsigned)_font->height;

	return 1;
}

/* ── Driver descriptor ────────────────────────────────────────────────────── */

const fb_drv_t bochs_drv = {
	.probe = bochs_probe,
	.get_char_dims = bochs_get_char_dims,
	.putcell = bochs_putcell,
	.redraw = bochs_redraw,
	.cursor_update = bochs_cursor_update,
	.scroll_line_px = bochs_scroll_line_px,
	.scroll_region_px = bochs_scroll_region_px,
	.insert_lines_px = bochs_insert_lines_px,
	.delete_lines_px = bochs_delete_lines_px,
	.clear_screen = bochs_clear_screen,
	.change_font = bochs_change_font,
	.sync_mode = bochs_sync_mode,
	.flush = bochs_flush,
	.snapshot_size = bochs_snapshot_size,
	.snapshot_save = bochs_snapshot_save,
	.snapshot_restore = bochs_snapshot_restore,
	.is_char_visible = bochs_is_char_visible,
	.cursor_erase = bochs_cursor_erase,
};
