#ifndef _LIB_TIMER_H
#define _LIB_TIMER_H
#include <lib/rbtree.h>

typedef struct timer_t timer_t;

typedef void (*timeout_t)(timer_t *timer, void *ctx);

void timer_init();

void do_timer_loop();

timer_t *timer_start(timeout_t callback, unsigned period, int repeatly,
		     void *ctx);

void timer_stop(timer_t *timer);

#endif