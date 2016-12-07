#ifndef _MOUNT_H_
#define _MOUNT_H_
#include <list.h>
typedef struct _mnt_list_entry
{
    LIST_ENTRY mnt_list;
    struct filesys_type* type;
    char path[260];
}mnt_list_entry;

struct filesys_type;

void mount_init();

int do_mount(char* path, struct filesys_type* type);

int do_umount(char* path);

struct filesys_type* mount_lookup(char* path);

#endif
