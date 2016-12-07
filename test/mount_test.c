#include <mount.h>
#include <vfs.h>
#include <list.h>
#include <lock.h>
#include <klib.h>

extern LIST_ENTRY mnt_list;
void dump_mount_point()
{
    PLIST_ENTRY entry;
    mnt_list_entry* mnt = 0;
    int found = 0;
    entry = mnt_list.Flink;
    while (entry != &mnt_list)
    {
        mnt = CONTAINER_OF(entry, mnt_list_entry, mnt_list);
        printk("mnt  \"%s\" --> \"%s\"\n", mnt->path, mnt->type->rootname);
        entry = entry->Flink;
    }
}