#!/usr/bin/env python3
"""Validate idle readiness across exit wakeups, affinity, and active CPUs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/ps/impl/alg/ps_alg_rr.c').read_text()
start = source.index('static int ps_task_available(')
end = source.index('/*\n * Timer queue helpers', start)
functions = source[start:end]
lists = (root / 'src/lib/impl/list.c').read_text()
lists = '\n'.join(line for line in lists.splitlines() if not line.startswith('#include'))
preamble = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <lib/list.h>
enum { ps_idle, ps_normal, PS_PRIORITY_MAX };
enum { ps_ready, ps_running, ps_waiting, ps_dying };
typedef struct task_struct task_struct;
typedef struct { task_struct *task; unsigned status, terminate_requested, vm_lock_depth, on_cpu, affinity_mask, priority; list_entry ps_list; } task_schedule;
typedef struct { void *param; } task_life;
struct task_struct { task_schedule *sched; task_life *life; };
static struct { list_entry ready_queue[PS_PRIORITY_MAX]; } control;
static task_struct *current;
static unsigned cpu;
static int ps_lock;
static unsigned smp_cpu_id(void) { return cpu; }
static void spinlock_lock(int *p, int *irq) { assert(!*p); *p=1; *irq=0; }
static void spinlock_unlock(int *p, int irq) { assert(*p && irq==0); *p=0; }
'''
checks = r'''
int main(void)
{
 task_life life={0}; task_schedule idle_s={.status=ps_running,.priority=ps_idle}, ready_s={.status=ps_ready,.priority=ps_normal};
 task_struct idle={&idle_s,&life}, ready={&ready_s,&life};
 current=&idle; idle_s.task=&idle; ready_s.task=&ready;
 for(unsigned i=0;i<PS_PRIORITY_MAX;i++) list_init(&control.ready_queue[i]);
 list_insert_tail(&control.ready_queue[ps_idle],&idle_s.ps_list);
 assert(!ps_has_ready_task());
 list_insert_tail(&control.ready_queue[ps_normal],&ready_s.ps_list);
 assert(ps_has_ready_task());
 assert(control.ready_queue[ps_normal].next==&ready_s.ps_list);
 ready_s.on_cpu=2; assert(!ps_has_ready_task()); ready_s.on_cpu=0;
 ready_s.affinity_mask=2; assert(!ps_has_ready_task()); cpu=1; assert(ps_has_ready_task());
 ready_s.terminate_requested=1; assert(!ps_has_ready_task());
 ready_s.vm_lock_depth=1; assert(ps_has_ready_task());
 ready_s.status=ps_waiting; assert(!ps_has_ready_task());
 ready_s.status=ps_dying; assert(!ps_has_ready_task());
 ready_s.status=ps_ready; assert(ps_get_available_ready_task(&control.ready_queue[ps_normal])==&ready);
 puts("Idle readiness and exit wakeup eligibility: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='mos-idle-ready-') as directory:
    probe = Path(directory) / 'probe.c'
    binary = Path(directory) / 'probe'
    probe.write_text(preamble + lists + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Werror', '-I', str(root / 'src'), str(probe), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
