#include <lib/klib.h>
#include <drivers/tty.h>
#include <int/timer.h>
#include <ps/lock.h>

static int cursor = 0;
#define CUR_ROW (cursor / TTY_MAX_COL)
#define CUR_COL (cursor % TTY_MAX_COL)

// following for malloc/free
static unsigned int cur_block_top = KHEAP_BEGIN;
static kblock free_list[513] = { 0 }; 
static unsigned int kblk(unsigned page_count);
static void klib_add_to_free_list(int index, kblock* buf, int force_merge);

static void klib_cursor_forward(int new_pos);
static unsigned int klib_random_num = 1;

static spinlock tty_lock;
static spinlock heap_lock;

static unsigned int heap_quota;
static unsigned int heap_quota_high;

static void lock_tty()
{
	spinlock_lock(&tty_lock);
}

static void unlock_tty()
{
	spinlock_unlock(&tty_lock);
}

static void lock_heap()
{
	spinlock_lock(&heap_lock);
}

static void unlock_heap()
{
	spinlock_unlock(&heap_lock);
}

void klib_init()
{
	tty_init();
	
	klib_random_num = 1;
	cursor = 0;
	heap_quota = 0;

    heap_quota_high = 0;

	cur_block_top = KHEAP_BEGIN;
	spinlock_init(&tty_lock);
	spinlock_init(&heap_lock);
}


void klib_putchar(char c)
{
	int new_pos = 0;
	if ( c == '\n'){
		int new_row = CUR_ROW + 1;
		int new_col = 0;
		new_pos = ROW_COL_TO_CUR(new_row, new_col);
	}else if (c == '\t'){
		int loop_count = 4 - (CUR_COL % 4);
		int i = 0;
		if (loop_count == 0)
		  loop_count = 4;
		for (i = 0; i < loop_count; i++)
			klib_putchar(' ');
		
		new_pos = cursor;
    } else if (c  == '\r') {
		new_pos = ROW_COL_TO_CUR(CUR_ROW, 0);
	}else{
		tty_putchar( CUR_ROW, CUR_COL, c);
		new_pos = cursor + 1;
	}

	klib_cursor_forward(new_pos);
}

void klib_print(char *str)
{
	if (!str || !*str){
		return;
	}

	while (*str){
		klib_putchar( *str++);
	}
}

void klib_putint(int num)
{
	char str[33] = {0};
	klib_print( klib_itoa(str, num));
}

void klib_info(char *info, int num, char* end)
{
    klib_print(info);
    klib_putint(num);
    klib_print(end);
}

void klib_clear()
{
	lock_tty();
	tty_clear();
	cursor = 0;
	unlock_tty();
}

static void klib_cursor_forward(int new_pos)
{
	cursor = new_pos;
	while (cursor >= TTY_MAX_CHARS)
	{
		tty_roll_one_line();
		cursor -= TTY_MAX_COL;
	}

	tty_movecurse((unsigned short)cursor);
}

char *klib_itoa(char *str, int num)
{  
  char *p = str;  
  char ch;  
  int i;  
  int flag = 0;
  
  *p++ = '0';  
  *p++ = 'x';  
  if(num == 0)         
    *p++ = '0';  
  else  
  {    
    for(i=28;i>=0;i-=4)
    {  
      ch = (num >> i) & 0xF;  
      if(flag || (ch > 0))  
      {  
        flag = 1;  
        ch += '0';  
        if(ch > '9')   
          ch += 7;  
        *p++ = ch;  
      }  
    }  
  }  
  *p = 0;   
  return str;  
}




//// 

void* kmalloc(unsigned size)
{
	unsigned int ret = 0;
	int free_list_index = size / 8 + 1;
	kblock* free_list_head = &free_list[free_list_index];
	int index = 0;
	int sliced = 0;

	
	if (free_list_index > 511) // that's too large
		return NULL;

	if (size == 0)
		return NULL;

	lock_heap();
	//log("[malloc] malloc %d ", free_list_index);
	if (free_list_head->next == NULL){
		for (index = free_list_index + 1; index <= 512; index++){
			int borrow_size, left_count;
			kblock* addr;
			int left_index;
			if (free_list[index].next == 0)
				continue;

			borrow_size = free_list_index;
			left_count = index - borrow_size;

			if (left_count < 1)
				continue;

			addr = (kblock*)free_list[index].next;
			free_list[index].next = addr->next;
			addr->next = 0;
			//log("[split] %d split into %d + %d", index, free_list_index, left_count - 1);
			klib_add_to_free_list(free_list_index, (void*)addr, 0);
			left_index = left_count - 1;
			klib_add_to_free_list(left_index, (void*)((unsigned int)addr + 8 * (borrow_size + 1)), 0);

			break;
		}

		if (index == 513){
			int left_index;
			int borrow_size = free_list_index;
			unsigned int vir = kblk(1);
			int left_count;
			if (vir == 0){
				unlock_heap();
				return NULL;
			}
			index = 511;
			left_count = index - free_list_index;
			//log("[split] %d split into %d + %d", index, free_list_index, left_count - 1);
			klib_add_to_free_list(free_list_index, (void*)vir, 0);
			left_index = index - free_list_index - 1;
			klib_add_to_free_list(left_index, (void*)((unsigned int)vir + 8 * (borrow_size + 1)), 0);
		}
	}

	if (free_list_head->next != NULL){
		kblock * block = free_list_head->next;
		free_list_head->next = block->next;
		block->next = 0;
		block->size = size;
		ret = (unsigned int)block;
		ret += sizeof(kblock);
		heap_quota += size;

        if (heap_quota_high < heap_quota) {
            heap_quota_high = heap_quota;
        }
        unlock_heap(); 

		return (void*)ret;
	}

	unlock_heap();
	return NULL;

}

void kfree(void* buf)
{
	kblock* block = NULL;
	int size = 0;
	int free_list_index;
	if (buf == NULL)
		return;

	lock_heap();
	block = (kblock*)((unsigned int)buf - 8);
	size = block->size;
	if (size == 0xdeadbeef) {
		// double free
		*((int *)0) = 0;
	}
	free_list_index = size / 8 + 1;
	block->size = 0xdeadbeef;
	memset(buf, 'c', size);
	heap_quota -= size;
	klib_add_to_free_list(free_list_index, block, 1);
	
	unlock_heap();
}

void klogquota()
{
	unsigned int quota = heap_quota;
	klib_info("heap quota: ", quota, " ");
	klib_info("max usage:  ", heap_quota_high, "\n");
}


static unsigned int kblk(unsigned page_count)
{
	unsigned ret = cur_block_top;

	//klib_putint(cur_block_top);
	
	if (page_count == 0)
		return 0;

	if (cur_block_top + page_count * PAGE_SIZE >= KHEAP_END){
		klib_print("buffer overflow\n");
		klib_info("cur_block_top: ", cur_block_top, "\n");
		klib_info("page_count: ", page_count, "\n");
		klib_info("KHEAP_BEGIN: ", KHEAP_BEGIN, "\n");
		klib_info("KHEAP_END: ", KHEAP_END, "\n");
		return 0;
	}

	cur_block_top += page_count * PAGE_SIZE;

#ifdef TEST_MALLOC
	klib_info("cur_block_top: ", cur_block_top, "\n");
#endif

	return ret;
}

static int klib_same_page(unsigned addr1, unsigned addr2)
{
	return ((addr1 & 0xfffff000) == (addr2 & 0xfffff000));
}

static int klib_free_list_merge(int node_index, kblock* buf, int target)
{
	kblock *head = NULL;
	kblock *node = NULL;
	kblock *pre = NULL;
	kblock *ppre = NULL;

	int merged = 0;
	int merged_index = 0;

	unsigned int pre_addr = (unsigned int)pre;
	unsigned int node_addr = (unsigned int)node;
	unsigned int buf_addr = (unsigned int)buf;

	head = &free_list[target];

	node = head->next;
	pre = head;
	ppre = NULL;
	while (node){
		unsigned int addr = (unsigned int)node;
		unsigned int inserted = (unsigned int)buf;
		if (addr > inserted){
			break;
		}
		ppre = pre;
		pre = node;
		node = node->next;
	}

	pre_addr = (unsigned int)pre;
	node_addr = (unsigned int)node;
	buf_addr = (unsigned int)buf;

	if (pre != head){
		merged_index = node_index + target + 1;
		if ((pre_addr + 8 * (target + 1) == buf_addr) && (merged_index <= 511) &&
			klib_same_page(pre_addr, buf_addr) ){
			// merge pre
			ppre->next = node;
			buf->next = NULL;
			pre->next = NULL;
			klib_add_to_free_list(merged_index, pre, 1);
			merged = 1;
			//log("[pre+buf] %d + %d blocks merged into %d block\n", target, node_index, merged_index);
		}
	}

	if (!merged && node != NULL){
		merged_index = node_index + target + 1;
		if ((buf_addr + 8 * (node_index + 1) == node_addr) && (merged_index <= 511) &&
			klib_same_page(buf_addr, node_addr)){
			pre->next = node->next;
			buf->next = NULL;
			node->next = NULL;
			klib_add_to_free_list(merged_index, buf, 1);
			merged = 1;
			//log("[buf+node] %d + %d  blocks merged into %d block\n", node_index, target, merged_index);
		}
	}

	return merged;
}


static void klib_add_to_free_list(int index, kblock* buf, int force_merge)
{
	kblock *head = NULL;
	kblock *node = NULL;
	kblock *pre = NULL;


	int merged = 0;
	int merged_index = 0;

	if (index < 0 || index > 511){
		return;
	}

	if (force_merge){
		for (merged_index = 0; merged_index < 511; merged_index++){
			merged = klib_free_list_merge(index, buf, merged_index);
			if (merged)
				return;
		}
	}


	head = &free_list[index];

	node = head->next;
	pre = head;
	while (node){
		unsigned int addr = (unsigned int)node;
		unsigned int inserted = (unsigned int)buf;
		if (addr > inserted){
			break;
		}
		pre = node;
		node = node->next;
	}



	if (!merged){
		pre->next = (kblock*)buf;
		((kblock*)buf)->next = node;
	}

}

////

void memcpy(void* _src, void* _dst, unsigned len)
{
	char* src = _src;
	char* dst = _dst;

	int i = 0;
    for (i = 0; i < len; i++) {
        src[i] = dst[i];
    }
}

void memmove(void* _src, void* _dst, unsigned len)
{
	char* src = _src;
	char* dst = _dst;

	int i = 0;

    if (src >= dst) {
		for (i = 0; i < len; i++) {
			src[i] = dst[i];
		}
    }else{
		for (i = len - 1; i >= 0; i--) {
			src[i] = dst[i];
		}
	}
    
}

int memcmp(void* _src, void* _dst, unsigned len)
{
	char* src = _src;
	char* dst = _dst;
	int i = 0;

    for (i = 0; i < len; i++) {
        if (src[i] > dst[i]) {
            return 1;
        }else if(src[i] < dst[i]){
			return -1;
		}
    }

	return 0;
}

void memset(void* _src, char val, int len)
{
	char* src = _src;
	int i = 0;

    for (i = 0; i < len; i++) {
        src[i] = val;
    }
}
 
unsigned strlen(const char* str)
{
	int count = 0;
    while (*str++) {
        count++;
    }
	return count;
}

char* strcpy(char* dst, const char* src)
{
	char *ret = dst;
	int len = strlen(src);
	int i = 0;
	for (i = 0; i < len; i++){
		ret[i] = src[i];
	}
	ret[i] = '\0';
    return dst;
}

char* strstr(const char* src, const char* str)
{
	// FIXME
	return NULL;
}

char* strrev(char *src)
{
	int len = strlen(src);
	int mid = len / 2;
	int i = 0;

    for (i = 0; i < mid; i++) {
        char tmp = src[i];
		src[i] = src[len - i -1];
		src[len - i - 1] = tmp;
    }

	return src;
}

int strcmp(char* src, char* dst)
{
	int src_len = strlen(src);
	int dst_len = strlen(dst);
	int len = src_len > dst_len ? dst_len : src_len;
	int i = 0;

    for (i = 0; i < len; i++) {
        if (src[i] > dst[i]) {
            return 1;
        }else if(src[i] < dst[i]){
			return -1;
		}
    }

    if (src_len > len) {
        return 1;
    }else if(dst_len > len){
		return -1;
	}else{
		return 0;
	}
}

char* strcat(char* src, char* msg)
{
	char* tmp = src + strlen(src);
	strcpy(tmp, msg);
	return src;
}

char* strchr(char* str, char c)
{
	int i = 0;
	int len = strlen(str);

	for (i = 0; i < len; i++){
		if (str[i] == c)
		  return (str+i);
	}

	return 0;
}

char* strrchr(char* str, char c)
{
	int i = 0;
	int len = strlen(str);
	for (i = len-1; i >= 0; i--) {
		if (str[i] == c)
		  return (str+i);
	}

	return 0;
}

int isspace(const char c)
{
    return (c == ' ' || c == '\t' || c == '\n');
}

int tolower(int c)
{
	if (isupper(c)) {
		return (c - 'A' + 'a');
	}

	return c;
}

int toupper(int c)
{
	if (islower(c)) {
		return (c - 'a' + 'A');
	}

	return c;
}

int islower(int c)
{
	return (c >= 'a' && c <= 'z');
}

int isupper(int c)
{
	return (c >= 'A' && c <= 'Z');
}

void printf(const char* str, ...)
{
	va_list ap;
	va_start(ap, str);
	vprintf(str,ap);
	va_end(ap);
}

void printk(const char* str, ...)
{
	va_list ap;
    char* str_mill;
    int len = 0;
    int i = 0;
	time_t time;
	timer_current(&time);
    str_mill = itoa(time.milliseconds, 10, 0);
    len = strlen(str_mill);
    len = 3 - len;
    lock_tty();
	printf("[%d.", time.seconds);
    for (i = 0; i < len; i++) {
        printf("0");
    }
    klib_print(str_mill);
	klib_print("]");
    kfree(str_mill);
    va_start(ap, str); 
	vprintf(str,ap);
	va_end(ap);
    unlock_tty();
}

void tty_write(const char* buf, unsigned len)
{
	int i = 0;
	lock_tty();
	for (i = 0; i < len; i++)
			klib_putchar(buf[i]);
	unlock_tty();
}


void vprintf(const char* src, va_list ap)
{
	int len = strlen(src);
	int i = 0;
    int min_len = 0;

    for (i = 0; i < len; i++) {
        char cur = src[i];
        if (cur == '%') {
			cur = src[i+1];
            switch (cur) {
			case '%':
				klib_putchar(src[i+1]);
				break;
			case 'd':
				{
					int arg = va_arg(ap, int);
					char* s = itoa(arg, 10, 1);
					klib_print(s);
					kfree(s);
					break;
				}
			case 'x':
			case 'p':
				{
					int arg = va_arg(ap, int);
					char* s = itoa(arg, 16, 0);
					klib_print(s);
					kfree(s);
					break;
				}
			case 'u':
				{
					int arg = va_arg(ap, int);
					char* s = itoa(arg, 10, 0);
					klib_print(s);
					kfree(s);
					break;
				}
			case 's':
				{
					char* arg = va_arg(ap, char*);
					klib_print(arg);
					break;
				}
            case 'h':
                {
                    unsigned arg = va_arg(ap, unsigned);
                    print_human_readable_size(arg);
                    break;
                }
			default:
				{
					klib_putchar('?');
				}
            }
			i++;
        }else{
			klib_putchar(cur);
		}
    }
}

void print_human_readable_size (unsigned int size) 
{
  if (size == 1)
    printf ("1 byte");
  else 
    {
      static const char *factors[] = {"bytes", "kB", "MB", "GB", "TB", NULL};
      const char **fp;

      for (fp = factors; size >= 1024 && fp[1] != NULL; fp++)
        size /= 1024;
      printf ("%u %s", size, *fp);
    }
}

static char _num(int i){


    if (i >= 0 && i < 10) {
        return i + '0';
    }

	return (i - 10) + 'a';
}

char* itoa(int num, int base, int sign)
{
	int str_len = 12;
	char* str = kmalloc(12);
	char* ret = str;
	int left = num;
    unsigned uleft = num;
	int cur = 0;
    unsigned ucur = 0;
	char *begin;

	memset(str, 0, 12);
	
	if (uleft == 0x80000000){ // this is a special number, that x == -x
		strcpy(str, "0x80000000");
		return str;
	}

	if (num == 0){
		if (base == 10)
		  strcpy(str, "0");
		else if(base == 16)
		  strcpy(str, "0x0");
		return str;
	}

    if (base != 10 && base != 16) {
		kfree(str);
        return NULL;
    }


	if ((base == 10) && sign && left < 0) {
		str[0] = '-';
		str++;
		left = (0 - left);
	}else if(base == 16){
		str[0] = '0';
		str[1] = 'x';
		str += 2;
	}

	begin = str;
    if (sign && (base != 16)) {
        while (left) {
            cur = left % base;
            *str = _num(cur);
            str++;
            left = left / base;
        }
    }else{
        while (uleft) {
            ucur = uleft % base;
            *str = _num(ucur);
            str++;
            uleft = uleft / base;
        }
    }

	strrev(begin);

	return ret;
}





void klib_srand(unsigned _seed)
{
	klib_random_num = _seed;
}

unsigned int klib_rand()
{
	unsigned int ret = klib_random_num * 0x343fd;
	ret += 0x269EC3;
	klib_random_num = ret;
	ret = klib_random_num;
	ret >>= 0x10;
	ret = ret & 0x7fff;
	return ret;
}


void reboot()
{
	// FIXME do formal steps before reboot
	_write_port(0x64, 0xfe);
}


void shutdown()
{
  const char s[] = "Shutdown";
  const char *p;

  printf ("Powering off...\n");


  /* 	This is a special power-off sequence supported by Bochs and
		QEMU, but not by physical hardware. */
  for (p = s; *p != '\0'; p++)
    _write_port (0x8900, *p);


  /* 	This will power off a VMware VM if "gui.exitOnCLIHLT = TRUE"
		is set in its configuration file. (The "pintos" script does
		that automatically.) */
  asm volatile ("cli; hlt" : : : "memory");

  /* None of those worked. */
  printf ("still running...\n");
  for (;;);
}


#ifdef TEST_MALLOC
	void test_malloc_process()
	{
#define MIN_MEM 1
#define MAX_MEM 4080
#define ALLOC_COUNT 10
		// test kmalloc, kfree
		void* addr[ALLOC_COUNT];
		int size = 0;
		int mallocor = 10;
		int* a = kmalloc(1);
		int* b = kmalloc(25);
		int i = 0;
		kfree(a);
		kfree(b);
		while (1){
			int mem_count = klib_rand() % ALLOC_COUNT;
			if (mem_count == 0)
				mem_count = 1;
			for (i = 0; i < mem_count; i++){
				size = klib_rand() % (MAX_MEM-MIN_MEM);
				size += MIN_MEM;
				if (size == 0)
					addr[i] = 0;
				else
				{
					addr[i] = kmalloc(size);
					if (addr[i])
						memset(addr[i], 0xc, size);
				}
			}
			for (i = 0; i < mem_count; i++){
				kfree(addr[i]);
			}
		}
	}

#endif

