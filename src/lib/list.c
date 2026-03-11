#include <lib/list.h>
#include <config.h>

void list_init(list_entry *head)
{
	head->prev = head->next = head;
	return;
}

int list_is_empty(list_entry *head)
{
	return (int)(head->prev == head);
}

int list_remove_entry(list_entry *entry)
{
	list_entry *next;
	list_entry *prev;
	prev = entry->prev;
	next = entry->next;
	next->prev = prev;
	prev->next = next;
	return (int)(prev == next);
}

list_entry *list_remove_tail(list_entry *head)
{
	list_entry *prev;
	list_entry *entry;
	entry = head->prev;
	prev = entry->prev;
	head->prev = prev;
	prev->next = head;
	return entry;
}

list_entry *list_remove_head(list_entry *head)
{
	list_entry *next;
	list_entry *entry;
	entry = head->next;
	next = entry->next;
	head->next = next;
	next->prev = head;
	return entry;
}

void list_insert_head(list_entry *head, list_entry *entry)
{
	list_entry *next;
	next = head->next;
	entry->prev = head;
	entry->next = next;
	next->prev = entry;
	head->next = entry;
	return;
}

void list_insert_tail(list_entry *head, list_entry *entry)
{
	list_entry *prev;
	prev = head->prev;
	entry->prev = prev;
	entry->next = head;
	prev->next = entry;
	head->prev = entry;
	return;
}
