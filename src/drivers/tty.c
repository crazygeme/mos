
/*
 * Copyright (C) 2014  Ender Zheng
 * License: GPL version 2 or higher http://www.gnu.org/licenses/gpl.html
 */
#include <tty.h>
#include <vga.h>
#include <unistd.h>
#include <ioctl.h>

static void tty_copy_row(int src, int dst);
static void tty_clear_row(int row);
static char *vidptr = (char*)0xC00b8000;
unsigned TTY_MAX_ROW;
unsigned TTY_MAX_COL;
static int vga_enabled;
static int cursor;

void tty_init(void)
{
    int i = 0;
    int j = 0;

    cursor = 0;
    if (_fb_buffer)
    {
        vidptr = _fb_buffer;
        TTY_MAX_ROW = _window_char_height;
        TTY_MAX_COL = _window_char_width;
        vga_enabled = 1;
    }
    else
    {
        TTY_MAX_ROW = 25;
        TTY_MAX_COL = 80;
        vga_enabled = 0;
    }

    for (i = 0; i < TTY_MAX_ROW; i++)
    {
        for (j = 0; j < TTY_MAX_COL; j++)
        {
            tty_putchar(i, j, ' ');
        }
    }

    //tty_movecurse(0);
}

void tty_setcolor(int x, int y, TTY_COLOR front, TTY_COLOR back)
{
    int color_field = (back << 4) | (front);
    int cur = ROW_COL_TO_CUR(x, y);

    if (x < 0 || x >= TTY_MAX_ROW ||
        y < 0 || y >= TTY_MAX_COL)
        return;

    if (!vga_enabled)
    {
        vidptr[cur * 2 + 1] = color_field;
    }
    else
    {
        // FIXME : only black color in text mode
        fb_write_color(y, x, VGA_COLOR_BLACK);
    }
}

TTY_COLOR tty_get_frontcolor(int x, int y)
{
    int cur = ROW_COL_TO_CUR(x, y);
    int color_field = 0;

    if (x < 0 || x >= TTY_MAX_ROW ||
        y < 0 || y >= TTY_MAX_COL)
        return clBlack;

    if (!vga_enabled)
    {
        color_field = vidptr[cur * 2 + 1];
        return (color_field % 8);
    }
    else
    {
        // FIXME : only white color in text mode
        return clWhite;
    }
}

TTY_COLOR tty_get_backcolor(int x, int y)
{
    int cur = ROW_COL_TO_CUR(x, y);
    int color_field = 0;

    if (x < 0 || x >= TTY_MAX_ROW ||
        y < 0 || y >= TTY_MAX_COL)
        return clBlack;

    if (!vga_enabled)
    {
        color_field = vidptr[cur * 2 + 1];
        return (color_field >> 4);
    }
    else
    {
        // FIXME : only black color in text mode
        return clBlack;
    }

}

void tty_putchar(int x, int y, char c)
{
    if (!vga_enabled)
    {
        int cur = ROW_COL_TO_CUR(x, y);
        if (x < 0 || x >= TTY_MAX_ROW ||
            y < 0 || y >= TTY_MAX_COL)
            return;

        vidptr[cur * 2] = c;

    }
    else
    {
        // FIXME : only white color in text mode
        int cur = ROW_COL_TO_CUR(x, y);
        _fb_text[cur] = c;
        fb_write_char(y, x, c, VGA_COLOR_GREEN);
    }

}

char tty_getchar(int x, int y)
{
    if (!vga_enabled)
    {
        int cur = ROW_COL_TO_CUR(x, y);
        if (x < 0 || x >= TTY_MAX_ROW ||
            y < 0 || y >= TTY_MAX_COL)
            return ' ';

        return vidptr[cur * 2];
    }
    else
    {
        int cur = ROW_COL_TO_CUR(x, y);
        return _fb_text[cur];
    }
}
#define CUR_ROW (cursor / TTY_MAX_COL)
#define CUR_COL (cursor % TTY_MAX_COL)

void tty_roll_one_line()
{
    if (!vga_enabled || (_resolution_x != _hw_resolution_x) || (_resolution_y != _hw_resolution_y))
    {
        char* dst = vidptr;
        char* src = dst + TTY_MAX_COL * 2;
        unsigned len = TTY_MAX_COL * (TTY_MAX_ROW - 1) * 2;
        memmove(dst, src, len);
        tty_clear_row(TTY_MAX_ROW - 1);
    }
    else
    {
        unsigned next_row = _fb_buffer + VGA_RESOLUTION_X * (VGA_COLOR_DEPTH / 8) * _fb_font_height;
        unsigned copy_size = VGA_RESOLUTION_X * (VGA_RESOLUTION_Y - _fb_font_height) * (VGA_COLOR_DEPTH / 8);
        char* new_row_ptr = (char*)next_row;
        char* old_row_ptr = (char*)_fb_buffer;
        char* last_row = (char*)(_fb_buffer + copy_size);
        unsigned row_pixel_size = VGA_RESOLUTION_X * _fb_font_height * (VGA_COLOR_DEPTH / 8);

        char* next_char_row = _fb_text + _window_char_width;
        unsigned txt_copy_size = _window_char_width * (_window_char_height - 1);
        char* last_char_row = _fb_text + _window_char_width * (_window_char_height - 1);


        // first clear cursor
        {
            char val = _fb_text[cursor];

            if (val == ' ' || val == '\0' || val == '\n' || val == '\r' || val == '\t' || val == '\b')
            {
                fb_write_char(CUR_COL, CUR_ROW, 130, VGA_COLOR_BLACK);
            }
        }
        memcpy(old_row_ptr, new_row_ptr, copy_size);
        // then paint cursor back
        {
            fb_write_char(CUR_COL, CUR_ROW, 129, VGA_COLOR_GREEN);
        }

        memcpy(_fb_text, next_char_row, txt_copy_size);
        memset(last_char_row, ' ', _window_char_width);
        // FIXME
        // assume it's BLACK background color
        memset(last_row, 0, row_pixel_size);

    }
}

static void tty_copy_row(int src, int dst)
{
    int col = 0;
    TTY_COLOR fg, bg;
    for (col = 0; col < TTY_MAX_COL; col++)
    {
        tty_putchar(dst, col, tty_getchar(src, col));
    }
}

static void tty_clear_row(int row)
{
    int col = 0;

    for (col = 0; col < TTY_MAX_COL; col++)
    {
        tty_putchar(row, col, ' ');
    }
}

void tty_clear()
{
    int row = 0;
    char* src = vidptr;
    unsigned len = TTY_MAX_COL * 2;
    if (!vga_enabled)
    {
        tty_clear_row(0);
        for (row = 1; row < TTY_MAX_ROW; row++)
        {
            char* dst = src + row * TTY_MAX_COL * 2;
            memcpy(dst, src, len);
        }
    }
    else
    {
        src = _fb_buffer;
        len = VGA_RESOLUTION_X * VGA_RESOLUTION_Y * (VGA_COLOR_DEPTH / 8);
        memset(src, 0, len);
    }
}


void tty_movecurse(unsigned c)
{

    if (!vga_enabled)
    {
        unsigned short cp = (unsigned short)c;
        _write_word(0x3d4, 0x0e | (cp & 0xff00));
        _write_word(0x3d4, 0x0f | (cp << 8));
    }
    else
    {
        int row = CUR_ROW;
        int col = CUR_COL;
        char val = _fb_text[cursor];

        if (val == ' ' || val == '\0' || val == '\n' || val == '\r' || val == '\t')
        {
            fb_write_char(CUR_COL, CUR_ROW, 130, VGA_COLOR_BLACK);
        }
        cursor = c;
        fb_write_char(CUR_COL, CUR_ROW, 129, VGA_COLOR_GREEN);
    }
}

int tty_ioctl(void* inode, unsigned cmd, void* buf)
{
    switch(cmd) {
        case TCGETS: {
            struct termios s = {ICRNL,        /* change incoming CR to NL */
                                OPOST | ONLCR,    /* change outgoing NL to CRNL */
                                B38400 | CS8,
                                IXON | ISIG | ICANON | ECHO | ECHOCTL | ECHOKE,
                                0,        /* console termio */
                                INIT_C_CC};
            memcpy(buf, &s, sizeof(s));
            return 0;
        }
        case TCSETS: {
            // FIXME
            return 0;
        }
        case TIOCGWINSZ: {
            struct winsize* size = (struct winsize*)buf;
            size->ws_row = TTY_MAX_ROW;
            size->ws_col = TTY_MAX_COL;
            size->ws_xpixel = size->ws_ypixel = 0;
            return 0;
        }
    }
}
