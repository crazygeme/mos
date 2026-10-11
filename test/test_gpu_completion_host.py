#!/usr/bin/env python3
"""Validate GPU completion enrollment and shared IRQ handler lifetime."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
gpu = (ROOT / "src/driver/video/virtio_gpu.c").read_text()
interrupts = (ROOT / "src/int/impl/int.c").read_text()
irq_code = interrupts[interrupts.index("static int_callback in_callbacks"):
                      interrupts.index("static void intr_maybe_preempt")]
wait_code = gpu[gpu.index("static void gpu_interrupt("):
                gpu.index("/* The control queue reuses")]

preamble = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#define IDT_SIZE 256
#define INT_VECTOR_IRQ0 32
typedef struct { unsigned vec_no; } intr_frame;
typedef void (*int_callback)(intr_frame *);
static unsigned long intr_stubs[IDT_SIZE], gates[IDT_SIZE];
static void arch_interrupt_set_gate(unsigned v, unsigned long f, int t, int d)
{ (void)t; (void)d; gates[v] = f; }
typedef struct { int remain_ticks, vm_lock_depth; } task_schedule;
typedef struct { int waiting; task_schedule *sched; } task_struct;
typedef struct { int held; } spinlock_t;
static task_struct task;
static task_schedule schedule;
static task_struct interrupted = { .sched = &schedule };
static task_struct *active = &task;
static spinlock_t gpu_wait_lock;
static task_struct *gpu_waiter;
static struct { uint16_t idx; } used, *gpu_used = &used;
static uint16_t gpu_index;
static uint8_t isr, *gpu_isr = &isr;
static unsigned gpu_irq = 11;
static uint64_t gpu_deadline = 5000000, clock_us;
static unsigned delay_ms, scheduled, finished, wakes, race;
static void gpu_interrupt(intr_frame *);
static task_struct *current_task(void) { return active; }
#define CURRENT_TASK() current_task()
static int ps_enabled(void) { return 1; }
static uint64_t time_now_us(void) { return clock_us; }
static void spinlock_lock(spinlock_t *l, int *irq)
{ assert(!l->held); l->held = 1; *irq = 1; }
static void ps_put_to_ready_queue(task_struct *t)
{ assert(gpu_wait_lock.held); t->waiting = 0; wakes++; }
static void spinlock_unlock(spinlock_t *l, int irq)
{
    (void)irq; assert(l->held); l->held = 0;
    if (race == 1) { race = 0; used.idx++; isr = 1; gpu_interrupt(NULL); }
}
static void ps_prepare_timed_wait(task_struct *t, unsigned ms)
{ assert(gpu_wait_lock.held); assert(ms); delay_ms = ms; t->waiting = 1; }
static void task_sched(void)
{
    assert(!gpu_wait_lock.held); assert(gpu_waiter == &task); scheduled++;
    assert(task.sched->vm_lock_depth == 1);
    if (race == 2) { race = 0; used.idx++; isr = 1; gpu_interrupt(NULL); }
    else if (task.waiting) { clock_us += delay_ms * 1000ULL; task.waiting = 0; }
}
static void ps_finish_timed_wait(task_struct *t)
{ assert(t == &task); assert(!gpu_waiter);
  assert(t->sched->vm_lock_depth == 1); finished++; }
'''

checks = r'''
static unsigned primary_calls, shared_calls;
static void primary(intr_frame *f) { (void)f; primary_calls++; }
static void shared(intr_frame *f) { (void)f; shared_calls++; }
static void reset(void)
{
    task.waiting = 0; gpu_waiter = NULL; used.idx = gpu_index = 0;
    clock_us = 0; delay_ms = scheduled = finished = wakes = race = 0;
    gpu_irq = 11; isr = 0; active = &task; task.sched = &schedule;
    schedule.remain_ticks = 4; schedule.vm_lock_depth = 0;
}
int main(void)
{
    intr_frame frame = {43};
    intr_stubs[43] = 123;
    assert(int_register_irq(16, shared) == -EINVAL);
    assert(int_register_irq(11, NULL) == -EINVAL);
    int_register(43, primary, 0, 0);
    assert(int_register_irq(11, shared) == 0);
    assert(int_register_irq(11, gpu_interrupt) == 0);
    in_callbacks[43](&frame);
    for (unsigned i = 0; i < SHARED_IRQ_HANDLERS; i++)
        if (irq_callbacks[11][i]) irq_callbacks[11][i](&frame);
    assert(primary_calls == 1 && shared_calls == 1);
    int_unregister(43);
    assert(gates[43] == 123);
    int_unregister_irq(11, shared);
    assert(gates[43] == 123);
    int_unregister_irq(11, gpu_interrupt);
    assert(!gates[43]);
    for (unsigned i = 0; i < SHARED_IRQ_HANDLERS; i++)
        assert(int_register_irq(11, shared) == 0);
    assert(int_register_irq(11, primary) == -ENOSPC);
    int_unregister_irq(11, shared);
    assert(!irq_has_handlers(11));

    /* A completion visible before enrollment cannot put the task to sleep. */
    reset(); used.idx = 1; gpu_wait_completion();
    assert(!scheduled && !task.waiting && !gpu_waiter);
    /* Completion between enrollment and scheduling must retain the wakeup. */
    reset(); race = 1; gpu_wait_completion();
    assert(scheduled == 1 && wakes == 1 && !clock_us && finished == 1);
    assert(!schedule.vm_lock_depth);
    /* Completion while asleep must wake the enrolled task. */
    reset(); race = 2; gpu_wait_completion();
    assert(scheduled == 1 && wakes == 1 && finished == 1);
    /* A missing interrupt retains the command deadline. */
    reset(); gpu_wait_completion();
    assert(delay_ms == 5000 && clock_us == gpu_deadline && finished == 1);
    assert(!schedule.vm_lock_depth);
    reset(); clock_us = gpu_deadline; gpu_wait_completion();
    assert(!scheduled && !task.waiting);
    /* Devices without an IRQ use bounded timer polling. */
    reset(); gpu_irq = ~0U; gpu_wait_completion();
    assert(delay_ms == 1 && clock_us == 1000 && finished == 1);
    /* A shared interrupt without a queue notification cannot wake the task. */
    reset(); gpu_waiter = &task; task.waiting = 1; isr = 2;
    gpu_interrupt(NULL); assert(!wakes && task.waiting);
    isr = 1; gpu_interrupt(NULL); assert(wakes == 1 && !task.waiting);
    active = &interrupted; task.waiting = 1; gpu_interrupt(NULL);
    assert(schedule.remain_ticks == 0 && wakes == 2);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="mos-gpu-completion-") as directory:
    source = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    source.write_text(preamble + irq_code + wait_code + checks)
    subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Werror",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("GPU completion and shared IRQ tests passed")
