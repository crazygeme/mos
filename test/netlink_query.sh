#!/bin/sh
# Validate route netlink interface-name filtering.
set -eu
probe_dir=$(mktemp -d /tmp/mos-netlink-query.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <errno.h>
#include <sys/socket.h>
#include <linux/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%d: %s: %s\n",__LINE__,#x,strerror(errno)); exit(1); } } while(0)
int main(void) {
 int fd=socket(AF_NETLINK,SOCK_RAW,NETLINK_ROUTE); CHECK(fd>=0); struct sockaddr_nl address={.nl_family=AF_NETLINK}; CHECK(bind(fd,(void *)&address,sizeof(address))==0);
 struct { struct nlmsghdr h; struct ifinfomsg link; char attrs[32]; } req={.h={.nlmsg_type=RTM_GETLINK,.nlmsg_flags=NLM_F_REQUEST,.nlmsg_seq=1}};
 struct rtattr *attr=(void *)req.attrs; attr->rta_type=IFLA_IFNAME; attr->rta_len=RTA_LENGTH(3); memcpy(RTA_DATA(attr),"lo",3); req.h.nlmsg_len=NLMSG_LENGTH(sizeof(req.link))+RTA_ALIGN(attr->rta_len);
 CHECK(sendto(fd,&req,req.h.nlmsg_len,0,(void *)&address,sizeof(address))==req.h.nlmsg_len); char *response=malloc(4096); CHECK(response); int count=recv(fd,response,4096,0); CHECK(count>0); int matches=0;
 for(struct nlmsghdr *h=(void *)response; NLMSG_OK(h,count);h=NLMSG_NEXT(h,count)) { CHECK(h->nlmsg_type!=NLMSG_ERROR); if(h->nlmsg_type==RTM_NEWLINK) matches++; }
 CHECK(matches==1); free(response); close(fd); puts("netlink interface-name query: PASS"); return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
