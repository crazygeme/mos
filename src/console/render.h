#ifndef MOS_CONSOLE_RENDER_H
#define MOS_CONSOLE_RENDER_H

typedef struct {
	char ch;
	unsigned fg;
	unsigned bg;
} console_cell;

void console_get_char_dims(unsigned *cols, unsigned *rows);
void console_putcell(const console_cell *cell, int col, int row);
void console_redraw(const console_cell *cells, unsigned cols, unsigned rows,
		    unsigned cursor_pos);
void console_cursor_update(unsigned old_pos, unsigned new_pos,
			   const console_cell *cells, unsigned cols);
void console_scroll_line(void);
void console_scroll_region(unsigned top_row, unsigned bot_row);
void console_insert_lines(unsigned row, unsigned bot_row, unsigned n);
void console_delete_lines(unsigned row, unsigned bot_row, unsigned n);
void console_clear_screen(void);
void console_change_font(const char *name);
void console_sync_mode(void);
void console_present(void);
int console_is_char_visible(unsigned char c);
void console_cursor_erase(unsigned pos, const console_cell *cells,
			  unsigned cols);

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

typedef struct {
	const char *name;
	int width;
	int height;
	int charcount;
	const unsigned char *
		glyphs; /* indexed by char value, row-major: [ch * height + row] */
	const unsigned char
		*cursor_glyphs; /* index 0 = cursor block, 1 = blank */
} console_font;

const console_font *font_get(const char *name);

#endif
