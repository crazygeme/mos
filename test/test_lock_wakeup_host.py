#!/usr/bin/env python3
"""Validate lock wakeups competing with scheduler queue transitions."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/lib/impl/lock.c').read_text()
start = source.index('static int lock_wake_one_unsafe(')
end = source.index('/* Avoid an atomic write', start)
wake = source[start:end]
lists = (ROOT / 'src/lib/impl/list.c').read_text()
lists = '\n'.join(line for line in lists.splitlines() if not line.startswith('#include'))
# Deliver a competing poll wake after the dequeue has selected its entry.
lists = lists.replace('entry = head->next;\n', 'entry = head->next;\n\tcompeting_wake();\n')
preamble = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <lib/list.h>
typedef struct { int held; } spinlock_t;
typedef struct task_struct task_struct;
typedef struct { task_struct *task; list_entry ps_list; } task_schedule;
struct task_struct { task_schedule *sched; int ready; };
static spinlock_t ps_lock;
static list_entry ready_queue, wait_queue;
static task_struct *racer;
static unsigned race, transitions;
static void ps_put_to_ready_queue_unsafe(task_struct *task);
static void competing_wake(void)
{
    if (!race || ps_lock.held) return;
    race = 0;
    ps_lock.held = 1;
    ps_put_to_ready_queue_unsafe(racer);
    ps_lock.held = 0;
}
static void spinlock_lock(spinlock_t *lock, int *irq)
{
    assert(lock == &ps_lock && !lock->held);
    competing_wake();
    *irq = 1; lock->held = 1;
}
static void spinlock_unlock(spinlock_t *lock, int irq)
{
    assert(lock == &ps_lock && lock->held && irq == 1);
    lock->held = 0;
}
'''
scheduler = r'''
static void ps_put_to_ready_queue_unsafe(task_struct *task)
{
    assert(ps_lock.held);
    if (task->ready) return;
    list_remove_entry(&task->sched->ps_list);
    list_insert_tail(&ready_queue, &task->sched->ps_list);
    task->ready = 1; transitions++;
}
void ps_put_to_ready_queue(task_struct *task)
{
    int irq;
    spinlock_lock(&ps_lock, &irq);
    ps_put_to_ready_queue_unsafe(task);
    spinlock_unlock(&ps_lock, irq);
}
'''
checks = r'''
static task_schedule schedules[2];
static task_struct tasks[2];
static void reset(void)
{
    ps_lock.held = 0; race = transitions = 0; racer = &tasks[0];
    list_init(&ready_queue); list_init(&wait_queue);
    for (unsigned i = 0; i < 2; i++) {
        tasks[i] = (task_struct){&schedules[i], 0};
        schedules[i].task = &tasks[i];
        list_init(&schedules[i].ps_list);
        list_insert_tail(&wait_queue, &schedules[i].ps_list);
    }
}
static unsigned verify(list_entry *head)
{
    unsigned count = 0;
    for (list_entry *node = head->next; node != head; node = node->next) {
        assert(++count <= 2);
        assert(node->prev->next == node && node->next->prev == node);
    }
    assert(head->next->prev == head && head->prev->next == head);
    return count;
}
int main(void)
{
    reset();
    assert(lock_wake_one_unsafe(&wait_queue) == 1);
    assert(tasks[0].ready && !tasks[1].ready);
    assert(verify(&ready_queue) == 1 && verify(&wait_queue) == 1);
    assert(lock_wake_one_unsafe(&wait_queue) == 1);
    assert(verify(&ready_queue) == 2 && verify(&wait_queue) == 0);
    assert(lock_wake_one_unsafe(&wait_queue) == 0);
    assert(transitions == 2 && !ps_lock.held);
    reset(); race = 1;
    assert(lock_wake_one_unsafe(&wait_queue) == 1);
    assert(tasks[0].ready && tasks[1].ready);
    assert(verify(&ready_queue) == 2 && verify(&wait_queue) == 0);
    assert(transitions == 2 && !ps_lock.held);
    assert(lock_wake_one_unsafe(&wait_queue) == 0);
    puts("Lock wakeup queue and competing poll wake validation passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='mos-lock-wakeup-') as directory:
    probe = Path(directory) / 'probe.c'
    binary = Path(directory) / 'probe'
    probe.write_text(preamble + lists + scheduler + wake + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-I', str(ROOT / 'src'),
                    str(probe), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
