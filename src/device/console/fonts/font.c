#include <device/console/render.h>
#include <lib/klib.h>

extern const console_font font_vga16;

const console_font *font_get(const char *name)
{
	return name && !strcmp(name, font_vga16.name) ? &font_vga16 : NULL;
}
