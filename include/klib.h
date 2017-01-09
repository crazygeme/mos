#ifndef _KLIB_H_
#define _KLIB_H_
#include <mm.h>
#include <config.h>

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

/// TTY 
typedef enum _tty_command_id
{
	tty_cmd_get_color,
	tty_cmd_set_color,
	tty_cmd_get_curse,
	tty_cmd_set_curse,
	tty_cmd_is_tty
}tty_command_id;

typedef struct _tty_command
{
	tty_command_id cmd_id;
	union{
		struct{
			TTY_COLOR fg_color;
			TTY_COLOR bg_color;
		} color;

		struct{
			unsigned cx;
			unsigned cy;
		}curse;

		struct{
			unsigned fd;
			int is_tty;
		};
	};
}tty_command;

/// heap
#define NULL (void*)0

typedef struct _kblock
{
	unsigned int size;
	struct _kblock *next;
}kblock;

typedef char * va_list;

#define _INTSIZEOF(n) ( (sizeof(n) + sizeof(int) - 1) & ~(sizeof(int) - 1) )

#define va_start(ap,v) ( ap = (va_list)&v + _INTSIZEOF(v) )

#define va_arg(ap,t) ( *(t *)((ap += _INTSIZEOF(t)) - _INTSIZEOF(t)) )

#define va_end(ap) ( ap = (va_list)0 )

void* malloc(unsigned int size);

void free(void* buf);

/// logs

void klib_init();

void klog_init();

void klog_write(char c);

void klog_enable();

void klog_printf(const char* str, ...);

void klog(char* str, ...);

void klog_close();

int klib_putchar(char c);

void klib_update_cursor(int pos);

void klib_flush_cursor();

void klib_putchar_update_cursor(char c);

void klib_putint(int num);

void klib_print(char *str);

void klib_info(char *info, int num, char* end);

void klib_clear();

char* klib_itoa(char* str, int num);

void tty_write(const char* str, unsigned len);


#define kmalloc(size) malloc(size)

#define kfree(p) free(p)

#define klib_srand srand
#define klib_rand rand

void klogquota();

// misc
//
void shutdown();

void printk(const char* str, ...);


typedef struct _TEST_CONTROL
{
    int test_enable;
    int test_all;
    int test_mmap;
    int test_mm;
    int test_fs_read;
    int test_fs_write;
    int test_ffs;
    int test_ext2;
	int test_mount;
	int test_block;
	int verbos;
}TEST_CONTROL;
extern TEST_CONTROL TestControl;

void memcpy(void* dst, void* src, unsigned len);

void memmove(void* dst, void* src, unsigned len);

int memcmp(void* src, void* dst, unsigned len);

void memset(void* src, char val, int len);

int isspace(const char c);
 
unsigned strlen(const char* str); 

char* strcpy(char* dst, const char* src);

char* strncpy(char* dst, const char* src, int len);

char* strstr(const char* src, const char* str);

char* strrev(char *src);

int strcmp(char* str, char* dst);

int strncmp(char* s1, char* s2, int n);

char* strcat(char* str, char* msg);

char* strchr(char* str, char c);

char* strrchr(char* str, char c);

char* strdup(char* str);

void printf(const char* str, ...);


typedef void (*fpputc)(unsigned char c);
typedef void (*fputstr)(char* str);

void vprintf(fpputc _putc, fputstr _putstr, const char* str, va_list ap);


char* itoa(int num, int base, int sign);

int atoi(const char *str);

int tolower(int c);

int toupper(int c);

int islower(int c);

int isupper(int c);

int isprint(int c);

void srand(unsigned _seed);


#endif
