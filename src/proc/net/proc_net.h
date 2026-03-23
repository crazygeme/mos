#ifndef _PROC_NET_PROC_NET_H
#define _PROC_NET_PROC_NET_H

#include <fs/fs.h>

/* Shared helpers */
file *make_text_file(char *buf);

/* Per-file open functions */
file *open_net_dev(void);
file *open_net_if_inet6(void);
file *open_net_route(void);
file *open_net_arp(void);

#endif /* _PROC_NET_PROC_NET_H */
