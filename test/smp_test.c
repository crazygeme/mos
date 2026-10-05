#include <test/test.h>
#include <ps/ps.h>
#include <ps/smp.h>
#include <mm/mm.h>
#include <mm/mmu.h>
#include <mm/mmap.h>
#include <lib/lock.h>
#include <device/time.h>
#include <macro.h>

struct smp_test_state {
	spinlock_t lock;
	unsigned parent;
	unsigned arrivals;
	unsigned completed;
	unsigned cpus;
	unsigned errors;
	unsigned count;
	vaddr_t page_dir;
};

/* The rendezvous requires simultaneous kernel execution on two CPUs. */
static void smp_test_worker(void *param)
{
	struct smp_test_state *state = param;
	unsigned long long deadline = time_now_tickets() + 200;
	current->ppid = state->parent;
	current->exit_signal = 0;
	sched_disable();
	addr_space_t original_root = arch_mm_current_address_space();
	smp_mm_activate(VIRT_TO_PHY(state->page_dir));
	if (arch_mm_current_address_space() != VIRT_TO_PHY(state->page_dir) ||
	    smp_cpus[smp_cpu_id()].active_root != VIRT_TO_PHY(state->page_dir))
		__sync_add_and_fetch(&state->errors, 1);
	__sync_or_and_fetch(&state->cpus, 1U << smp_cpu_id());
	__atomic_add_fetch(&state->arrivals, 1, __ATOMIC_RELEASE);
	while (__atomic_load_n(&state->arrivals, __ATOMIC_ACQUIRE) != 2 &&
	       time_now_tickets() < deadline) {
		smp_tlb_poll();
		PAUSE();
	}
	if (__atomic_load_n(&state->arrivals, __ATOMIC_ACQUIRE) != 2)
		__sync_add_and_fetch(&state->errors, 1);
	else {
		for (unsigned i = 0; i < 128; i++) {
			vaddr_t table = mm_alloc_page_table();
			if (!table) {
				__sync_add_and_fetch(&state->errors, 1);
				break;
			}
			int irq;
			spinlock_lock(&state->lock, &irq);
			state->count++;
			/* The peer must acknowledge while spinning with IF clear. */
			if (!(i % 16))
				smp_tlb_flush();
			else if (!(i % 8))
				smp_tlb_flush_user(state->page_dir);
			spinlock_unlock(&state->lock, irq);
			mm_free_page_table(table);
		}
	}
	smp_mm_activate(original_root);
	sched_enable();
	__atomic_add_fetch(&state->completed, 1, __ATOMIC_RELEASE);
	do_exit(0);
}

KTEST(smp, parallel_kernel_and_shootdown)
{
	if (smp_cpu_count() < 2)
		return 0;
	struct smp_test_state *state = zalloc(sizeof(*state));
	ASSERT_NONNULL(state);
	spinlock_init(&state->lock);
	state->parent = current->psid;
	state->page_dir = current->user->vm->page_dir;
	unsigned pids[2], created = 0;
	for (unsigned i = 0; i < 2; i++) {
		unsigned pid =
			ps_create(smp_test_worker, state, ps_normal, ps_kernel);
		if ((int)pid < 0)
			break;
		pids[created++] = pid;
		current->nchildren++;
	}
	while (__atomic_load_n(&state->completed, __ATOMIC_ACQUIRE) < created)
		time_wait(1);
	for (unsigned i = 0; i < created; i++) {
		int status = -1;
		EXPECT_EQ(do_waitpid(pids[i], &status, 0, NULL), (int)pids[i]);
		EXPECT_EQ(status, 0);
	}
	EXPECT_EQ(created, 2U);
	EXPECT_EQ(state->errors, 0U);
	EXPECT_EQ(state->count, 256U);
	EXPECT_NE(state->cpus & (state->cpus - 1), 0U);
	kfree(state);
	return 0;
}
