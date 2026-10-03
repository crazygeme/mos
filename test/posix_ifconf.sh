#!/bin/sh
set -e

BASE=/root/tests/posix_ifconf
mkdir -p "$BASE"
cat > "$BASE/ifconf.c" <<'EOF'
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <rpc/rpc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "ifconf: %s\n", what);
		exit(1);
	}
}

int main(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	unsigned char buffer[8 * sizeof(struct ifreq) + 16];
	struct {
		struct ifconf conf;
		unsigned int poison[2];
	} arg;
	struct ifconf query;
	struct sockaddr_in address;
	int i, count, bytes;

	check(fd >= 0, "socket");
	memset(&arg, 0, sizeof(arg));
	/* An AMD64 load at offset 8 of an i386 ifconf reads this unmapped
	 * address instead of the buffer pointer stored at offset 4. */
	arg.poison[0] = 0x74706000U;
	arg.poison[1] = 0;
	memset(buffer, 0xa5, sizeof(buffer));
	arg.conf.ifc_len = sizeof(buffer) - 16;
	arg.conf.ifc_buf = (char *)buffer;
	check(ioctl(fd, SIOCGIFCONF, &arg.conf) == 0, "enumerate");
	bytes = arg.conf.ifc_len;
	check(bytes > 0 && bytes <= (int)sizeof(buffer) - 16 &&
	      bytes % sizeof(struct ifreq) == 0, "entry size and count");
	check(arg.conf.ifc_buf == (char *)buffer, "buffer pointer preserved");
	check(arg.poison[0] == 0x74706000U && arg.poison[1] == 0,
	      "ifconf bounds");
	for (i = bytes; i < (int)sizeof(buffer); i++)
		check(buffer[i] == 0xa5, "output bounds");
	count = bytes / sizeof(struct ifreq);
	for (i = 0; i < count; i++) {
		struct ifreq request = ((struct ifreq *)buffer)[i];
		check(memchr(request.ifr_name, 0, IFNAMSIZ) != NULL,
		      "interface name");
		check(request.ifr_addr.sa_family == AF_INET, "address family");
		check(ioctl(fd, SIOCGIFFLAGS, &request) == 0, "interface flags");
		check((request.ifr_flags & IFF_UP) != 0, "interface up");
	}
	memset(&query, 0, sizeof(query));
	check(ioctl(fd, SIOCGIFCONF, &query) == 0 && query.ifc_len == bytes,
	      "NULL-buffer size query");
	memset(buffer, 0xa5, sizeof(buffer));
	arg.conf.ifc_len = sizeof(struct ifreq) - 1;
	check(ioctl(fd, SIOCGIFCONF, &arg.conf) == 0 && arg.conf.ifc_len == 0,
	      "short buffer");
	for (i = 0; i < (int)sizeof(buffer); i++)
		check(buffer[i] == 0xa5, "short buffer unchanged");
	arg.conf.ifc_len = sizeof(struct ifreq);
	check(ioctl(fd, SIOCGIFCONF, &arg.conf) == 0 &&
	      arg.conf.ifc_len == sizeof(struct ifreq), "one-entry buffer");
	for (i = sizeof(struct ifreq); i < (int)sizeof(buffer); i++)
		check(buffer[i] == 0xa5, "one-entry bounds");
	/* Exercise the glibc RPC helper used by the portmapper client. */
	get_myaddress(&address);
	check(address.sin_family == AF_INET, "RPC local address");
	close(fd);
	puts("ifconf: PASS");
	return 0;
}
EOF
gcc -Wall -Werror -o "$BASE/ifconf" "$BASE/ifconf.c"
"$BASE/ifconf"
