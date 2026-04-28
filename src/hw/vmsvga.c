#include <int/int.h>
#include <boot/multiboot.h>
#include <hw/pci.h>
#include <hw/vga.h>
#include <hw/font.h>
#include <mm/mm.h>
#include <macro.h>
#include <lib/port.h>
#include <lib/klib.h>

/* VMware SVGA2 PCI IDs */
#define VMSVGA_VENDOR_ID 0x15AD
#define VMSVGA_DEVICE_ID 0x0405

/* I/O port offsets from BAR0 */
#define SVGA_INDEX_PORT 0
#define SVGA_VALUE_PORT 1

/* Register indices */
#define SVGA_REG_ID 0
#define SVGA_REG_ENABLE 1
#define SVGA_REG_WIDTH 2
#define SVGA_REG_HEIGHT 3
#define SVGA_REG_BPP 7
#define SVGA_REG_CAPABILITIES 17
#define SVGA_REG_MEM_SIZE 19
#define SVGA_REG_CONFIG_DONE 20
#define SVGA_REG_SYNC 21
#define SVGA_REG_BUSY 22

/* SVGA2 device ID */
#define SVGA_ID_2 (0x900000UL << 8 | 2) /* 0x90000002 */

/* Capabilities */
#define SVGA_CAP_RECT_FILL 0x0001
#define SVGA_CAP_RECT_COPY 0x0002

/* FIFO header dword indices */
#define SVGA_FIFO_MIN 0
#define SVGA_FIFO_MAX 1
#define SVGA_FIFO_NEXT_CMD 2
#define SVGA_FIFO_STOP 3

/* FIFO commands */
#define SVGA_CMD_UPDATE 1
#define SVGA_CMD_RECT_FILL 2
#define SVGA_CMD_RECT_COPY 3

static const unsigned char bit_mask[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };

static unsigned short _iobase;
static uint32_t *_fifo;
static uint32_t _caps;

static unsigned _fb_buffer;
static unsigned _fb_phys;
static unsigned _fb_mapped_bytes;
static unsigned _hw_resolution_x;
static unsigned _hw_resolution_y;
static unsigned _window_char_width;
static unsigned _window_char_height;
static const fb_font_t *_font = NULL;

static void svga_update(unsigned x, unsigned y, unsigned w, unsigned h);

static void vmsvga_ensure_fb_mapping(unsigned width, unsigned height)
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

static unsigned vmsvga_snapshot_size(void)
{
	return _hw_resolution_x * _hw_resolution_y * (VGA_COLOR_DEPTH / 8);
}

static void vmsvga_get_phys_window(unsigned *phys, unsigned *size)
{
	if (phys)
		*phys = _fb_phys;
	if (size)
		*size = vmsvga_snapshot_size();
}

static void vmsvga_snapshot_save(void *dst, unsigned size)
{
	unsigned need = vmsvga_snapshot_size();

	if (!dst || size < need)
		return;
	memcpy(dst, (const void *)_fb_buffer, need);
}

static void vmsvga_snapshot_restore(const void *src, unsigned size)
{
	unsigned need = vmsvga_snapshot_size();

	if (!src || size < need)
		return;
	memcpy((void *)_fb_buffer, src, need);
	svga_update(0, 0, _hw_resolution_x, _hw_resolution_y);
}

/* ── Register I/O ─────────────────────────────────────────────────────────── */

static void svga_write_reg(unsigned index, unsigned value)
{
	port_write_dword(_iobase + SVGA_INDEX_PORT, index);
	port_write_dword(_iobase + SVGA_VALUE_PORT, value);
}

static unsigned svga_read_reg(unsigned index)
{
	port_write_dword(_iobase + SVGA_INDEX_PORT, index);
	return port_read_dword(_iobase + SVGA_VALUE_PORT);
}

/* ── FIFO ─────────────────────────────────────────────────────────────────── */

static void fifo_write(uint32_t val)
{
	uint32_t next = _fifo[SVGA_FIFO_NEXT_CMD];

	_fifo[next / 4] = val;
	next += 4;
	if (next >= _fifo[SVGA_FIFO_MAX])
		next = _fifo[SVGA_FIFO_MIN];

	while (next == _fifo[SVGA_FIFO_STOP]) {
		svga_write_reg(SVGA_REG_SYNC, 1);
		while (svga_read_reg(SVGA_REG_BUSY))
			;
	}

	_fifo[SVGA_FIFO_NEXT_CMD] = next;
}

static void fifo_sync(void)
{
	svga_write_reg(SVGA_REG_SYNC, 1);
	while (svga_read_reg(SVGA_REG_BUSY))
		;
}

/* ── Hardware-accelerated primitives ─────────────────────────────────────── */

static void svga_update(unsigned x, unsigned y, unsigned w, unsigned h)
{
	fifo_write(SVGA_CMD_UPDATE);
	fifo_write(x);
	fifo_write(y);
	fifo_write(w);
	fifo_write(h);
	/* No fifo_sync: fire-and-forget. fifo_sync is reserved for
	 * RECT_COPY/RECT_FILL to ensure VRAM ordering. */
}

static void svga_rect_copy(unsigned srcX, unsigned srcY, unsigned dstX,
			   unsigned dstY, unsigned w, unsigned h)
{
	fifo_write(SVGA_CMD_RECT_COPY);
	fifo_write(srcX);
	fifo_write(srcY);
	fifo_write(dstX);
	fifo_write(dstY);
	fifo_write(w);
	fifo_write(h);
	fifo_sync();
}

static void svga_rect_fill(unsigned color, unsigned x, unsigned y, unsigned w,
			   unsigned h)
{
	if (_caps & SVGA_CAP_RECT_FILL) {
		fifo_write(SVGA_CMD_RECT_FILL);
		fifo_write(color);
		fifo_write(x);
		fifo_write(y);
		fifo_write(w);
		fifo_write(h);
		fifo_sync();
		return;
	}
	unsigned *fb = (unsigned *)_fb_buffer;
	unsigned row, col;
	for (row = y; row < y + h; row++)
		for (col = x; col < x + w; col++)
			fb[row * _hw_resolution_x + col] = color;
	svga_update(x, y, w, h);
}

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

static void vmsvga_get_char_dims(unsigned *cols, unsigned *rows)
{
	*cols = _window_char_width;
	*rows = _window_char_height;
}

static void vmsvga_putcell(const tty_cell_t *cell, int col, int row)
{
	if (!_font)
		return;
	render_cell(cell, col, row);
	svga_update((unsigned)col * _font->width, (unsigned)row * _font->height,
		    _font->width, _font->height);
}

static void vmsvga_cursor_update(unsigned old_pos, unsigned new_pos,
				 const tty_cell_t *cells, unsigned cols)
{
	int old_col = (int)(old_pos % cols);
	int old_row = (int)(old_pos / cols);
	render_cell(&cells[old_pos], old_col, old_row);

	int new_col = (int)(new_pos % cols);
	int new_row = (int)(new_pos / cols);
	const tty_cell_t *nc = &cells[new_pos];

	if (nc->ch == '\0' || nc->ch == ' ')
		render_cursor_cell(new_col, new_row, ' ', VGA_COLOR_WHITE,
				   nc->bg, VGA_COLOR_WHITE);
	else
		render_cursor_cell(new_col, new_row, nc->ch, nc->fg, nc->bg,
				   VGA_COLOR_WHITE);

	unsigned min_row =
		(old_row < new_row ? (unsigned)old_row : (unsigned)new_row);
	unsigned max_row =
		(old_row > new_row ? (unsigned)old_row : (unsigned)new_row);
	svga_update(0, min_row * _font->height, _hw_resolution_x,
		    (max_row - min_row + 1) * _font->height);
}

static void vmsvga_redraw(const tty_cell_t *cells, unsigned cols, unsigned rows,
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

	svga_update(0, 0, _hw_resolution_x, _hw_resolution_y);
}

static void vmsvga_scroll_line_px(void)
{
	if (!_font)
		return;

	unsigned fh = (unsigned)_font->height;
	svga_rect_copy(0, fh, 0, 0, _hw_resolution_x, _hw_resolution_y - fh);
	svga_rect_fill(0, 0, _hw_resolution_y - fh, _hw_resolution_x, fh);
}

static void vmsvga_scroll_region_px(unsigned top_row, unsigned bot_row)
{
	if (!_font)
		return;

	unsigned fh = (unsigned)_font->height;
	svga_rect_copy(0, (top_row + 1) * fh, 0, top_row * fh, _hw_resolution_x,
		       (bot_row - top_row) * fh);
	svga_rect_fill(0, 0, bot_row * fh, _hw_resolution_x, fh);
}

static void vmsvga_insert_lines_px(unsigned row, unsigned bot_row, unsigned n)
{
	if (!_font)
		return;

	unsigned fh = (unsigned)_font->height;

	if (n >= bot_row - row + 1) {
		svga_rect_fill(0, 0, row * fh, _hw_resolution_x,
			       (bot_row - row + 1) * fh);
		return;
	}
	unsigned move_rows = bot_row - row + 1 - n;
	svga_rect_copy(0, row * fh, 0, (row + n) * fh, _hw_resolution_x,
		       move_rows * fh);
	svga_rect_fill(0, 0, row * fh, _hw_resolution_x, n * fh);
}

static void vmsvga_delete_lines_px(unsigned row, unsigned bot_row, unsigned n)
{
	if (!_font)
		return;

	unsigned fh = (unsigned)_font->height;

	if (n >= bot_row - row + 1) {
		svga_rect_fill(0, 0, row * fh, _hw_resolution_x,
			       (bot_row - row + 1) * fh);
		return;
	}
	unsigned move_rows = bot_row - row + 1 - n;
	svga_rect_copy(0, (row + n) * fh, 0, row * fh, _hw_resolution_x,
		       move_rows * fh);
	svga_rect_fill(0, 0, (bot_row - n + 1) * fh, _hw_resolution_x, n * fh);
}

static void vmsvga_clear_screen(void)
{
	svga_rect_fill(0, 0, 0, _hw_resolution_x, _hw_resolution_y);
}

static void vmsvga_cursor_erase(unsigned pos, const tty_cell_t *cells,
				unsigned cols)
{
	if (!_font)
		return;
	render_cell(&cells[pos], (int)(pos % cols), (int)(pos / cols));
	svga_update((pos % cols) * _font->width, (pos / cols) * _font->height,
		    _font->width, _font->height);
}

static void vmsvga_change_font(const char *name)
{
	const fb_font_t *f = font_get(name);
	if (f)
		_font = f;
}

static void vmsvga_sync_mode(void)
{
	unsigned width;
	unsigned height;

	width = svga_read_reg(SVGA_REG_WIDTH);
	height = svga_read_reg(SVGA_REG_HEIGHT);
	if (width > 0 && height > 0)
		vmsvga_ensure_fb_mapping(width, height);
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

static void vmsvga_flush(void)
{
	svga_update(0, 0, _hw_resolution_x, _hw_resolution_y);
	/*
	 * The periodic graphics-refresh path relies on flush() to make user-space
	 * framebuffer writes visible even when no other accelerated blit happens.
	 * An UPDATE alone is only queued in the FIFO; force a SYNC here so QEMU
	 * processes that queued update immediately instead of appearing to "wake
	 * up" only on the next unrelated input event.
	 */
	fifo_sync();
}

static int vmsvga_is_char_visible(unsigned char c)
{
	if (isprint(c))
		return 1;
	if (!_font)
		return 0;
	return (unsigned)c < (unsigned)_font->charcount;
}

/* ── PCI discovery ────────────────────────────────────────────────────────── */

typedef struct {
	uint32_t iobase;
	uint32_t fb_phys;
	uint32_t fifo_phys;
} vmsvga_pci_t;

static void vmsvga_scan_pci(uint32_t device, uint16_t v, uint16_t d,
			    void *extra)
{
	if (v == VMSVGA_VENDOR_ID && d == VMSVGA_DEVICE_ID) {
		vmsvga_pci_t *p = (vmsvga_pci_t *)extra;
		p->iobase = pci_read_field(device, PCI_BAR0, 4) & ~3u;
		p->fb_phys = pci_read_field(device, PCI_BAR1, 4) & ~15u;
		p->fifo_phys = pci_read_field(device, PCI_BAR2, 4) & ~15u;
	}
}

/* ── Probe / init ─────────────────────────────────────────────────────────── */

static int vmsvga_probe(void)
{
	vmsvga_pci_t pci = { 0, 0, 0 };
	pci_scan(vmsvga_scan_pci, -1, &pci);

	if (!pci.iobase)
		return 0;

	_iobase = (unsigned short)pci.iobase;

	svga_write_reg(SVGA_REG_ID, SVGA_ID_2);
	if (svga_read_reg(SVGA_REG_ID) != SVGA_ID_2)
		return 0;

	uint32_t fifo_size = svga_read_reg(SVGA_REG_MEM_SIZE);
	uint32_t a;
	for (a = pci.fifo_phys; a < pci.fifo_phys + fifo_size; a += PAGE_SIZE)
		mm_map_io(a);
	RELOAD_CR3();
	_fifo = (uint32_t *)pci.fifo_phys;

	_fifo[SVGA_FIFO_MIN] = 4 * sizeof(uint32_t);
	_fifo[SVGA_FIFO_MAX] = fifo_size;
	_fifo[SVGA_FIFO_NEXT_CMD] = _fifo[SVGA_FIFO_MIN];
	_fifo[SVGA_FIFO_STOP] = _fifo[SVGA_FIFO_MIN];

	svga_write_reg(SVGA_REG_WIDTH, VGA_RESOLUTION_X);
	svga_write_reg(SVGA_REG_HEIGHT, VGA_RESOLUTION_Y);
	svga_write_reg(SVGA_REG_BPP, VGA_COLOR_DEPTH);
	svga_write_reg(SVGA_REG_ENABLE, 1);
	svga_write_reg(SVGA_REG_CONFIG_DONE, 1);

	_caps = svga_read_reg(SVGA_REG_CAPABILITIES);

	uint32_t fb_size =
		VGA_RESOLUTION_X * VGA_RESOLUTION_Y * (VGA_COLOR_DEPTH / 8);
	for (a = pci.fb_phys; a < pci.fb_phys + fb_size; a += PAGE_SIZE)
		mm_map_io(a);
	RELOAD_CR3();
	_fb_phys = pci.fb_phys;
	_fb_buffer = pci.fb_phys;
	_fb_mapped_bytes = fb_size;

	memset((char *)_fb_buffer, 0, fb_size);

	_hw_resolution_x = VGA_RESOLUTION_X;
	_hw_resolution_y = VGA_RESOLUTION_Y;

	vmsvga_change_font("vga16");

	_window_char_width = _hw_resolution_x / (unsigned)_font->width;
	_window_char_height = _hw_resolution_y / (unsigned)_font->height;

	return 1;
}

/* ── Driver descriptor ────────────────────────────────────────────────────── */

const fb_drv_t vmsvga_drv = {
	.probe = vmsvga_probe,
	.get_char_dims = vmsvga_get_char_dims,
	.putcell = vmsvga_putcell,
	.redraw = vmsvga_redraw,
	.cursor_update = vmsvga_cursor_update,
	.scroll_line_px = vmsvga_scroll_line_px,
	.scroll_region_px = vmsvga_scroll_region_px,
	.insert_lines_px = vmsvga_insert_lines_px,
	.delete_lines_px = vmsvga_delete_lines_px,
	.clear_screen = vmsvga_clear_screen,
	.change_font = vmsvga_change_font,
	.sync_mode = vmsvga_sync_mode,
	.flush = vmsvga_flush,
	.get_phys_window = vmsvga_get_phys_window,
	.snapshot_size = vmsvga_snapshot_size,
	.snapshot_save = vmsvga_snapshot_save,
	.snapshot_restore = vmsvga_snapshot_restore,
	.is_char_visible = vmsvga_is_char_visible,
	.cursor_erase = vmsvga_cursor_erase,
};
