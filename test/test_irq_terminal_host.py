#!/usr/bin/env python3
"""Validate PIC acknowledgement before terminal task stack abandonment."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/int/impl/int.c').read_text()
start = source.index('void intr_handler(intr_frame *frame)')
end = source.index('\nvoid intr_syscall_handler', start)
body = source[start:end]
harness = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#define IDT_SIZE 256
#define SHARED_IRQ_HANDLERS 4
#define INT_VECTOR_IRQ0 32
typedef struct {int vec_no;} intr_frame;
typedef void (*int_callback)(intr_frame *);
static int_callback in_callbacks[IDT_SIZE],irq_callbacks[16][4];
static struct {struct {int remain_ticks;} *sched;} task;
#define current (&task)
static jmp_buf terminal;
static int terminating,handled,acknowledged,sequence,special;
static int smp_interrupt(intr_frame *f){(void)f;return special;}
static void smp_check_stop(void){if(terminating)longjmp(terminal,1);}
static void ps_account_tick(intr_frame *f){(void)f;}
static int ps_enabled(void){return 0;}
static void pic_end_of_interrupt(int vector){assert(vector>=32&&vector<48);acknowledged=++sequence;}
static void callback(intr_frame *f){(void)f;handled=++sequence;}
static void intr_sanitize_user_return(intr_frame *f){(void)f;}
static void ps_alarm_tick(void){}
static void net_service_tick(void){}
static void intr_maybe_preempt(void){}
static void intr_prepare_user_return(intr_frame *f){(void)f;}
'''
main = r'''
int main(void){
 for(int vector=32;vector<48;vector++){
  intr_frame f={vector};in_callbacks[vector]=callback;
  irq_callbacks[vector-32][0]=callback;
  for(int stop=0;stop<2;stop++){
   terminating=stop;handled=acknowledged=sequence=0;
   if(!setjmp(terminal)){intr_handler(&f);assert(!stop);}
   assert(handled==2&&acknowledged==3);
  }
 }
 intr_frame f={6};terminating=1;handled=acknowledged=sequence=0;
 in_callbacks[6]=callback;
 if(!setjmp(terminal)){intr_handler(&f);assert(0);}
 assert(!handled&&!acknowledged);
 puts("PIC acknowledgement and terminal task ordering validation passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mos-irq-terminal-') as temporary:
    path=Path(temporary)
    (path/'test.c').write_text(harness+body+main)
    subprocess.run(['gcc','-O2','-Wall',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=5)
