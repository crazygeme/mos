/*
 * route.c — /proc/net/route (kernel routing table).
 *
 * Linux format: hex little-endian 32-bit fields.
 *   Iface  Destination  Gateway  Flags  RefCnt  Use  Metric  Mask  MTU  Window  IRTT
 */
#include "proc_net.h"
#include <net/net.h>
#include <lib/klib.h>
#include <macro.h>

#include <lwip/netif.h>
#include <lwip/ip4_addr.h>

file *open_net_route(void)
{
	char *buf = (char *)vm_alloc(1);
	char *p = buf;
	memset(buf, 0, PAGE_SIZE);

	p += sprintf(p, "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\t"
			"Metric\tMask\t\tMTU\tWindow\tIRTT\n");

	struct netif *nif = net_get_default_netif();
	if (nif && !ip4_addr_isany(netif_ip4_addr(nif))) {
		char name[8];
		sprintf(name, "%c%c%u", nif->name[0], nif->name[1], nif->num);

		u32_t ip = ntohl(netif_ip4_addr(nif)->addr);
		u32_t mask = ntohl(netif_ip4_netmask(nif)->addr);
		u32_t gw = ntohl(netif_ip4_gw(nif)->addr);
		u32_t net = ip & mask;

		/* default route via gateway */
		p += sprintf(
			p,
			"%s\t00000000\t%08X\t0003\t0\t0\t100\t00000000\t0\t0\t0\n",
			name, gw);
		/* subnet route */
		p += sprintf(
			p, "%s\t%08X\t00000000\t0001\t0\t0\t0\t%08X\t0\t0\t0\n",
			name, net, mask);
	}

	(void)p;
	return make_text_file(buf);
}
