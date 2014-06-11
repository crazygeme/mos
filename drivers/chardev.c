#include <drivers/chardev.h>
#include <ps/lock.h>
#include <lib/klib.h>

static LIST_ENTRY chardev_list;
static spinlock chardev_lock;
static unsigned int chardev_id;

static void getlock()
{
	spinlock_lock(&chardev_lock);
}

static void putlock()
{
	spinlock_unlock(&chardev_lock);
}

void chardev_init()
{
	InitializeListHead(&chardev_list);
	spinlock_init(&chardev_lock);
	chardev_id = 0;
}

chardev* chardev_register(void* aux, char* name, fpchar_read read, fpchar_write write, fpchar_close close)
{
	chardev* dev = kmalloc(sizeof(*dev));
	memset(dev, 0, sizeof(*dev));
	dev->aux = aux;
	dev->id = __sync_fetch_and_add(&chardev_id, 1);
	dev->read = read;
	dev->write = write;
	dev->close = close;
	strcpy(dev->name, name);
	getlock();
	InsertTailList(&chardev_list, &dev->char_list);
	putlock();
	return dev;
}

chardev* chardev_get_by_id(unsigned int id)
{
	LIST_ENTRY* head = &chardev_list;
	LIST_ENTRY* entry;
	chardev* node = 0;
	chardev* ret = 0;

	getlock();
	for( entry = head->Flink; entry != head; entry = entry->Flink)
	{
		node = CONTAINER_OF(entry, chardev, char_list);
		if(node->id == id)
		{
			ret = node;
			break;
		}
	}
	putlock();

	return ret;
}

void chardev_close()
{
	LIST_ENTRY* head = &chardev_list;
	LIST_ENTRY* entry;
	chardev* node = 0;

	getlock();
	for( entry = head->Flink; entry != head; entry = entry->Flink)
	{
		node = CONTAINER_OF(entry, chardev, char_list);
		putlock();

		if(node->close)
			node->close(node->aux);

		getlock();
	}
	putlock();
}
