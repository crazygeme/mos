#!/usr/bin/env python3
"""Validate task timer replacement and terminal-state timer removal."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

scheduler = (ROOT / 'src/ps/impl/alg/ps_alg_rr.c').read_text()
process = (ROOT / 'src/ps/impl/ps.c').read_text()
header = (ROOT / 'src/lib/rbtree.h').read_text()
rbtree = (ROOT / 'src/lib/impl/rbtree.c').read_text()
header = '\n'.join(line for line in header.splitlines() if not line.startswith('#include'))
rbtree = '\n'.join(line for line in rbtree.splitlines() if not line.startswith('#include'))
preamble = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
'''
model = r'''
typedef struct task_struct task_struct;
typedef struct { task_struct *task; struct rb_node timer_rb; unsigned long long timer_due_ms; } task_wait;
typedef struct { unsigned live_member, status; } task_schedule;
typedef struct { unsigned live_tasks; } task_thread;
struct task_struct { task_wait *wait; task_schedule *sched; task_thread *thread; };
static struct { struct rb_root timer_queue; } control;
static unsigned reparented;
static unsigned long long clock_ms;
enum { ps_dying = 4 };
static unsigned long long time_deadline_ms(unsigned ms) { return clock_ms + ms; }
static void ps_reparent_children_unsafe(task_struct *task) { (void)task; reparented++; }
void timer_disarm_unsafe(task_struct *task);
'''
checks = r'''
#define COUNT 48
static task_struct tasks[COUNT];
static task_wait waits[COUNT];
static task_schedule schedules[COUNT];
static task_thread group;
static unsigned seen[COUNT];
static unsigned visit(struct rb_node *node, struct rb_node *parent,
                      unsigned long long min, unsigned long long max)
{
    if (!node) return 1;
    task_wait *wait = rb_entry(node, task_wait, timer_rb);
    ptrdiff_t index = wait - waits;
    assert(index >= 0 && index < COUNT && !seen[index]++);
    assert(rb_parent(node) == parent);
    assert(wait->timer_due_ms >= min && wait->timer_due_ms <= max);
    if (rb_is_red(node)) {
        assert(!node->rb_left || rb_is_black(node->rb_left));
        assert(!node->rb_right || rb_is_black(node->rb_right));
    }
    unsigned left = visit(node->rb_left, node, min, wait->timer_due_ms);
    unsigned right = visit(node->rb_right, node, wait->timer_due_ms, max);
    assert(left == right);
    return left + rb_is_black(node);
}
static unsigned verify(void)
{
    memset(seen, 0, sizeof(seen));
    if (control.timer_queue.rb_node) assert(rb_is_black(control.timer_queue.rb_node));
    visit(control.timer_queue.rb_node, NULL, 0, UINT64_MAX);
    unsigned count = 0;
    for (unsigned i = 0; i < COUNT; i++) count += seen[i];
    return count;
}
int main(void)
{
    group.live_tasks = COUNT;
    for (unsigned i = 0; i < COUNT; i++) {
        tasks[i] = (task_struct){&waits[i], &schedules[i], &group};
        waits[i].task = &tasks[i]; schedules[i].live_member = 1;
        rb_init_node(&waits[i].timer_rb);
        timer_arm_unsafe(&tasks[i], 100 + i);
    }
    assert(verify() == COUNT);
    for (unsigned round = 0; round < 128; round++) {
        clock_ms++;
        for (unsigned i = 0; i < COUNT; i++) {
            unsigned index = (i * 17 + round) % COUNT;
            unsigned delay = (round * 13 + i * 11) % 127 + 1;
            timer_arm_unsafe(&tasks[index], delay);
            assert(waits[index].timer_due_ms == clock_ms + delay);
            assert(verify() == COUNT);
        }
    }
    for (unsigned i = 0; i < COUNT; i++) {
        ps_mark_dying_unsafe(&tasks[i]);
        assert(RB_EMPTY_NODE(&waits[i].timer_rb));
        assert(!waits[i].timer_due_ms && schedules[i].status == ps_dying);
        assert(group.live_tasks == COUNT - i - 1);
        assert(verify() == COUNT - i - 1);
        ps_mark_dying_unsafe(&tasks[i]);
        assert(group.live_tasks == COUNT - i - 1);
    }
    assert(!control.timer_queue.rb_node && reparented == COUNT * 2);
    puts("Task timer replacement and terminal cleanup validation passed");
}
'''
code = (preamble + header + rbtree + model
        + function(scheduler, 'void timer_arm_unsafe(')
        + function(scheduler, 'void timer_disarm_unsafe(')
        + function(process, 'void ps_mark_dying_unsafe(') + checks)
with tempfile.TemporaryDirectory(prefix='mos-task-timer-') as directory:
    probe = Path(directory) / 'probe.c'
    binary = Path(directory) / 'probe'
    probe.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', str(probe), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
