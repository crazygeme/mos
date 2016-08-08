#ifndef _GUI_TOOLKIT_H_
#define _GUI_TOOLKIT_H_

typedef enum _gui_event
{
    WM_CREATE,
    WM_DESTROY,
    WM_DRAW, //draw to draw_buffer
    WM_MOVE
}gui_event;

typedef enum _gui_event_result
{
    WS_SUCCESS,
    WS_UNHANDLE
}gui_event_result;

typedef struct _gui_base* gui_base_t;
typedef struct _gui_panel* gui_panel_t;
typedef struct _gui_window* gui_window_t;
typedef struct _gui_button* gui_button_t;

typedef gui_event_result (*fp_gui_event)(gui_base_t base, gui_event e, void* param);

void gui_toolkit_init(void* hw_buffer, unsigned hw_size, int width, int height, unsigned bg_color, char* background);

gui_panel_t gui_create_panel(gui_base_t parent, int x, int y, int width, int height, unsigned color);

void gui_refresh();

#endif
