#!/usr/bin/env python3
"""Validate ordered PS/2 input delivery across concurrent controller readers."""
from pathlib import Path
import subprocess
import re
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'src/device/ps2/i8042.c').read_text()

def function(name):
    start = re.search(r'(?:static )?(?:void|int) ' + name + r'\(', source).start()
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

preamble = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#define PS2_PORT_AUX 1
#define PAUSE() sched_yield()
#define I8042_STATUS 0x64
#define I8042_DATA 0x60
#define I8042_STATUS_OBF 1
#define I8042_STATUS_AUX 0x20
#define SPINLOCK_INITIALIZER {0}
typedef struct { int locked; } spinlock_t;
static unsigned command_busy[2];
static spinlock_t input_lock = SPINLOCK_INITIALIZER;
static int parent_pid, observed_status, contender_read, contended;
static unsigned next_byte, received, input[4];
static int shift_down, contender_mode;
static void wait_for(volatile int *flag)
{ while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) sched_yield(); }
static int int_intr_disable(void) { return 1; }
static void int_intr_setlevel(int irq) { (void)irq; }
static void spinlock_lock(spinlock_t *lock, int *irq)
{
 *irq = int_intr_disable();
 while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
  __atomic_store_n(&contended, 1, __ATOMIC_RELEASE); sched_yield();
 }
}
static void spinlock_unlock(spinlock_t *lock, int irq)
{ __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE); int_intr_setlevel(irq); }
static unsigned char port_read_byte(unsigned port)
{
 if (port == I8042_STATUS) {
  unsigned status = next_byte < 2 ? I8042_STATUS_OBF : 0;
  if (getpid() == parent_pid && !observed_status) {
   __atomic_store_n(&observed_status, 1, __ATOMIC_RELEASE);
   while (!__atomic_load_n(&contender_read, __ATOMIC_ACQUIRE) &&
          !__atomic_load_n(&contended, __ATOMIC_ACQUIRE)) sched_yield();
  }
  return status;
 }
 assert(port == I8042_DATA);
 unsigned index = next_byte++;
 if (getpid() != parent_pid && index == 0) {
  __atomic_store_n(&contender_read, 1, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&received, __ATOMIC_ACQUIRE)) sched_yield();
 }
 return index == 0 ? 0x2a : index == 1 ? 0xaa : 0xff;
}
static void ps2_receive(unsigned port, unsigned char byte)
{
 assert(port == 0);
 input[received] = byte;
 if (byte == 0x2a) shift_down = 1;
 if (byte == 0xaa) shift_down = 0;
 __atomic_add_fetch(&received, 1, __ATOMIC_RELEASE);
}
'''
code = ''
if 'static void ps2_drain_input_unsafe(' in source:
    code += function('ps2_drain_input_unsafe') + '\n'
code += function('ps2_drain_input') + '\n'
code += function('ps2_read_reply') + '\n'
code += function('ps2_discard_input')
tests = r'''
static int contender(void *unused)
{
 (void)unused; wait_for(&observed_status);
 if (contender_mode == 0) ps2_drain_input();
 else if (contender_mode == 1) {
  unsigned char reply; assert(ps2_read_reply(PS2_PORT_AUX, &reply, 16)==-ETIMEDOUT);
 } else ps2_discard_input(PS2_PORT_AUX);
 return 0;
}
int main(void)
{
 alarm(10); parent_pid=getpid();
 void *stack=malloc(1024*1024); assert(stack);
 int failures=0;
 for (contender_mode=0; contender_mode<3; contender_mode++) {
 observed_status=contender_read=contended=0;
 next_byte=received=shift_down=0;
 int child=clone(contender, (char *)stack+1024*1024, CLONE_VM|SIGCHLD, NULL);
 assert(child>0); ps2_drain_input();
 int status; assert(waitpid(child,&status,0)==child && status==0);
 printf("reader=%d received=%u bytes=%02x,%02x shift_down=%d\n",contender_mode,received,input[0],input[1],shift_down); fflush(stdout);
 failures += !(received==2 && input[0]==0x2a && input[1]==0xaa && !shift_down);
 }
 assert(!failures);
 free(stack); puts("PS/2 concurrent input ordering checks passed"); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='mos-ps2-controller-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(preamble + code + tests)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wno-unused-function',
                    '-Wno-unused-variable', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
