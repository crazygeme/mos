#include <gui/gui_test.h>
#include <gui/gui_core.h>
#include <drivers/vga.h>
#include <drivers/tty.h>

static void gui_test_draw_shell()
{
    unsigned width = 800;
    unsigned height = 600;
    unsigned x_off, y_off;

    x_off = MAX_X - width - 20;
    y_off = 40;
    gui_fill_rectange_gradually(x_off, y_off - 20, width, 20, VGA_COLOR_BLACK, VGA_COLOR_GRAY);
    gui_fill_rectangle_alpha(x_off, y_off, width, height, VGA_COLOR_BLACK, 0.8);

    _fb_x_off = x_off;
    _fb_y_off = y_off;
    _resolution_x = width;
    _resolution_y = height;
    _window_char_width = _resolution_x / char_width;
    _window_char_height = _resolution_y / char_height;
    TTY_MAX_ROW = _window_char_height;
    TTY_MAX_COL = _window_char_width;
}

static void gui_test_draw_panel()
{
    gui_fill_rectange_gradually(0, MAX_Y - 40, MAX_X , 20, VGA_COLOR_GRAY, 0x00666666);
    gui_fill_rectange_gradually(0, MAX_Y - 20, MAX_X , 20, 0x00666666, VGA_COLOR_GRAY);
}

static void gui_test_draw_desktop()
{
    if (!gui_fill_picture("/bin/screen.bmp")) {
        gui_fill_rectangle(0, 0, MAX_X, MAX_Y, VGA_COLOR_BLUE); 
    }

}

void gui_test()
{
    gui_test_draw_desktop();

    gui_test_draw_shell();

    gui_test_draw_panel();
}
