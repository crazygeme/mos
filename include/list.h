#ifndef _LIST_H_
#define _LIST_H_

typedef struct _list_entry {
    struct _list_entry* Flink;
    struct _list_entry* Blink;
} list_entry;

void list_init(list_entry* ListHead);

int list_is_empty(list_entry* ListHead);

list_entry* list_remove_head(list_entry* ListHead);

list_entry* list_remove_tail(list_entry* ListHead);

void list_insert_tail(list_entry* ListHead, list_entry* Entry);

void list_insert_head(list_entry* ListHead, list_entry* Entry);

void list_append_tail(list_entry* ListHead, list_entry* ListToAppend);

#define offset_of(type, field) (unsigned long)(&(((type*)0)->field))

#define container_of(node, type, field) (type*)((char*)node - offset_of(type, field))

typedef struct _STACK {
    unsigned int top;
    unsigned int count;
    unsigned int mem[1024];
} STACK, *PSTACK;

void InitializeStack(PSTACK stack);

int PushStack(PSTACK stack, unsigned int val);

unsigned int PopStack(PSTACK stack);

#endif
