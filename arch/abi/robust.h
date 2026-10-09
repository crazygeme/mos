/* ROBUST_WORD and ROBUST_PREFIX define the syscall wire format. */
static int ROBUST_PREFIX(reader)(task_struct *task, uintptr_t address,
				 uintptr_t *next, intptr_t *offset,
				 uintptr_t *pending)
{
	struct {
		ROBUST_WORD next;
		ROBUST_SIGNED offset;
		ROBUST_WORD pending;
	} head;
	size_t length = offset ? sizeof(head) : sizeof(head.next);
	if (ps_read_process_memory(task, (void *)address, &head, length))
		return -EFAULT;
	*next = head.next;
	if (offset) {
		*offset = head.offset;
		*pending = head.pending;
	}
	return 0;
}
int ROBUST_PREFIX(set_robust_list)(void *head, size_t length)
{
	if (length != 3 * sizeof(ROBUST_WORD))
		return -EINVAL;
	if (!head)
		return -EFAULT;
	current->execution->robust_list_head = head;
	current->execution->robust_list_size = length;
	current->execution->robust_list_reader = ROBUST_PREFIX(reader);
	return 0;
}
int ROBUST_PREFIX(get_robust_list)(int pid, void *head, void *length)
{
	if (!head || !length)
		return -EFAULT;
	task_struct *task = pid ? ps_find_process(pid) : current;
	if (!task)
		return -ESRCH;
	*(ROBUST_WORD *)head = (uintptr_t)task->execution->robust_list_head;
	*(ROBUST_WORD *)length = task->execution->robust_list_size;
	return 0;
}
#undef ROBUST_WORD
#undef ROBUST_SIGNED
#undef ROBUST_PREFIX
