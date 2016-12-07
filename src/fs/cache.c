#include <namespace.h>
#include <vfs.h>
#include <mm.h>
#include <klib.h>
#define CACHE_HASH_SIZE 0
#include <cache.h>
#include <unistd.h>

hash_table* inode_cache;

static int str_cmp(void* key1, void* key2)
{
    char* str1 = key1;
    char* str2 = key2;

    if (!str1 || !*str1 || !str2 || !*str2)
        return 0;

    return strcmp(str1, str2);
}

void file_cache_init()
{
    inode_cache = hash_create(str_cmp);
}
