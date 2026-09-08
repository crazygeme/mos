#ifndef _HW_FONT_H_
#define _HW_FONT_H_

#include <lib/list.h>

typedef struct {
	const char *name;
	int width;
	int height;
	int charcount;
	const unsigned char *
		glyphs; /* indexed by char value, row-major: [ch * height + row] */
	const unsigned char
		*cursor_glyphs; /* index 0 = cursor block, 1 = blank */
	list_entry node;
} fb_font_t;

/* Add default fonts at very beginning */
void font_init();

void font_add(const fb_font_t *font);

const fb_font_t *font_get(const char *name);

#endif /* _HW_FONT_H_ */
