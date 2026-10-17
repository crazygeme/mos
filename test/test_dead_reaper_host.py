#!/usr/bin/env python3
"""Validate isolated thread cleanup and reaper wait notification."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/ps/impl/ps_syscall.c').read_text()

def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

lists = (ROOT / 'src/lib/impl/list.c').read_text()
lists = '\n'.join(line for line in lists.splitlines() if not line.startswith('#include'))
preamble = r'''
#include <assert.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdio.h>
#include <lib/list.h>
typedef struct task_struct task_struct;
typedef struct { task_struct *task; unsigned on_cpu, enumeration_refs; list_entry ps_list; } task_schedule;
struct task_struct { task_schedule *sched; };
typedef struct { int held; } spinlock_t;
static spinlock_t ps_lock;
static task_schedule reaper_schedule, victim_schedule;
static task_struct reaper = {&reaper_schedule}, victim = {&victim_schedule};
static task_struct *current, *dead_reaper = &reaper;
static int dead_reaper_waiting;
static list_entry dead_threads;
static unsigned cleaned, delays, waits, wakes;
static jmp_buf idle;
static void spinlock_lock(spinlock_t *lock, int *irq)
{ assert(!lock->held); *irq = 1; lock->held = 1; }
static void spinlock_unlock(spinlock_t *lock, int irq)
{ assert(lock->held && irq == 1); lock->held = 0; }
static void ps_reap_group_thread(task_struct *task)
{
    assert(current == &reaper && task == &victim && !ps_lock.held);
    assert(!task->sched->on_cpu && !task->sched->enumeration_refs);
    assert(task->sched->ps_list.next == &task->sched->ps_list);
    /* Cleanup owns no caller's device transaction or timed wait. */
    assert(!dead_reaper_waiting); cleaned++;
}
static void ps_put_to_wait_queue_unsafe(task_struct *task, list_entry *queue)
{ assert(ps_lock.held && task == &reaper && queue == NULL); waits++; }
static void ps_put_to_ready_queue_unsafe(task_struct *task)
{ assert(ps_lock.held && task == &reaper); wakes++; }
static void task_sched(void)
{ assert(!ps_lock.held && dead_reaper_waiting); longjmp(idle, 1); }
static void time_wait(unsigned ms)
{
    assert(current == &reaper && !ps_lock.held && ms == 1);
    delays++; victim_schedule.on_cpu = victim_schedule.enumeration_refs = 0;
}
'''
checks = r'''
static void reset(void)
{
    cleaned = delays = waits = wakes = 0; dead_reaper_waiting = 0;
    list_init(&dead_threads); list_init(&victim_schedule.ps_list);
    victim_schedule.task = &victim; current = &reaper;
}
static void run(void) { if (!setjmp(idle)) ps_dead_reaper(NULL); }
int main(void)
{
    reset(); run();
    assert(!cleaned && !delays && waits == 1 && dead_reaper_waiting);
    int irq; spinlock_lock(&ps_lock, &irq);
    ps_notify_reaper_unsafe(); ps_notify_reaper_unsafe();
    spinlock_unlock(&ps_lock, irq);
    assert(wakes == 1 && !dead_reaper_waiting);
    ps_notify_reaper_unsafe(); assert(wakes == 1);

    reset(); victim_schedule.on_cpu = 1;
    list_insert_tail(&dead_threads, &victim_schedule.ps_list);
    run(); assert(cleaned == 1 && delays == 1 && waits == 1);
    assert(list_is_empty(&dead_threads));

    reset(); victim_schedule.enumeration_refs = 1;
    list_insert_tail(&dead_threads, &victim_schedule.ps_list);
    run(); assert(cleaned == 1 && delays == 1 && waits == 1);
    assert(list_is_empty(&dead_threads));
    puts("Isolated thread reaper and wait notification validation passed");
}
'''
code = (preamble + lists + function('static void ps_reap_dead_threads(')
        + function('static void ps_notify_reaper_unsafe(')
        + function('static void ps_dead_reaper(') + checks)
with tempfile.TemporaryDirectory(prefix='mos-dead-reaper-') as directory:
    probe = Path(directory) / 'probe.c'
    binary = Path(directory) / 'probe'
    probe.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-I', str(ROOT / 'src'),
                    str(probe), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
