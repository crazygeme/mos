#include <int/int.h>
#include <mm/pagefault.h>
#include <mm/mm.h>

static void pf_process(intr_frame* frame);

void pf_init()
{
	int_register(0xe, pf_process, 0, 0);

}

static void pf_process(intr_frame* frame)
{
    unsigned cr2;
    __asm__("movl %cr2, %eax");
    __asm__("movl %%eax, %0" : "=m"(cr2));
    printk("page fault! error code %x, address %x, eip %x\n", frame->error_code, cr2, frame->eip);
    __asm__("hlt");
	for(;;);
}
