/* IA-32 wire layouts are independent of the kernel's pointer size. */
#include <ps/ps.h>
#include <elf/exec.h>
#include <lib/klib.h>
#include <errno.h>
#include <syscall/impl/syscall_internal.h>
#include <arch/abi/i386/compat.h>
struct iovec32 {
	uint32_t base, len;
};
static char **vector32(const uint32_t *v)
{
	unsigned n = 0;
	if (v)
		while (v[n]) {
			if (++n > 4096)
				return NULL;
		}
	char **out = kmalloc((n + 1) * sizeof(char *));
	if (!out)
		return NULL;
	for (unsigned i = 0; i < n; i++)
		out[i] = (char *)(uintptr_t)v[i];
	out[n] = NULL;
	return out;
}
int compat_execve(const char *file, const uint32_t *argv, const uint32_t *envp)
{
	char **av = vector32(argv), **ev = vector32(envp);
	if (!av || !ev) {
		kfree(av);
		kfree(ev);
		return -ENOMEM;
	}
	extern int sys_execve_owned(const char *, char **, char **);
	int ret = sys_execve_owned(file, av, ev);
	kfree(av);
	kfree(ev);
	return ret;
}
static int compat_iov(int fd, const struct iovec32 *in, int count, int write)
{
	if (count < 0 || count > 1024)
		return -EINVAL;
	struct iovec *iov = kmalloc(count * sizeof(*iov));
	if (!iov && count)
		return -ENOMEM;
	for (int i = 0; i < count; i++) {
		iov[i].iov_base = (char *)(uintptr_t)in[i].base;
		iov[i].iov_len = in[i].len;
	}
	int ret = write ? sys_writev(fd, iov, count) :
			  sys_readv(fd, iov, count);
	kfree(iov);
	return ret;
}
int compat_readv(int fd, const void *iov, int n)
{
	return compat_iov(fd, iov, n, 0);
}
int compat_writev(int fd, const void *iov, int n)
{
	return compat_iov(fd, iov, n, 1);
}
struct sigaction32 {
	uint32_t handler, mask, flags, restorer;
};
int compat_sigaction(int sig, const void *in, void *out)
{
	struct sigaction a, old;
	const struct sigaction32 *src = in;
	if (src) {
		a.sa_handler = (void *)(uintptr_t)src->handler;
		a.sa_mask = src->mask;
		a.sa_flags = src->flags;
		a.sa_restorer = (void *)(uintptr_t)src->restorer;
	}
	int ret = sys_sigaction(sig, in ? &a : NULL, out ? &old : NULL);
	if (!ret && out) {
		struct sigaction32 *dst = out;
		dst->handler = (uintptr_t)old.sa_handler;
		dst->mask = old.sa_mask;
		dst->flags = old.sa_flags;
		dst->restorer = (uintptr_t)old.sa_restorer;
	}
	return ret;
}
struct stack32 {
	uint32_t sp;
	int32_t flags;
	uint32_t size;
};
int compat_sigaltstack(const void *in, void *out)
{
	const struct stack32 *src = in;
	stack_t a, old;
	if (src) {
		a.ss_sp = (void *)(uintptr_t)src->sp;
		a.ss_flags = src->flags;
		a.ss_size = src->size;
	}
	int ret = sys_sigaltstack(in ? &a : NULL, out ? &old : NULL);
	if (!ret && out) {
		struct stack32 *dst = out;
		dst->sp = (uintptr_t)old.ss_sp;
		dst->flags = old.ss_flags;
		dst->size = old.ss_size;
	}
	return ret;
}
