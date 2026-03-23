/*
 * if_inet6.c — /proc/net/if_inet6 (empty; no IPv6 support).
 */
#include "proc_net.h"
#include <lib/klib.h>
#include <macro.h>

file *open_net_if_inet6(void)
{
	char *buf = (char *)vm_alloc(1);
	memset(buf, 0, PAGE_SIZE);
	return make_text_file(buf);
}
