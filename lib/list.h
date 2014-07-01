#ifndef _LIST_H_
#define _LIST_H_

#ifdef WIN32
#include <Windows.h>
#else
typedef struct _LIST_ENTRY {
	struct _LIST_ENTRY  *Flink;
	struct _LIST_ENTRY  *Blink; 
} LIST_ENTRY, *PLIST_ENTRY;
#endif

void InitializeListHead(PLIST_ENTRY ListHead);

int IsListEmpty(LIST_ENTRY * ListHead);

PLIST_ENTRY RemoveHeadList(PLIST_ENTRY ListHead);

PLIST_ENTRY RemoveTailList(PLIST_ENTRY ListHead);

void InsertTailList(PLIST_ENTRY ListHead,PLIST_ENTRY Entry);

void InsertHeadList(PLIST_ENTRY ListHead,PLIST_ENTRY Entry);

void AppendTailList(PLIST_ENTRY ListHead,PLIST_ENTRY ListToAppend);

#define OFFSET_OF(type, field) \
	(unsigned long)(&(((type*)0)->field))

#define CONTAINER_OF( node, type, field ) \
	(type*)( (char *)node - OFFSET_OF(type, field)   )


typedef struct _STACK {
	unsigned int top;
	unsigned int count;
	//unsigned int mem[1024];
}STACK, *PSTACK;

void InitializeStack(PSTACK stack);

int PushStack(PSTACK stack, unsigned int val);

unsigned int PopStack(PSTACK stack);

#endif
