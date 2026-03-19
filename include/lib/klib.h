#ifndef _LIB_KLIB_H
#define _LIB_KLIB_H
#include <mm/mm.h>
#include <config.h>

/* ── Heap ─────────────────────────────────────────────────────────────────── */

#define NULL (void *)0

typedef struct _kblock {
	unsigned int size;
	struct _kblock *next;
} kblock;

typedef char *va_list;

#define _INTSIZEOF(n) ((sizeof(n) + sizeof(int) - 1) & ~(sizeof(int) - 1))

#define va_start(ap, v) (ap = (va_list) & v + _INTSIZEOF(v))

#define va_arg(ap, t) (*(t *)((ap += _INTSIZEOF(t)) - _INTSIZEOF(t)))

#define va_end(ap) (ap = (va_list)0)

#define kmalloc(size) malloc(size)
#define kfree(p) free(p)
#define kzalloc(size) zalloc(size)

#define klib_srand srand
#define klib_rand rand

void *malloc(unsigned int size);
void free(void *buf);
void *zalloc(unsigned size);

/* ── Init ─────────────────────────────────────────────────────────────────── */

void klib_init(void);

/* ── Logging ──────────────────────────────────────────────────────────────── */

void klog(char *str, ...);
void klog_close(void);

/* ── Formatted output ─────────────────────────────────────────────────────── */

void printk(const char *str, ...);

void vprintf(const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
void printf(const char *str, ...);
int sprintf(char *buf, const char *fmt, ...);

/* ── String / memory ──────────────────────────────────────────────────────── */

void memcpy(void *dst, void *src, unsigned len);
void memmove(void *dst, void *src, unsigned len);
int memcmp(void *src, void *dst, unsigned len);
void memset(void *src, char val, int len);

unsigned strlen(const char *str);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, int len);
char *strstr(const char *src, const char *str);
char *strrev(char *src);
int strcmp(char *str, char *dst);
int strncmp(char *s1, char *s2, int n);
char *strcat(char *str, char *msg);
char *strchr(char *str, char c);
char *strrchr(char *str, char c);
char *strdup(char *str);

/* ── Character / number ───────────────────────────────────────────────────── */

int isspace(const char c);
int isprint(int c);
int tolower(int c);
int toupper(int c);
int islower(int c);
int isupper(int c);
char *itoa(int num, int base, int sign);
int atoi(const char *str);
void srand(unsigned _seed);

/* ── Misc ─────────────────────────────────────────────────────────────────── */

typedef struct _TEST_CONTROL {
	int verbos;
	int profiling;
	const char *init_binary;
} TEST_CONTROL;
extern TEST_CONTROL TestControl;

#endif
