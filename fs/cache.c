#include <fs/cache.h>
#include <fs/namespace.h>
#include <fs/vfs.h>
#include <mm/mm.h>
#include <lib/klib.h>
#include <syscall/unistd.h>

#define CACHE_HASH_SIZE 0

typedef struct _file_cache_item
{
	char* path;
	unsigned len;
	void* context;
	struct _file_cache_item* next;
}file_cache_item;

static file_cache_item cache[CACHE_HASH_SIZE];

hash_table* inode_cache;

static void file_cache_add(char* path);
static int file_cache_hash(char* path);

static int str_cmp(void* key1, void* key2)
{
	char* str1 = key1;
	char* str2 = key2;

	if(!str1 || !*str1 || !str2 || !*str2)
		return 0;

	return strcmp(str1, str2);
}

void file_cache_init()
{
	inode_cache = hash_create(str_cmp);
	memset(cache, 0, sizeof(cache));
	//file_cache_add("/lib/libc.so.6");
	//file_cache_add("/lib/ld-linux.so.2");
	//file_cache_add("/bin/ls");
}

void* file_cache_find(char* path)
{
	int hash = file_cache_hash(path);
	file_cache_item* item = 0;

	if(hash < 0)
		return 0;

	// because file cache is readonly and constant, 
	// we dont have to add lock here
	if(cache[hash].next == 0)
		return 0;

	item = cache[hash].next;
	while(item)
	{
		if(!strcmp(item->path, path))
			return item;

		item = item->next;
	}

	return 0;
}

int file_cache_read(void* cachefd, unsigned off, void* buf, unsigned len)
{
	file_cache_item* item = (file_cache_item*)cachefd;
	unsigned file_len = 0, read_len = 0;

	if(item == 0)
		return -1;

	file_len = item->len;
	read_len = (file_len > (off+len)) ? len : (file_len - off);
	memcpy(buf, (char*)item->context+off, read_len);
	return read_len;
}

static void file_cache_add(char* path)
{
	int hash = file_cache_hash(path);
	file_cache_item* item = 0, **cur = 0;;
	int fd;
	struct stat s;
	unsigned ctx_len = 0;

	if(hash < 0)
		return;

	if( fs_stat(path,&s) == -1)
		return;

	if(S_ISDIR(s.st_mode))
		return;

	fd = fs_open(path);
	if(fd < 0)
		return;

	ctx_len = ((s.st_size - 1) & PAGE_SIZE_MASK) + PAGE_SIZE;
	
	item = kmalloc(sizeof(*item));
	item->len = s.st_size;
	item->context = vm_alloc( (ctx_len)/PAGE_SIZE );
	fs_read(fd, 0, item->context, s.st_size);
	item->path = strdup(path);
	item->next = 0;

	cur = &cache[hash].next;
	while( (*cur) )
		cur = &((*cur)->next);

	*cur = item;

	fs_close(fd);
}

static int file_cache_hash(char* path)
{
	char tmp;
	int len, i;
	int hash = 0;
	if(!path || !*path)
		return -1;

	len = strlen(path);
	for(i = 0; i < len; i++)
	{
		tmp = path[i];
		hash += (int)tmp;
	}

	return (hash % 10);
}
