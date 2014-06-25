#include <int/int.h>
#include <mm/pagefault.h>
#include <mm/mm.h>
#include <ps/ps.h>

static void pf_process(intr_frame* frame);

void pf_init()
{
	int_register(0xe, pf_process, 0, 0);

}

static void pf_process(intr_frame* frame)
{
    unsigned cr2;
    unsigned cr3;
    task_struct* cur = CURRENT_TASK();
    __asm__("movl %cr2, %eax");
    __asm__("movl %%eax, %0" : "=m"(cr2));
    __asm__("movl %%cr3, %0" : "=q"(cr3));
    printk("page fault! error code %x, address %x, eip %x\n", frame->error_code, cr2, frame->eip);
    printk("cur->user.page_dir %x, cr3 %x\n", cur->user.page_dir, cr3);

	for(;;)
    {
        __asm__("hlt");
    }
}
