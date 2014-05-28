#ifndef _KLIB_H_
#define _KLIB_H_

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

#ifdef __KERNEL__

#include <mm/mm.h>
#include <config.h>

void klib_init();

void klib_putchar(char c);

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


#else

#include <user/syscall.h>

#define PAGE_SIZE (4*1024)

void libc_init();

void libc_init();

void libc_putchar(char c);

void libc_print(char *str);

#endif

void memcpy(void* dst, void* src, unsigned len);

void memmove(void* dst, void* src, unsigned len);

int memcmp(void* src, void* dst, unsigned len);

void memset(void* src, char val, int len);

int isspace(const char c);
 
unsigned strlen(const char* str); 

char* strcpy(char* dst, const char* src);

char* strstr(const char* src, const char* str);

char* strrev(char *src);

int strcmp(char* str, char* dst);

char* strcat(char* str, char* msg);

char* strchr(char* str, char c);

char* strrchr(char* str, char c);

char* strdup(char* str);

void printf(const char* str, ...);

void vprintf(const char* str, va_list ap);

void print_human_readable_size (unsigned int size) ;

char* itoa(int num, int base, int sign);

int tolower(int c);

int toupper(int c);

int islower(int c);

int isupper(int c);

void srand(unsigned _seed);

#endif
