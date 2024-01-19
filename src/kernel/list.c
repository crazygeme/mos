#include <list.h>
#include <mm.h>
#include <config.h>

void list_init(list_entry* ListHead) {
    ListHead->Flink = ListHead->Blink = ListHead;
    return;
}

int list_is_empty(list_entry* ListHead) {
    return (int)(ListHead->Flink == ListHead);
}

int RemoveEntryList(list_entry* Entry) {
    list_entry* Blink;
    list_entry* Flink;
    Flink = Entry->Flink;
    Blink = Entry->Blink;
    Blink->Flink = Flink;
    Flink->Blink = Blink;
    return (int)(Flink == Blink);
}

list_entry* list_remove_head(list_entry* ListHead) {
    list_entry* Flink;
    list_entry* Entry;
    Entry = ListHead->Flink;
    Flink = Entry->Flink;
    ListHead->Flink = Flink;
    Flink->Blink = ListHead;
    return Entry;
}

list_entry* list_remove_tail(list_entry* ListHead) {
    list_entry* Blink;
    list_entry* Entry;
    Entry = ListHead->Blink;
    Blink = Entry->Blink;
    ListHead->Blink = Blink;
    Blink->Flink = ListHead;
    return Entry;
}

void list_insert_tail(list_entry* ListHead, list_entry* Entry) {
    list_entry* Blink;
    Blink = ListHead->Blink;
    Entry->Flink = ListHead;
    Entry->Blink = Blink;
    Blink->Flink = Entry;
    ListHead->Blink = Entry;
    return;
}

void list_insert_head(list_entry* ListHead, list_entry* Entry) {
    list_entry* Flink;
    Flink = ListHead->Flink;
    Entry->Flink = Flink;
    Entry->Blink = ListHead;
    Flink->Blink = Entry;
    ListHead->Flink = Entry;
    return;
}

void list_append_tail(list_entry* ListHead, list_entry* ListToAppend) {
    list_entry* ListEnd = ListHead->Blink;
    ListHead->Blink->Flink = ListToAppend;
    ListHead->Blink = ListToAppend->Blink;
    ListToAppend->Blink->Flink = ListHead;
    ListToAppend->Blink = ListEnd;
    return;
}

void InitializeStack(PSTACK stack) {
    int i = 0;

    for (i = 0; i < 1024; i++) {
        stack->mem[i] = PAGE_TABLE_CACHE_BEGIN + i * PAGE_SIZE;
    }
    stack->top = 1023;
    stack->count = 1024;
}

unsigned pgc_count;
unsigned pgc_top;

int PushStack(PSTACK stack, unsigned int val) {
    if (stack->count < 1024) {
        stack->mem[stack->top] = val;
        __sync_add_and_fetch(&(stack->top), 1);
        __sync_add_and_fetch(&(stack->count), 1);
        pgc_count = stack->count;
        pgc_top = stack->top;
        return 1;
    } else {
        return 0;
    }
}

unsigned int PopStack(PSTACK stack) {
    unsigned ret = 0;
    if (stack->count > 0) {
        __sync_add_and_fetch(&(stack->top), -1);
        ret = stack->mem[stack->top];
        __sync_add_and_fetch(&(stack->count), -1);
        pgc_count = stack->count;
        pgc_top = stack->top;
    } else {
        ret = 0;
    }

    return ret;
}
