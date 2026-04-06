#include "common.h"

static void proc_buf_ensure(proc_buf_t *pb, size_t need)
{
	char *nb;
	size_t nc = pb->cap;
	if (pb->len + need + 1 <= nc)
		return;
	while (pb->len + need + 1 > nc)
		nc *= 2;
	nb = (char *)kmalloc(nc);
	memcpy(nb, pb->buf, pb->len + 1);
	kfree(pb->buf);
	pb->buf = nb;
	pb->cap = nc;
}

proc_buf_t *proc_buf_new(void)
{
	proc_buf_t *pb = malloc(sizeof(*pb));
	pb->cap = 512;
	pb->len = 0;
	pb->buf = (char *)kmalloc(pb->cap);
	pb->buf[0] = '\0';
	return pb;
}

void proc_buf_free(proc_buf_t *pb)
{
	if (!pb)
		return;

	if (pb->buf)
		kfree(pb->buf);

	free(pb);
}

void proc_buf_printf(proc_buf_t *pb, const char *fmt, ...)
{
	va_list ap;
	va_list ap_copy;
	int n;

	va_start(ap, fmt);
	ap_copy = ap;
	n = vsprintf(NULL, fmt, ap);
	va_end(ap);

	if (n <= 0)
		goto done;

	proc_buf_ensure(pb, (size_t)n);
	vsprintf(pb->buf + pb->len, fmt, ap_copy);
	pb->len += (size_t)n;
done:
	va_end(ap_copy);
}

void proc_buf_copy(proc_buf_t *pb, const void *src, size_t len)
{
	proc_buf_ensure(pb, len);
	memcpy(pb->buf, src, len);
	pb->len += len;
}
