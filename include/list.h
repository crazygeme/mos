#ifndef _LIST_H_
#define _LIST_H_

typedef struct _list_entry {
	struct _list_entry *prev;
	struct _list_entry *next;
} list_entry;

void list_init(list_entry *head);

int list_is_empty(list_entry *head);

list_entry *list_remove_head(list_entry *head);

list_entry *list_remove_tail(list_entry *head);

void list_insert_tail(list_entry *head, list_entry *entry);

void list_insert_head(list_entry *head, list_entry *entry);

void list_append_tail(list_entry *head, list_entry *entry);

#define offset_of(type, field) (unsigned long)(&(((type *)0)->field))

#define container_of(node, type, field) \
	(type *)((char *)node - offset_of(type, field))

#endif
