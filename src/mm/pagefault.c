#include <int.h>
#include <pagefault.h>
#include <mm.h>
#include <ps.h>
#include <mmap.h>
#include <phymm.h>
#include <fs.h>
#include <klib.h>
#include <time.h>
#include <macro.h>

unsigned page_fault_cow = 0;
unsigned page_fault_invalid = 0;
unsigned page_fault_file = 0;
unsigned page_fault_file_read = 0;
unsigned page_fault_perm = 0;
unsigned long long page_fault_cow_spent = 0;
unsigned long long page_fault_invalid_spent = 0;
unsigned long long page_fault_file_spent = 0;
unsigned long long page_fault_perm_spent = 0;

static void pf_process(intr_frame *frame);
void pf_init()
{
	int_register(0xe, pf_process, 0, 0);
}

/*
31                4                             0
+-----+-...-+-----+-----+-----+-----+-----+-----+
|     Reserved    | I/D | RSVD| U/S | W/R |  P  |
+-----+-...-+-----+-----+-----+-----+-----+-----+

*/
#define PF_MASK_P 0x00000001 // page valid
#define PF_MASK_RW 0x00000002 // write access
#define PF_MASK_US 0x00000004 // user mode access
#define PF_MASK_RSVD 0x00000008
extern phymm_page *phymm_pages;
static void pf_process(intr_frame *frame)
{
	unsigned cr2;
	unsigned error = frame->error_code;
	task_struct *cur = CURRENT_TASK();
	unsigned this_begin;
	unsigned vir;
	unsigned tmp;
	int page_valid;
	int write_access;
	int user_mode;
	unsigned oldint;
	unsigned long long begin = time_now_us();

	oldint = int_intr_disable();

	LOAD_CR2(cr2);

	page_valid = ((error & PF_MASK_P) == PF_MASK_P);
	write_access = ((error & PF_MASK_RW) == PF_MASK_RW);
	user_mode = ((error & PF_MASK_US) == PF_MASK_US);

	if (!page_valid) {
		vm_region *region;
		unsigned this_offset;
		filep f;
		ext4_file *ff;
		size_t rcnt = 0;

		this_begin = cr2 & PAGE_SIZE_MASK;
		region = vm_find_map(cur->user.vm, this_begin);
		if (region) {
			f = region->node;
			this_offset =
				region->offset + (this_begin - region->begin);
			mm_add_dynamic_map(this_begin, 0, PAGE_ENTRY_USER_DATA);
			RELOAD_CR3();

			if (f != 0) {
				int ret = 0;

				page_fault_file++;
				ff = f->inode;
				ret = ext4_fseek(ff, this_offset, SEEK_SET);
				if (ret != EOK) {
					klog("FAIL: mmap: seek to off %x\n",
					     this_offset);
				}
				ret = ext4_fread(ff, this_begin, PAGE_SIZE,
						 &rcnt);
				if (ret != EOK) {
					klog("FAIL: mmap: read to buffer %x, size %x\n",
					     this_begin, PAGE_SIZE);
				}
				page_fault_file_read += rcnt;
				page_fault_file_spent +=
					(time_now_us() - begin);
			} else {
				page_fault_invalid++;
				memset(this_begin, 0, PAGE_SIZE);
				page_fault_invalid_spent +=
					(time_now_us() - begin);
			}

			goto Done;
		} else if (user_mode) {
			sys_exit(-1);
		} else {
			// it must be a kernel bug!
			goto HLT;
		}
	} else if (write_access) {
		unsigned page_index = mm_get_attached_page_index(cr2);
		unsigned cow;
		unsigned flag;
		unsigned new_mem = 0;

		cow = phymm_is_cow(page_index);
		if (cow) {
			page_fault_cow++;

			vir = cr2;
			vir = (vir & PAGE_SIZE_MASK);
			// 1. alloc a new physical memory
			new_mem = vm_alloc(1);

			// 2. copy cow value
			memcpy(new_mem, vir, PAGE_SIZE);

			// 3. unmap origin address
			flag = mm_get_map_flag(vir);
			mm_del_dynamic_map(vir);

			// 4. unmap newly allocated physical memory
			// note that should ref it first so that on one will use
			// this physical memory
			phymm_reference_page(VIRT_TO_PAGE_IDX(new_mem));
			mm_del_direct_map(new_mem);

			// 5. map newly allocated physical memory to origin virtual address
			flag |= PAGE_ENTRY_PRESENT;
			flag |= PAGE_ENTRY_WRITABLE;
			mm_add_dynamic_map(vir, VIRT_TO_PHY(new_mem), flag);

			phymm_dereference_page(VIRT_TO_PAGE_IDX(new_mem));

			RELOAD_CR3();

			page_fault_cow_spent += (time_now_us() - begin);
		} else {
			page_fault_perm++;
			vir = cr2;
			vir = (vir & PAGE_SIZE_MASK);
			flag = mm_get_map_flag(vir);
			flag |= PAGE_ENTRY_WRITABLE;
			mm_set_map_flag(vir, flag);
			RELOAD_CR3();

			page_fault_perm_spent += (time_now_us() - begin);
		}

		goto Done;
	} else {
		goto HLT;
	}

HLT:
	do {
		task_struct *cur = CURRENT_TASK();
		printk("[%d]page fault! error code %x, address %x, eip %x\n",
		       cur->psid, frame->error_code, cr2, frame->eip);
	} while (0);

	DIE();

Done:
	if (oldint) {
		int_intr_enable();
	}
}
