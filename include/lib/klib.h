#ifndef _LIB_KLIB_H
#define _LIB_KLIB_H
#include <mm/mm.h>
#include <config.h>

/* ── Heap ─────────────────────────────────────────────────────────────────── */

#ifndef NULL
#define NULL (void *)0
#endif

typedef struct _kblock {
	unsigned int size;
	struct _kblock *next;
} kblock;

#ifndef _STDARG_H
typedef __builtin_va_list va_list;

#define va_start(ap, v) __builtin_va_start(ap, v)
#define va_arg(ap, t) __builtin_va_arg(ap, t)
#define va_end(ap) __builtin_va_end(ap)
#endif /* _STDARG_H */

#define kmalloc(size) malloc(size)
#define kfree(p) free(p)
#define kzalloc(size) zalloc(size)

#define klib_srand srand
#define klib_rand rand

void *malloc(unsigned int size);
void free(void *buf);
void *zalloc(unsigned size);
extern unsigned int heap_quota; /* current live-allocation byte count */

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

void memcpy(void *dst, const void *src, unsigned len);
void memmove(void *dst, void *src, unsigned len);
int memcmp(const void *src, const void *dst, unsigned len);
void memset(void *src, char val, int len);

unsigned strlen(const char *str);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, int len);
char *strstr(const char *src, const char *str);
char *strrev(char *src);
int strcmp(const char *str, const char *dst);
int strncmp(const char *s1, const char *s2, int n);
char *strcat(char *str, const char *msg);
char *strchr(const char *str, char c);
char *strrchr(const char *str, char c);
char *strdup(const char *str);
const char *strglob(const char *pat, const char *str);

/* ── Character / number ───────────────────────────────────────────────────── */

int isspace(const char c);
int isprint(int c);
int tolower(int c);
int toupper(int c);
int islower(int c);
int isupper(int c);
char *itoa(int num, int base, int sign);
char *lltoa(long long num, int base, int sign);
int atoi(const char *str);
void srand(unsigned _seed);
unsigned int rand(void);

/* ── Misc ─────────────────────────────────────────────────────────────────── */

typedef struct _TEST_CONTROL {
	int verbos; /* verbose level: 0=off, 1=trace, 2=info */
	int profiling;
	int bash;
	int test;
} TEST_CONTROL;
extern TEST_CONTROL TestControl;

#define TEST_LOG_OFF 0
#define TEST_LOG_TRACE 1
#define TEST_LOG_INFO 2

#define TEST_LOG(level) (TestControl.verbos >= (level))

#endif
