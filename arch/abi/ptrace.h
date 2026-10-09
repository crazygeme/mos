/* PTRACE_WORD, PTRACE_REGS and PTRACE_PREFIX define the wire layout. */
int PTRACE_PREFIX(ptrace)(int request, int pid, void *addr, void *data)
{
	if (request != PTRACE_GETEVENTMSG && request != PTRACE_PEEKDATA &&
	    request != PTRACE_PEEKTEXT && request != PTRACE_PEEKUSER &&
	    request != PTRACE_GETREGS)
		return ps_ptrace_control(request, pid, addr, data);
	task_struct *target;
	int ret = ps_ptrace_target(pid, &target);
	if (ret)
		return ret;
	PTRACE_WORD word = 0;
	PTRACE_REGS regs;
	switch (request) {
	case PTRACE_GETEVENTMSG:
		if (target->sched->status != ps_stopped)
			return -ESRCH;
		word = target->execution->ptrace_eventmsg;
		break;
	case PTRACE_PEEKDATA:
	case PTRACE_PEEKTEXT:
		ret = ps_read_process_memory(target, addr, &word, sizeof(word));
		if (ret)
			return ret;
		break;
	case PTRACE_GETREGS:
		ret = PTRACE_PREFIX(copy_regs)(target, &regs);
		if (!ret)
			memcpy(data, &regs, sizeof(regs));
		return ret;
	case PTRACE_PEEKUSER:
		if (((uintptr_t)addr & (sizeof(word) - 1)) ||
		    (uintptr_t)addr >= sizeof(regs))
			return -EIO;
		ret = PTRACE_PREFIX(copy_regs)(target, &regs);
		if (ret)
			return ret;
		memcpy(&word, (char *)&regs + (uintptr_t)addr, sizeof(word));
		break;
	}
	memcpy(data, &word, sizeof(word));
	return 0;
}
