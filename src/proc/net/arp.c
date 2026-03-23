/*
 * arp.c — /proc/net/arp (ARP cache).
 *
 * Linux format:
 *   IP address       HW type  Flags  HW address            Mask  Device
 *   192.168.1.1      0x1      0x2    52:54:00:12:34:56     *     eth0
 */
#include "proc_net.h"
#include <net/net.h>
#include <lib/klib.h>
#include <macro.h>

#include <lwip/netif.h>
#include <lwip/etharp.h>

file *open_net_arp(void)
{
	char *buf = (char *)vm_alloc(1);
	char *p = buf;
	memset(buf, 0, PAGE_SIZE);

	p += sprintf(p, "IP address       HW type     Flags       "
			"HW address            Mask     Device\n");

	struct netif *nif = net_get_default_netif();
	if (nif) {
		char name[8];
		sprintf(name, "%c%c%u", nif->name[0], nif->name[1], nif->num);

		int i;
		for (i = 0; i < ARP_TABLE_SIZE; i++) {
			ip4_addr_t *entry_ip = NULL;
			struct eth_addr *entry_mac = NULL;
			if (etharp_get_entry((size_t)i, &entry_ip, NULL,
					     &entry_mac) == 1) {
				p += sprintf(p,
					     "%-16s 0x1         0x2         "
					     "%02x:%02x:%02x:%02x:%02x:%02x"
					     "     *        %s\n",
					     ip4addr_ntoa(entry_ip),
					     entry_mac->addr[0],
					     entry_mac->addr[1],
					     entry_mac->addr[2],
					     entry_mac->addr[3],
					     entry_mac->addr[4],
					     entry_mac->addr[5], name);
			}
		}
	}

	(void)p;
	return make_text_file(buf);
}
