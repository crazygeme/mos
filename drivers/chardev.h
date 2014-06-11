#ifndef _CHARDEV_H_
#define _CHARDEV_H_
#include <lib/list.h>


typedef int (*fpchar_read)(void* aux, void* buf, unsigned len);

typedef int (*fpchar_write)(void* aux, void* buf, unsigned len);

typedef void (*fpchar_close)(void* aux);


typedef struct _chardev
{
	unsigned int id;
	char name[16];
	void* aux;
	fpchar_read read;
	fpchar_write write;
	fpchar_close close;
	LIST_ENTRY char_list;
}chardev;


void chardev_init();

chardev* chardev_register(void* aux, char* name, fpchar_read read, fpchar_write write, fpchar_close close);

chardev* chardev_get_by_id(unsigned int id);

void chardev_close();

#endif
