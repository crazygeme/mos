#include <int/int.h>
#include <boot/multiboot.h>
#include <hw/pci.h>
#include <hw/vga.h>
#include <hw/font.h>
#include <mm/mm.h>
#include <macro.h>
#include <lib/port.h>
#include <lib/klib.h>

static const unsigned char bit_mask[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };

/* Internal framebuffer state */

static unsigned int *fb_buffer = (unsigned int *)0xE0000;

static unsigned _fb_buffer;
static unsigned _fb_buffer_phy;
static unsigned _hw_resolution_x;
static unsigned _hw_resolution_y;
static unsigned _window_char_width;
static unsigned _window_char_height;
static unsigned _fb_x_off;
static unsigned _fb_y_off;
static const fb_font_t *_font = NULL;

/* Pixel rendering primitives */

static void fb_render_cell(const tty_cell_t *cell, int col, int row)
{
	if (!_font)
		return;

	unsigned *disp = (unsigned *)_fb_buffer;
	int px = (int)(_fb_x_off + (unsigned)col * (unsigned)_font->width);
	int py = (int)(_fb_y_off + (unsigned)row * (unsigned)_font->height);
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

static void fb_render_cursor_cell(int col, int row, char ch, unsigned fg,
				  unsigned bg, unsigned cursor_color)
{
	if (!_font)
		return;

	unsigned *disp = (unsigned *)_fb_buffer;
	int px = (int)(_fb_x_off + (unsigned)col * (unsigned)_font->width);
	int py = (int)(_fb_y_off + (unsigned)row * (unsigned)_font->height);
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

/* Public API */

void fb_get_char_dims(unsigned *cols, unsigned *rows)
{
	*cols = _window_char_width;
	*rows = _window_char_height;
}

void fb_putcell(const tty_cell_t *cell, int col, int row)
{
	fb_render_cell(cell, col, row);
}

void fb_cursor_update(unsigned old_pos, unsigned new_pos,
		      const tty_cell_t *cells, unsigned cols)
{
	int old_col = (int)(old_pos % cols);
	int old_row = (int)(old_pos / cols);
	const tty_cell_t *oc = &cells[old_pos];

	fb_render_cell(oc, old_col, old_row);

	int new_col = (int)(new_pos % cols);
	int new_row = (int)(new_pos / cols);
	const tty_cell_t *nc = &cells[new_pos];

	if (nc->ch == '\0' || nc->ch == ' ')
		fb_render_cursor_cell(new_col, new_row, ' ', VGA_COLOR_WHITE,
				      nc->bg, VGA_COLOR_WHITE);
	else
		fb_render_cursor_cell(new_col, new_row, nc->ch, nc->fg, nc->bg,
				      VGA_COLOR_WHITE);
}

void fb_redraw(const tty_cell_t *cells, unsigned cols, unsigned rows,
	       unsigned cursor_pos)
{
	unsigned i;
	unsigned total = cols * rows;
	unsigned pix_len =
		_hw_resolution_x * _hw_resolution_y * (VGA_COLOR_DEPTH / 8);

	memset((char *)_fb_buffer, 0, pix_len);

	for (i = 0; i < total; i++) {
		const tty_cell_t *c = &cells[i];
		fb_render_cell(c, (int)(i % cols), (int)(i / cols));
	}

	{
		unsigned nc = cursor_pos % cols;
		unsigned nr = cursor_pos / cols;
		const tty_cell_t *c = &cells[cursor_pos];

		if (c->ch == '\0' || c->ch == ' ')
			fb_render_cursor_cell((int)nc, (int)nr, ' ',
					      VGA_COLOR_WHITE, c->bg,
					      VGA_COLOR_WHITE);
		else
			fb_render_cursor_cell((int)nc, (int)nr, c->ch, c->fg,
					      c->bg, VGA_COLOR_WHITE);
	}
}

void fb_scroll_line_px(void)
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

void fb_scroll_region_px(unsigned top_row, unsigned bot_row)
{
	if (!_font)
		return;

	unsigned bpr = _hw_resolution_x * (unsigned)_font->height *
		       (VGA_COLOR_DEPTH / 8);
	unsigned region_rows = bot_row - top_row;
	char *fb = (char *)_fb_buffer;

	memmove(fb + top_row * bpr, fb + (top_row + 1) * bpr,
		region_rows * bpr);
	memset(fb + bot_row * bpr, 0, bpr);
}

void fb_insert_lines_px(unsigned row, unsigned bot_row, unsigned n)
{
	if (!_font)
		return;

	unsigned bpr = _hw_resolution_x * (unsigned)_font->height *
		       (VGA_COLOR_DEPTH / 8);
	char *fb = (char *)_fb_buffer;
	unsigned move_rows;

	if (n >= bot_row - row + 1) {
		memset(fb + row * bpr, 0, (bot_row - row + 1) * bpr);
		return;
	}
	move_rows = bot_row - row + 1 - n;
	memmove(fb + (row + n) * bpr, fb + row * bpr, move_rows * bpr);
	memset(fb + row * bpr, 0, n * bpr);
}

void fb_delete_lines_px(unsigned row, unsigned bot_row, unsigned n)
{
	if (!_font)
		return;

	unsigned bpr = _hw_resolution_x * (unsigned)_font->height *
		       (VGA_COLOR_DEPTH / 8);
	char *fb = (char *)_fb_buffer;
	unsigned move_rows;

	if (n >= bot_row - row + 1) {
		memset(fb + row * bpr, 0, (bot_row - row + 1) * bpr);
		return;
	}
	move_rows = bot_row - row + 1 - n;
	memmove(fb + row * bpr, fb + (row + n) * bpr, move_rows * bpr);
	memset(fb + (bot_row - n + 1) * bpr, 0, n * bpr);
}

void fb_clear_screen(void)
{
	unsigned len =
		_hw_resolution_x * _hw_resolution_y * (VGA_COLOR_DEPTH / 8);
	memset((char *)_fb_buffer, 0, len);
}

/* Bochs VBE / QEMU VGA hardware init */

static void bga_write_register(unsigned short IndexValue,
			       unsigned short DataValue)
{
	port_write_word(VBE_DISPI_IOPORT_INDEX, IndexValue);
	port_write_word(VBE_DISPI_IOPORT_DATA, DataValue);
}

static unsigned short bga_read_register(unsigned short IndexValue)
{
	port_write_word(VBE_DISPI_IOPORT_INDEX, IndexValue);
	return port_read_word(VBE_DISPI_IOPORT_DATA);
}

static int bga_is_available(void)
{
	int old_version =
		(bga_read_register(VBE_DISPI_INDEX_ID) != VBE_DISPI_ID5);

	if (old_version) {
		bga_write_register(VBE_DISPI_INDEX_ID, VBE_DISPI_ID5);
		return (bga_read_register(VBE_DISPI_INDEX_ID) == VBE_DISPI_ID5);
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
		if (t > 0) {
			*((uint8_t **)extra) = (uint8_t *)(t & 0xFFFFFFF0);
		}
	}
}

void fb_init(void)
{
	unsigned resolution_x, resolution_y;
	unsigned mm_size = 0;
	unsigned dma = 0;

	bga_is_available();
	bga_set_video_mode(VGA_RESOLUTION_X, VGA_RESOLUTION_Y, VGA_COLOR_DEPTH,
			   1, 1);

	pci_scan(bochs_scan_pci, -1, &fb_buffer);

	if (fb_buffer) {
		resolution_x = VGA_RESOLUTION_X;
		resolution_y = VGA_RESOLUTION_Y;
	} else {
		resolution_x = 0;
		resolution_y = 0;
	}

	_hw_resolution_x = resolution_x;
	_hw_resolution_y = resolution_y;

	dma = (unsigned)fb_buffer;

	_fb_buffer_phy = (unsigned)fb_buffer;

	_fb_x_off = _fb_y_off = 0;

	mm_size = _hw_resolution_x * _hw_resolution_y * 4;
	if (_fb_buffer_phy) {
		unsigned end = _fb_buffer_phy + mm_size;
		for (dma = _fb_buffer_phy; dma < end; dma += PAGE_SIZE)
			mm_map_io(dma);
		RELOAD_CR3();
		_fb_buffer = _fb_buffer_phy;
	}

	fb_change_font("vga16");

	_window_char_width = _hw_resolution_x / (unsigned)_font->width;
	_window_char_height = _hw_resolution_y / (unsigned)_font->height;
}

void fb_cursor_erase(unsigned pos, const tty_cell_t *cells, unsigned cols)
{
	fb_render_cell(&cells[pos], (int)(pos % cols), (int)(pos / cols));
}

void fb_change_font(const char *name)
{
	const fb_font_t *f = font_get(name);
	if (f)
		_font = f;
}

int fb_is_char_visiable(unsigned char c)
{
	if (c < 127)
		return isprint(c);

	if (!_font)
		return 0;

	return (unsigned)c < (unsigned)_font->charcount;
}
