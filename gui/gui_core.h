#ifndef _GUI_CORE_H_
#define _GUI_CORE_H_

#define MAX_X _hw_resolution_x
#define MAX_Y _hw_resolution_y

void gui_fill_rectangle(int x, int y, int width, int height, unsigned color);

// this is very slow because of float point number is slow
void gui_fill_rectangle_alpha(int x, int y, int width, int height, unsigned color, double alpha);

void gui_fill_rectange_gradually(int x, int y, int width, int height, unsigned from, unsigned to);

int gui_fill_picture(const char* path);

#endif
