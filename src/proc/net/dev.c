/*
 * dev.c — /proc/net/dev (interface stats; read by ifconfig).
 *
 * Linux two-line header format followed by one line per interface.
 */
#include "proc_net.h"
#include <net/net.h>
#include <lib/klib.h>
#include <macro.h>

#include <lwip/netif.h>

file *open_net_dev(void)
{
	char *buf = (char *)vm_alloc(1);
	char *p = buf;
	memset(buf, 0, PAGE_SIZE);

	p += sprintf(
		p,
		"Inter-|   Receive                                                |"
		"  Transmit\n");
	p += sprintf(
		p,
		" face |bytes    packets errs drop fifo frame compressed multicast|"
		"bytes    packets errs drop fifo colls carrier compressed\n");

	/* lo — always present, always zero */
	p += sprintf(p,
		     "   lo:       0       0    0    0    0     0          0"
		     "         0        0       0    0    0    0     0       0"
		     "          0\n");

	/* eth0 — live counters */
	net_stats_t st;
	net_get_stats(&st);
	struct netif *nif = net_get_default_netif();
	if (nif) {
		char name[8];
		sprintf(name, "%c%c%u", nif->name[0], nif->name[1], nif->num);
		p += sprintf(p,
			     "%6s:%8lu %7lu    0    0    0     0          0"
			     "         0%8lu %7lu    0    0    0     0       0"
			     "          0\n",
			     name, st.rx_bytes, st.rx_packets, st.tx_bytes,
			     st.tx_packets);
	}

	(void)p;
	return make_text_file(buf);
}
