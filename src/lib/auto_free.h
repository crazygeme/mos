#ifndef MOS_LIB_AUTO_FREE_H
#define MOS_LIB_AUTO_FREE_H
#include <lib/klib.h>

/* Scoped heap ownership includes every ordinary return and goto exit. */
static inline void auto_free_pointer(void *address)
{
	void *pointer;
	memcpy(&pointer, address, sizeof(pointer));
	free(pointer);
}
#define AUTO_FREE __attribute__((cleanup(auto_free_pointer)))
#endif
