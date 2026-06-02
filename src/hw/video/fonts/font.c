#include <lib/list.h>
#include <lib/klib.h>
#include <hw/font.h>

/* Add default fonts at very beginning */
extern fb_font_t font_vga16;
list_entry font_list;

void font_init()
{
	list_init(&font_list);
	font_add(&font_vga16);
}

void font_add(const fb_font_t *font)
{
	list_insert_tail(&font_list, (list_entry *)&font->node);
}

const fb_font_t *font_get(const char *name)
{
	list_entry *e;
	for (e = font_list.next; e != &font_list; e = e->next) {
		fb_font_t *f = container_of(e, fb_font_t, node);
		if (strcmp(f->name, name) == 0)
			return f;
	}
	return NULL;
}