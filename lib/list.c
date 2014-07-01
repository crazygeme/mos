#include <lib/list.h>
#include <mm/mm.h>
#include <config.h>

void InitializeListHead(PLIST_ENTRY ListHead)
{

	ListHead->Flink = ListHead->Blink = ListHead;
	return;
}


int IsListEmpty(LIST_ENTRY * ListHead)
{
	return (int)(ListHead->Flink == ListHead);
}

int RemoveEntryList(PLIST_ENTRY Entry)
{
	PLIST_ENTRY Blink;
	PLIST_ENTRY Flink;
	Flink = Entry->Flink;
	Blink = Entry->Blink;
	Blink->Flink = Flink;
	Flink->Blink = Blink;
	return (int)(Flink == Blink);
}

PLIST_ENTRY RemoveHeadList(PLIST_ENTRY ListHead)
{
	PLIST_ENTRY Flink;
	PLIST_ENTRY Entry;
	Entry = ListHead->Flink;
	Flink = Entry->Flink;
	ListHead->Flink = Flink;
	Flink->Blink = ListHead;
	return Entry;
}



PLIST_ENTRY RemoveTailList(PLIST_ENTRY ListHead)
{
	PLIST_ENTRY Blink;
	PLIST_ENTRY Entry;
	Entry = ListHead->Blink;
	Blink = Entry->Blink;
	ListHead->Blink = Blink;
	Blink->Flink = ListHead;
	return Entry;
}

void InsertTailList(PLIST_ENTRY ListHead,PLIST_ENTRY Entry)
{
	PLIST_ENTRY Blink;
	Blink = ListHead->Blink;
	Entry->Flink = ListHead;
	Entry->Blink = Blink;
	Blink->Flink = Entry;
	ListHead->Blink = Entry;
	return;
}


void InsertHeadList(PLIST_ENTRY ListHead,PLIST_ENTRY Entry)
{
	PLIST_ENTRY Flink;
	Flink = ListHead->Flink;
	Entry->Flink = Flink;
	Entry->Blink = ListHead;
	Flink->Blink = Entry;
	ListHead->Flink = Entry;
	return;
}

void AppendTailList(PLIST_ENTRY ListHead,PLIST_ENTRY ListToAppend)
{
	PLIST_ENTRY ListEnd = ListHead->Blink;
	ListHead->Blink->Flink = ListToAppend;
	ListHead->Blink = ListToAppend->Blink;
	ListToAppend->Blink->Flink = ListHead;
	ListToAppend->Blink = ListEnd;
	return;
}

void InitializeStack(PSTACK stack)
{
	int i = 0;

	stack->top = PAGE_TABLE_CACHE_END;
	stack->count = 1024;
}


unsigned pgc_count;
unsigned pgc_top;

int PushStack(PSTACK stack, unsigned int val)
{
	if (stack->count < 1024){
		__sync_add_and_fetch(&(stack->top), PAGE_SIZE);
		__sync_add_and_fetch(&(stack->count), 1);
		pgc_count = stack->count;
		pgc_top = stack->top;
		return 1;
	}else{
		return 0;
	}
}

unsigned int PopStack(PSTACK stack)
{
	unsigned ret = 0;
	if (stack->count > 0){
		__sync_add_and_fetch(&(stack->top), (0-PAGE_SIZE));
		ret = stack->top;
		__sync_add_and_fetch(&(stack->count), -1);
		pgc_count = stack->count;
		pgc_top = stack->top;
	}else{
		ret = 0;
	}

	return ret;
}


