#include <gui/gui_core.h>
#include <drivers/vga.h>
#include <fs/namespace.h>
#include <syscall/unistd.h>


static unsigned gui_alpha_color(int x, int y, unsigned color, double alpha)
{
    unsigned* buffer = (unsigned*)_fb_buffer;
    unsigned bc;
    unsigned ra, ga, ba, rb, gb, bb;

    bc = buffer[y*MAX_X + x];
    ra = _RED(bc);
    ga = _GRE(bc);
    ba = _BLU(bc);

    color = ( ((color << 8) >> 8) & 0x00FFFFFF);
    rb = _RED(color);
    gb = _GRE(color);
    bb = _BLU(color);


    rb = alpha*rb + (1-alpha)*ra;
    gb = alpha*gb + (1-alpha)*ga;
    bb = alpha*bb + (1-alpha)*ba;
    color = ARGB(0, rb, gb, bb);
    return color;
}

static unsigned gui_gradually_color(unsigned from, unsigned to, int height, int current)
{
    unsigned ra, ga, ba, rb, gb, bb;
    unsigned rs, gs, bs;
    double per;
    ra = _RED(from);
    ga = _GRE(from);
    ba = _BLU(from);
    rb = _RED(to);
    gb = _GRE(to);
    bb = _BLU(to);

    rs = (ra > rb) ? (ra - rb) : (rb - ra);
    gs = (ga > gb) ? (ga - gb) : (gb - ga);
    bs = (ba > bb) ? (ba - bb) : (bb - ba);

    per = (double)current / (double)height;
    rs =(unsigned)((double)rs * per);
    gs =(unsigned)((double)gs * per);
    bs =(unsigned)((double)bs * per);

    ra = (ra > rb) ? (ra - rs) : (ra + rs);
    ga = (ga > gb) ? (ga - gs) : (ga + gs);
    ba = (ba > bb) ? (ba - bs) : (ba + bs);

    return ARGB(0, ra, ga, ba);
}

void gui_fill_rectangle(int x, int y, int width, int height, unsigned color)
{
    int i, j;

    if ( x >= MAX_X || y >= MAX_Y) {
        return;
    }

    if ((x + width) >= MAX_X) {
        width = MAX_X - x - 1;
    }

    if ((y + height) >= MAX_Y) {
        height = MAX_Y - y - 1;
    }


    for (j = y; j < (y+height); j++) {
        for (i = x; i < (x+width); i++) {
            fb_set_point(i,j,color);
        }
    }

}

void gui_fill_rectangle_alpha(int x, int y, int width, int height, unsigned color, double alpha)
{
    int i, j;

    if ( x >= MAX_X || y >= MAX_Y) {
        return;
    }

    if ((x + width) >= MAX_X) {
        width = MAX_X - x - 1;
    }

    if ((y + height) >= MAX_Y) {
        height = MAX_Y - y - 1;
    }


    for (j = y; j < (y+height); j++) {
        for (i = x; i < (x+width); i++) {
            unsigned c = gui_alpha_color(i, j,color,alpha);
            fb_set_point(i,j,c);
        }
    }
}

void gui_fill_rectange_gradually(int x, int y, int width, int height, unsigned from, unsigned to)
{
    int i, j;

    if ( x >= MAX_X || y >= MAX_Y) {
        return;
    }

    if ((x + width) >= MAX_X) {
        width = MAX_X - x - 1;
    }

    if ((y + height) >= MAX_Y) {
        height = MAX_Y - y - 1;
    }


    for (j = y; j < (y+height); j++) {
        for (i = x; i < (x+width); i++) {
            unsigned c = gui_gradually_color(from,to,height, (j-y));
            fb_set_point(i,j,c);
        }
    }
}


static int load_bmp(const char * filename) {
	/* Open the requested binary */
    int fd, ret;
    struct stat s;
	unsigned image_size= 0;
    unsigned width;
    unsigned height;
    unsigned short bpp;
    unsigned row_width;
    char* bufferb;
    unsigned short x, y;
    unsigned int *bufferi;
    unsigned off_x, off_y;
    unsigned i;
    unsigned size;
    unsigned data_off;

    ret = 0;
    fd = fs_open(filename);
    if (fd == -1) {
        return 0;
    }

    fs_stat(filename, &s);
    image_size = s.st_size;

    	/* Alright, we have the length */
	bufferb = do_mmap(0, image_size, 0, 0,fd, 0);

	x = 0; /* -> 212 */
	y = 0; /* -> 68 */
	/* Get the width / height of the image */
    if (bufferb[0] != 0x42 || bufferb[1] != 0x4d) {
        goto done;
    }

    size = *((unsigned*)(bufferb + 0x2));
    if (size > image_size) {
        goto done;
    }

    data_off = *((unsigned*)(bufferb + 0xa));
    width = *((unsigned*)(bufferb + 0x12));
    height = *((unsigned*)(bufferb + 0x16));
    bpp = *((unsigned short*)(bufferb + 0x1C));

    if (width > _hw_resolution_x || height > _hw_resolution_y) {
        goto done;
    }

    off_x = (_hw_resolution_x - width) / 2;
    off_y = (_hw_resolution_y - height) / 2;

    row_width = (bpp * width + 31) / 32 * 4;
	/* Skip right to the important part */
	i = data_off;

	for (y = 0; y < height; ++y) {
		for (x = 0; x < width; ++x) {
            unsigned color;
			if (i > image_size)
            {
                goto done;
            }
			/* Extract the color */
			if (bpp == 24) {
				color =	(bufferb[i   + 3 * x] & 0xFF) +
						(bufferb[i+1 + 3 * x] & 0xFF) * 0x100 +
						(bufferb[i+2 + 3 * x] & 0xFF) * 0x10000;
			} else if (bpp == 32) {
				color =	(bufferb[i   + 4 * x] & 0xFF) * 0x1000000 +
						(bufferb[i+1 + 4 * x] & 0xFF) * 0x100 +
						(bufferb[i+2 + 4 * x] & 0xFF) * 0x10000 +
						(bufferb[i+3 + 4 * x] & 0xFF) * 0x1;
			}
			/* Set our point */
			fb_set_point(_hw_resolution_x - (off_x + x) - 1, _hw_resolution_y -  (off_y + y) - 1, color);
		}
		i += row_width;
	}
    ret = 1;
done:
	fs_close(fd);
	do_munmap(bufferb, image_size);

    return ret;
}

int gui_fill_picture(const char* path)
{
    // FIXME
    // no scale this time, put picture in middle
    // if too large, just fail
    return load_bmp(path);
}
