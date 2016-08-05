#include <gui/gui_toolkit.h>
#include <lib/list.h>
#include <lib/klib.h>

struct _gui_base
{
    int x;
    int y;
    int width;
    int height;
    unsigned color;
    void draw_buffer;
    LIST_ENTRY self;
    LIST_ENTRY children;
    gui_base_t parent;
    fp_gui_event on_event;
};

static void gui_base_init(gui_base_t base, void* fb_buffer,
    int x, int y, int width, int height,
    gui_base_t parent, unsigned color,
    fp_gui_event onevent)
{

    base->x = x;
    base->y = y;
    base->width = width;
    base->height = height;
    base->color = color;
    base->draw_buffer = fb_buffer;
    InitializeListHead(&base->self);
    InitializeListHead(&base->children);
    base->parent = parent;
    base->on_event = onevent;
    return;
}

static void gui_draw(gui_base_t base)
{
    LIST_ENTRY* child = 0;
    LIST_ENTRY* children = &base->children;

    if (base && base->on_event)
    {
        base->on_event(base, WM_DRAW, 0);
    }

    child = children->Flink;
    while (child != children)
    {
        gui_base_t c = CONTAINER_OF(child, struct _gui_base, self);
        gui_draw(c);
        child = child->Flink;
    }

}

static gui_base_t desktop;
static void* hw_buffer;
static void* background;
static unsigned hw_buffer_size;

static gui_event_result gui_topmost_event(gui_base_t base, gui_event e, void* param)
{
    gui_event_result result = WS_UNHANDLE;
    switch (e)
    {
    case WM_DRAW:
        memcpy(base->draw_buffer, background, hw_buffer_size);
        break;
    default:
        break;
    }

    return result;
}

void gui_toolkit_init(void* hw_lfb, unsigned hw_size, int width, int height, unsigned bg_color, char* back_pickure)
{
    unsigned draw_buffer_size = hw_size / PAGE_SIZE + 1;
    void* draw_buffer = vm_alloc(draw_buffer_size);
    hw_buffer = hw_lfb;
    hw_buffer_size = hw_size;
    desktop = kmalloc(sizeof(*desktop));
    gui_base_init(desktop, draw_buffe, 0, 0, width, height, 0, bg_color, gui_topmost_event);

    // load background if has, orelse fill it with color


    // 
}

void gui_refresh()
{

}
