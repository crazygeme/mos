#include <fs/mount.h>
#include <fs/vfs.h>
#ifdef WIN32
#include <Windows.h>
#include <osdep.h>
#elif MACOS
#include <osdep.h>
#include <lib/list.h>
#else
#include <lib/list.h>
#include <ps/lock.h>
#include <lib/klib.h>
#endif

static LIST_ENTRY mnt_list;
static semaphore mnt_lock;

typedef struct _mnt_list_entry
{
    LIST_ENTRY mnt_list;
    struct filesys_type* type;
    char path[260];
}mnt_list_entry;

void mount_init()
{
    InitializeListHead(&mnt_list);
    sema_init(&mnt_lock, "mount", 0);
}

int do_mount(char* path, struct filesys_type* type)
{
    mnt_list_entry* entry;
    if (mount_lookup(path)) {
        return 0;
    }

    entry = kmalloc(sizeof(*entry));
    strcpy(entry->path, path);
    entry->type = type;

    sema_wait(&mnt_lock);
    InsertTailList(&mnt_list, &entry->mnt_list);
    sema_trigger(&mnt_lock);
    
    return 1;
}

static mnt_list_entry* do_mount_lookup(char* path)
{
    PLIST_ENTRY entry;
    mnt_list_entry* mnt = 0;
    int found = 0;

    sema_wait(&mnt_lock);
    entry = mnt_list.Flink;
    while (entry != &mnt_list) {
        mnt = CONTAINER_OF(entry, mnt_list_entry, mnt_list);
        if (strcmp(mnt->path, path)==0) {
            found = 1;
            break;
        }

        entry = entry->Flink;
    }
    sema_trigger(&mnt_lock);

    if (!found) {
        return 0;
    }

    return mnt;
}

int do_umount(char* path)
{
    mnt_list_entry* mnt = do_mount_lookup(path);

    if (!mnt) {
        return 0;
    }

    sema_wait(&mnt_lock);
    RemoveEntryList(&mnt->mnt_list);
    sema_trigger(&mnt_lock);
    kfree(mnt);

    return 1;
}

struct filesys_type* mount_lookup(char* path)
{
    mnt_list_entry* mnt = do_mount_lookup(path);
    struct filesys_type* ret;

    if (!mnt) {
        return 0;
    }

    ret = mnt->type;
    return ret;
}

#ifdef TEST_MOUNT
void dump_mount_point()
{
    PLIST_ENTRY entry;
    mnt_list_entry* mnt = 0;
    int found = 0;

    sema_wait(&mnt_lock);
    entry = mnt_list.Flink;
    while (entry != &mnt_list) {
        mnt = CONTAINER_OF(entry, mnt_list_entry, mnt_list);
        printk("mnt  \"%s\" --> \"%s\"\n", mnt->path, mnt->type->rootname);
        entry = entry->Flink;
    }
    sema_trigger(&mnt_lock);

}
#endif
