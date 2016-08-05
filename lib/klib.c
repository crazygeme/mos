#ifdef __KERNEL__
#include <lib/klib.h>
#include <drivers/tty.h>
#include <int/timer.h>
#include <ps/lock.h>
#include <ps/ps.h>
#include <fs/vfs.h>;
#else
#include <syscall.h>
#include <lib/klib.h>
#endif

// following for malloc/free

static kblock free_list[513] = { 0 }; 
static void klib_add_to_free_list(int index, kblock* buf, int force_merge);
unsigned int heap_quota;
unsigned int heap_quota_high;
unsigned int mem_time;
unsigned int heap_time;
static unsigned int klib_random_num = 1;


#ifdef __KERNEL__

void mm_report()
{
    printk("memcpy time %d millisecond, malloc/free time %d millisecond\n", mem_time, heap_time);
    mem_time = heap_time = 0;
}


unsigned int cur_block_top = KHEAP_BEGIN;
static int cursor = 0;
#define CUR_ROW (cursor / TTY_MAX_COL)
#define CUR_COL (cursor % TTY_MAX_COL)


static unsigned int kblk(unsigned page_count);
static void klib_cursor_forward(int new_pos);

static spinlock tty_lock;
static spinlock heap_lock;
static semaphore klog_lock;



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

#ifdef __DEBUG__
static int klog_inited = 0;
void klog_init()
{
	klog_inited = 1;
	sema_init(&klog_lock, "klog", 0);

	klog("\n\n===========================\n");
	
}

void klog_write(char c)
{
	if (!klog_inited) {
		return;
	}

	if (isprint(c)) {
		serial_putc(c);
	}else{
		klog_printf("\\%x", c);
	}
}

void klog_writestr(char* str)
{
	if (!klog_inited) {
		return;
	}

	if (!str || !*str) {
		return;
	}

	while (*str) {
		klog_write(*str++);
	}
}

void klog_close()
{
	if (!klog_inited) {
		return;
	}

	serial_flush();
}

#else
void klog_init()
{
	sema_init(&klog_lock, "klog", 0);
}

void klog_write(char c)
{
}

void klog_writestr(char* str)
{
}

void klog_close()
{
}
#endif

void klib_init()
{
	tty_init();
	
	klib_random_num = 1;
	cursor = 0;
	heap_quota = 0;
    heap_quota_high = 0;
    mem_time = 0;
    heap_time = 0;

	cur_block_top = KHEAP_BEGIN;
	spinlock_init(&tty_lock);
	spinlock_init(&heap_lock);

}

static int ansi_control_flag = 0;
static char ansi_control[10] = {0};
static int ansi_control_idx = 0;

static void ansi_control_begin()
{
	memset(ansi_control, 0, sizeof(ansi_control));
	ansi_control_idx = 0;
	ansi_control_flag = 1;
}

static void ansi_control_end()
{
	memset(ansi_control, 0, sizeof(ansi_control));
	ansi_control_idx = 0;
	ansi_control_flag = 0;
}

static int is_ansi_control_begin()
{
	return ansi_control_flag;
}


static void ansi_control_add(char c)
{
	char* str_arg = ansi_control + 1;
	int arg = 0;
	int row, col;
	int new_pos = 0;
	char* str_row, *str_col;

	switch (c) {
	case 'm':
		arg = atoi(str_arg);
		if (arg == 0) { // 关闭所有属性
		} else if (arg == 1){ // 设置高亮度 
		} else if (arg == 4){ // 下划线 
		} else if (arg == 5){ // 闪 烁 
		} else if (arg == 7){ // 反显 
		} else if (arg == 8){ // 消隐 
		} else if (arg >= 30 && arg <= 37){ // 设置前景色 
		} else if (arg >= 40 && arg <= 47) { // 设置背景色 
		}
		break;
	case 'A': //光标上移n行 
		arg = atoi(str_arg);
		row = CUR_ROW;
		col = CUR_COL;
		row -= arg;
		if (row < 0)
			row = 0;
		new_pos = ROW_COL_TO_CUR(row, col);
		klib_cursor_forward(new_pos); 
		
		break;
	case 'B': // 光标下移n行 
		arg = atoi(str_arg);
		row = CUR_ROW;
		col = CUR_COL;
		row += arg;
		if (row >= TTY_MAX_ROW)
			row = TTY_MAX_ROW-1;
		new_pos = ROW_COL_TO_CUR(row, col);
		klib_cursor_forward(new_pos); 
		break;
	case 'C': // 光标右移n行 
		arg = atoi(str_arg);
		row = CUR_ROW;
		col = CUR_COL;
		col += arg;
		if (col >= TTY_MAX_COL)
			col = TTY_MAX_COL-1;
		new_pos = ROW_COL_TO_CUR(row, col);
		klib_cursor_forward(new_pos); 
		break;
	case 'D': // 光标左移n行 
		arg = atoi(str_arg);
		row = CUR_ROW;
		col = CUR_COL;
		col -= arg;
		if (col < 0)
			col = 0;
		new_pos = ROW_COL_TO_CUR(row, col);
		klib_cursor_forward(new_pos); 
		break;
	case 'H': // 设置光标位置 (ROW:COL)
		str_col = strchr(str_arg, ';');
		*str_col = '\0';
		str_col++;
		str_row = str_arg;
		row = atoi(str_row)-1;
		col = atoi(str_col)-1;
		if (row < 0)
			row = 0;
		if (row >= TTY_MAX_ROW)
			row = TTY_MAX_ROW - 1;
		if (col < 0)
			col = 0;
		if (col >= TTY_MAX_COL)
			col = TTY_MAX_COL - 1;
		new_pos = ROW_COL_TO_CUR(row, col);
		klib_cursor_forward(new_pos); 
		break;
	case 'J': // 清屏 
		tty_clear();
		break;
	case 'K': // 清除从光标到行尾的内容 
		break;
	case 's':
		break;
	case 'u':
		break;
	case 'l':
		break;
	case 'h':
		break;
	default:
		break;
	}

	if ( (c >= 'a' && c <= 'z') ||
		(c >= 'A' && c <= 'Z')){
		// not support yet
		ansi_control_end();
	} else {
		ansi_control[ansi_control_idx] = c;
		ansi_control_idx++;
	}
}

int klib_putchar(char c)
{
    int new_pos = 0;

    if (is_ansi_control_begin())
    {
        ansi_control_add(c);
        return -1;
    }

    if (c == '\n')
    {
        int new_row = CUR_ROW + 1;
        int new_col = 0;
        new_pos = ROW_COL_TO_CUR(new_row, new_col);
    }
    else if (c == '\t')
    {
        int loop_count = 4 - (CUR_COL % 4);
        int i = 0;
        if (loop_count == 0)
            loop_count = 4;
        for (i = 0; i < loop_count; i++)
            klib_putchar_update_cursor(' ');

        new_pos = cursor;
    }
    else if (c == '\r')
    {
        new_pos = ROW_COL_TO_CUR(CUR_ROW, 0);
    }
    else if (c == '\b')
    {
        cursor--;
        //tty_putchar( CUR_ROW, CUR_COL, ' ');
        new_pos = cursor;
    }
    else if ((unsigned)c == 0xc)
    {
        tty_clear();
    }
    else if (isprint(c))
    {
        tty_putchar(CUR_ROW, CUR_COL, c);
        new_pos = cursor + 1;
    }
    else if ((unsigned)c == 0x1b)
    {
        ansi_control_begin();
    }
    else
    {
        return -1;
    }

    return new_pos;
}

void klib_update_cursor(int pos)
{
    if (pos < 0)
        return;

    cursor = pos;
}

void klib_flush_cursor()
{
    klib_cursor_forward(cursor);
}

void klib_putchar_update_cursor(char c)
{
    int new_pos = klib_putchar(c);
	klib_cursor_forward(new_pos);
}

void klib_print(char *str)
{
	if (!str || !*str){
		return;
	}

	while (*str){
		klib_putchar_update_cursor( *str++);
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

	tty_movecurse((unsigned)cursor);
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
		return vm_alloc(page_count);
	}

	cur_block_top += page_count * PAGE_SIZE;

#ifdef TEST_MALLOC
	klib_info("cur_block_top: ", cur_block_top, "\n");
#endif

	return ret;
}


#else

static unsigned int cur_block_top = 0;

#define kblk(page) blk(page)
#define klib_putchar_update_cursor libc_putchar
#define klib_print libc_print

static unsigned int blk(unsigned page_count)
{
	unsigned ret = cur_block_top;
	unsigned new_top = cur_block_top + page_count * PAGE_SIZE;

	if (page_count == 0) {
		return ret;
	}

	cur_block_top = brk(new_top);
	return ret;

}

static void lock_heap()
{
}

static void unlock_heap()
{
}


void libc_init()
{
	cur_block_top = brk(0);
	heap_quota = heap_quota_high = 0;
}


void libc_putchar(char c)
{
	char buf[1];
	buf[0] = c;
	write(1, buf, 1);
}



void libc_print(char *str)
{
	write(1, str, strlen(str));
}

#endif


//// 

void* malloc(unsigned size)
{
	unsigned int ret = 0;
	int free_list_index = size / 8 + 1;
	kblock* free_list_head = &free_list[free_list_index];
	int index = 0;
	int sliced = 0;
#ifdef DEBUG_FFS
    unsigned t = time_now();
#endif
	
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
                #ifdef DEBUG_FFS
                heap_time += time_now() - t;
                #endif
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

        #ifdef DEBUG_FFS
        heap_time += time_now() - t;
        #endif
		return (void*)ret;
	}

	unlock_heap();
    #ifdef DEBUG_FFS
    heap_time += time_now() - t;
    #endif
	return NULL;

}

void free(void* buf)
{
	kblock* block = NULL;
	int size = 0;
	int free_list_index;

    #ifdef DEBUG_FFS
    unsigned t = time_now();
    #endif

	if (buf == NULL)
		return;

	lock_heap();
	block = (kblock*)((unsigned int)buf - 8);
	size = block->size;
	if (size == 0xdeadbeef) {
		// double free
		printf("double free!\n");
		*((int *)0) = 0;
	}
	free_list_index = size / 8 + 1;
	block->size = 0xdeadbeef;
	memset(buf, 'c', size);
	heap_quota -= size;
	klib_add_to_free_list(free_list_index, block, 1);
	
	unlock_heap();

    #ifdef DEBUG_FFS
    heap_time += time_now() - t;
    #endif
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


extern bcopy(void* src, void*dst, unsigned n);
void memcpy(void* to, void* from, unsigned n)
{
    bcopy(from, to, n);
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

char* strdup(char* str)
{

    unsigned len = strlen(str)+1;
    char* ret = malloc(len);
    strcpy(ret, str);
    ret[len-1] = '\0';
    return ret;

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

int isprint(int c)
{
    
      if((c > 31) && (c <  127))
      {
         return 1;
      }

	  if ( c == 10 || c == 13 || c == 9) {
		  return 1;
	  }
   
      return 0;
}

void printf(const char* str, ...)
{
	va_list ap;
	va_start(ap, str);
	vprintf(klib_putchar_update_cursor, klib_print, str,ap);
	va_end(ap);
}


#define TTY_BUF_SIZE 512
static char tty_buf[TTY_BUF_SIZE];
static char* tty_buf_pos;
static void _put_buf_c(fputstr _putstr, char c)
{
	*tty_buf_pos = c;
	tty_buf_pos++;
	if ((tty_buf_pos - tty_buf) >= TTY_BUF_SIZE) {
		_putstr(tty_buf);
		tty_buf_pos = tty_buf;
	}
	*tty_buf_pos = '\0';
}

static void _put_buf_str(fputstr _putstr, char* str)
{
	while (*str) {
		_put_buf_c(_putstr, *str);
		str++;
	}
}

static void print_human_readable_size (fputstr _putstr, unsigned int size) 
{
  char* s = 0;

  if (size == 1)
    _put_buf_str (_putstr, "1");
  else 
    {
      static const char *factors[] = {"", "k", "M", "G", "T", NULL};
      const char **fp;

      for (fp = factors; size >= 1024 && fp[1] != NULL; fp++)
        size /= 1024;

	  s = itoa(size, 10, 0);
	  _put_buf_str(_putstr, s);
	  free(s);
	  _put_buf_str(_putstr, *fp);
    }
}

void vprintf(fpputc _putc, fputstr _putstr, const char* src, va_list ap)
{
	int len = strlen(src);
	int i = 0;
    int min_len = 0;
	tty_buf[0] = '\0';
	tty_buf_pos = tty_buf;
    for (i = 0; i < len; i++) {
        char cur = src[i];
        if (cur == '%') {
			cur = src[i+1];
            switch (cur) {
			case '%':
				_put_buf_c(_putstr, src[i+1]);
				break;
			case 'd':
				{
					int arg = va_arg(ap, int);
					char* s = itoa(arg, 10, 1);
					_put_buf_str(_putstr, s);
					free(s);
					break;
				}
			case 'x':
			case 'p':
				{
					int arg = va_arg(ap, int);
					char* s = itoa(arg, 16, 0);
					_put_buf_str(_putstr, s);
					free(s);
					break;
				}
			case 'u':
				{
					int arg = va_arg(ap, int);
					char* s = itoa(arg, 10, 0);
					_put_buf_str(_putstr, s);
					free(s);
					break;
				}
			case 's':
				{
					char* arg = va_arg(ap, char*);
					_put_buf_str(_putstr, arg);
					break;
				}
            case 'h':
                {
                    unsigned arg = va_arg(ap, unsigned);
                    print_human_readable_size(_putstr, arg);
                    break;
                }
			case 'c':
				{
					unsigned char arg = va_arg(ap, unsigned char);
					_put_buf_c(_putstr, arg);
					break;
				}
            case 'b':
                {
                    unsigned char  arg = va_arg(ap, unsigned char );
					char* s = itoa(arg, 16, 0);
                    char* str = (s+2);
                    if (strlen(str) == 1) {
                        _put_buf_str(_putstr, "0"); 
                    }
                    _put_buf_str(_putstr, str); 
					free(s);
					break;
                }
			default:
				{
					_put_buf_c(_putstr,'?');
					break;
				}
            }
			i++;
        }else{
			_put_buf_c(_putstr,cur);
		}
    }

	_putstr(tty_buf);
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
	char* str = malloc(12);
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
		free(str);
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

int atoi(const char *str)
{
	int base = 10;
	int neg = 0;
	int ret = 0;

	if (!str || !*str) {
		return 0;
	}

	if (*str == '-') {
		neg = 1;
		str++;
	}

	// things like -0x will be positive!
	if (*str == '0') {
		neg = 0;
		str++;
		base = (*str == 'x') ? 16 
			: (*str == '0') ? 8
			: (*str == 'b') ? 2 
			: 0;
		str++;
	}

	if (base == 0) {
		return 0;
	}

	while (*str) {
		int cur = 0;

		if ( (base == 8) && (*str > '7' || *str < '0')) {
			break;
		}

		if ( (base == 10) && (*str > '9' || *str < '0')) {
			break;
		}

		if ( (base == 16) && !( (*str >= '0' && *str <= '9') ||
								(*str >= 'a' && *str <= 'f') ||
								(*str >= 'A' && *str <= 'F')
								)
			 ) {
			break;
		}

		if (*str >= '0' && *str <= '9') {
			cur = *str - '0';
		}else if (*str >= 'a' && *str <= 'f') {
			cur = *str - 'a' + 10;
		}else if (*str >= 'A' && *str <= 'F') {
			cur = *str - 'A' + 10;
		}

		ret = ret*base + cur;
		str++;
	}

	if (neg) {
		return (0-ret);
	}else{
		return ret;
	}
}

void srand(unsigned _seed)
{
	klib_random_num = _seed;
}

unsigned int rand()
{
	unsigned int ret = klib_random_num * 0x343fd;
	ret += 0x269EC3;
	klib_random_num = ret;
	ret = klib_random_num;
	ret >>= 0x10;
	ret = ret & 0x7fff;
	return ret;
}




#ifdef __KERNEL__


static TTY_COLOR cur_fg_color = clWhite;
static TTY_COLOR cur_bg_color = clBlack;

int tty_ioctl(tty_command* cmd)
{
	switch (cmd->cmd_id) {
	case tty_cmd_get_color:
		cmd->color.fg_color = cur_fg_color;
		cmd->color.bg_color = cur_bg_color;
		break;
	case tty_cmd_set_color:
		cur_fg_color = cmd->color.fg_color;
		cur_bg_color = cmd->color.bg_color;
		break;
	case tty_cmd_get_curse:
		cmd->curse.cx = CUR_ROW;
		cmd->curse.cy = CUR_COL;
		break;
	case tty_cmd_set_curse:
		cursor = ROW_COL_TO_CUR(cmd->curse.cx, cmd->curse.cy);
		break;
	default:
		return -1;
	}

	return 1;
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
	vprintf(klib_putchar_update_cursor, klib_print, str,ap);
	va_end(ap);
    unlock_tty();
}


void klog_printf(const char* str, ...)
{
	va_list ap;

	va_start(ap, str);
	vprintf(klog_write, klog_writestr, str,ap);
	va_end(ap);
}

void klog(char* str, ...)
{
	va_list ap;
    char* str_mill;
    int len = 0;
    int i = 0;
	time_t time;
    task_struct* cur = 0;

	cur = CURRENT_TASK();

	sema_wait(&klog_lock);

	timer_current(&time);
    str_mill = itoa(time.milliseconds, 10, 0);
    len = strlen(str_mill);
    len = 3 - len;
	klog_printf("[%d: %d.", cur->psid, time.seconds );
    for (i = 0; i < len; i++) {
        klog_printf("0");
    }
    klog_writestr(str_mill);
	klog_writestr("]");
    kfree(str_mill);
    va_start(ap, str); 
	vprintf(klog_write, klog_writestr, str,ap);
	va_end(ap);

	sema_trigger(&klog_lock);
}

void tty_write(const char* buf, unsigned len)
{
	int i = 0;
    int new_pos;
	lock_tty();
	for (i = 0; i < len; i++){
		//tty_setcolor(CUR_ROW, CUR_COL, cur_fg_color, cur_bg_color);
        new_pos = klib_putchar(buf[i]);
        klib_update_cursor(new_pos);
		//klib_putchar_update_cursor(buf[i]);
	}
    klib_flush_cursor();
	unlock_tty();
}


void reboot()
{
	klog_close();
	block_close();
	_write_port(0x64, 0xfe);
}


void shutdown()
{
  const char s[] = "Shutdown";
  const char *p;

  printf ("Powering off\n");
  printf ("Close klog\n");
  klog_close();
  printf ("Flush block cache\n");
  block_close();

  /* 	This is a special power-off sequence supported by Bochs and
		QEMU, but not by physical hardware. */
  printf ("Power off...\n");
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
#endif

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

