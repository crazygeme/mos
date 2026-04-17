#include <ps/ps.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include <lib/klib.h>

struct user_desc {
	unsigned int entry_number;
	unsigned int base_addr;
	unsigned int limit;
	unsigned int seg_32bit : 1;
	unsigned int contents : 2;
	unsigned int read_exec_only : 1;
	unsigned int limit_in_pages : 1;
	unsigned int seg_not_present : 1;
	unsigned int useable : 1;
	unsigned int empty : 25;
};

extern unsigned long long gdt[];

static void decode_tls_desc(unsigned long long desc, struct user_desc *u)
{
	unsigned int base_mid = (unsigned int)((desc >> 32) & 0xff);
	unsigned int type = (unsigned int)((desc >> 40) & 0x0f);

	memset(u, 0, sizeof(*u));
	u->base_addr = ((unsigned int)(desc >> 16) & 0xffff) |
		       (base_mid << 16) | ((unsigned int)(desc >> 56) << 24);
	u->limit = ((unsigned int)desc & 0xffff) |
		   (((unsigned int)(desc >> 48) & 0x0f) << 16);
	u->seg_32bit = 1;
	u->contents = (type >> 2) & 0x1;
	u->read_exec_only = (type & 0x2) ? 0 : 1;
	u->limit_in_pages = ((unsigned int)(desc >> 55) & 0x1);
	u->seg_not_present = (((unsigned int)(desc >> 47) & 0x1) == 0);
	u->useable = ((unsigned int)(desc >> 54) & 0x1);
}

static unsigned long long build_tls_desc(struct user_desc *u)
{
	unsigned int type;
	unsigned int base = u->base_addr;
	unsigned int limit = u->limit;
	unsigned int low, high;

	if (u->contents >= 2)
		type = 8 | ((u->contents & 1) << 2) |
		       (u->read_exec_only ? 0 : 2);
	else
		type = (u->contents << 2) | (u->read_exec_only ? 0 : 2);

	type |= 1;

	low = (limit & 0xffff) | ((base & 0xffff) << 16);
	high = ((base >> 16) & 0xff) | (type << 8) | (SEG_CLASS_DATA << 12) |
	       (USER_PRIVILEGE << 13) | (1 << 15) |
	       (((limit >> 16) & 0x0f) << 16) | ((u->useable & 0x1) << 20) |
	       (1 << 22) | ((u->limit_in_pages & 0x1) << 23) |
	       (base & 0xff000000);

	return (unsigned long long)low | ((unsigned long long)high << 32);
}

static int user_desc_empty(const struct user_desc *u)
{
	return u->entry_number == 0 && u->base_addr == 0 && u->limit == 0 &&
	       u->seg_32bit == 0 && u->contents == 0 &&
	       u->read_exec_only == 1 && u->limit_in_pages == 0 &&
	       u->seg_not_present == 1 && u->useable == 0;
}

static unsigned long long build_ldt_desc(const struct user_desc *u)
{
	unsigned int type;
	unsigned int base = u->base_addr;
	unsigned int limit = u->limit;
	unsigned int low, high;

	if (u->contents >= 2)
		type = 8 | ((u->contents & 1) << 2) |
		       (u->read_exec_only ? 0 : 2);
	else
		type = (u->contents << 2) | (u->read_exec_only ? 0 : 2);

	low = (limit & 0xffff) | ((base & 0xffff) << 16);
	high = ((base >> 16) & 0xff) | (type << 8) | (SEG_CLASS_DATA << 12) |
	       (USER_PRIVILEGE << 13) | ((u->seg_not_present ? 0 : 1) << 15) |
	       (((limit >> 16) & 0x0f) << 16) | ((u->useable & 0x1) << 20) |
	       ((u->seg_32bit & 0x1) << 22) |
	       ((u->limit_in_pages & 0x1) << 23) | (base & 0xff000000);

	return (unsigned long long)low | ((unsigned long long)high << 32);
}

int ps_set_thread_area_for(task_struct *task, void *info)
{
	unsigned int entry;
	struct user_desc *u_info = (struct user_desc *)info;

	if (!task || !task->user || !u_info)
		return -EFAULT;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("set_thread_area(entry=%d, base=%x)\n",
		     u_info->entry_number, u_info->base_addr);

	entry = u_info->entry_number;

		if (entry == (unsigned int)-1) {
		for (entry = GDT_ENTRY_TLS_MIN; entry <= GDT_ENTRY_TLS_MAX;
		     entry++) {
			if (!task->user->tls_desc[entry - GDT_ENTRY_TLS_MIN])
				break;
		}
		if (entry > GDT_ENTRY_TLS_MAX)
			return -ESRCH;
		u_info->entry_number = entry;
	}

	if (entry < GDT_ENTRY_TLS_MIN || entry > GDT_ENTRY_TLS_MAX)
		return -EINVAL;

	task->user->tls_desc[entry - GDT_ENTRY_TLS_MIN] = build_tls_desc(u_info);
	if (task == CURRENT_TASK())
		gdt[entry] = task->user->tls_desc[entry - GDT_ENTRY_TLS_MIN];
	return 0;
}

int sys_set_thread_area(void *info)
{
	return ps_set_thread_area_for(CURRENT_TASK(), info);
}

int sys_get_thread_area(void *info)
{
	task_struct *cur = CURRENT_TASK();
	struct user_desc *u_info = (struct user_desc *)info;
	unsigned int entry;

	if (!u_info)
		return -EFAULT;

	entry = u_info->entry_number;
	if (entry < GDT_ENTRY_TLS_MIN || entry > GDT_ENTRY_TLS_MAX)
		return -EINVAL;

	decode_tls_desc(cur->user->tls_desc[entry - GDT_ENTRY_TLS_MIN], u_info);
	u_info->entry_number = entry;
	return 0;
}

int sys_modify_ldt(int func, void *ptr, unsigned long bytecount)
{
	task_struct *cur = CURRENT_TASK();
	struct user_desc *u_info = (struct user_desc *)ptr;
	unsigned long copy_len;

	switch (func) {
	case 0:
		if (!ptr)
			return -EFAULT;
		copy_len = bytecount;
		if (copy_len > sizeof(cur->user->ldt_desc))
			copy_len = sizeof(cur->user->ldt_desc);
		memcpy(ptr, cur->user->ldt_desc, copy_len);
		return (int)copy_len;
	case 2:
		if (!ptr)
			return -EFAULT;
		memset(ptr, 0, bytecount);
		return (int)bytecount;
	case 1:
	case 0x11:
		if (!u_info)
			return -EFAULT;
		if (bytecount != sizeof(*u_info))
			return -EINVAL;
		if (u_info->entry_number >= LDT_ENTRY_COUNT)
			return -EINVAL;
		if (!user_desc_empty(u_info)) {
			if (!u_info->seg_32bit || u_info->contents == 3)
				return -EINVAL;
			cur->user->ldt_desc[u_info->entry_number] =
				build_ldt_desc(u_info);
		} else {
			cur->user->ldt_desc[u_info->entry_number] = 0;
		}
		ps_update_ldt(cur);
		return 0;
	default:
		return -ENOSYS;
	}
}
