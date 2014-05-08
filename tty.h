#ifndef _TTY_H_
#define _TTY_H_
#include "mm.h"

#define TTY_MAX_ROW 25
#define TTY_MAX_COL 80
#define TTY_MAX_CHARS (TTY_MAX_ROW * TTY_MAX_COL)
#define ROW_COL_TO_CUR(row, col) \
	(row * TTY_MAX_COL + col)


typedef enum _TTY_COLOR
{ 
	clBlack , 
	clBlue , 
	clGreen , 
	clCyan , 
	clRed , 
	clMagenta ,
	clBrown , 
	clLightGray , 
	clDarkGray , 
	clLightBlue , 
	clLightGreen ,
	clLightCyan , 
	clLightRed , 
	clLightMagenta , 
	clYellow , 
	clWhite 
}TTY_COLOR;

void tty_init(void);

void tty_setcolor(int x, int y, TTY_COLOR front, TTY_COLOR back);

TTY_COLOR tty_get_frontcolor(int x, int y);

TTY_COLOR tty_get_backcolor(int x, int Y);

void tty_putchar(int x, int y, char c);

char tty_getchar(int x, int y);

void tty_roll_one_line();

void tty_clear();

#endif
