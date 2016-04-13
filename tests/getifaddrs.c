#define _LARGEFILE64_SOURCE /* needed to use lseek64 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <parlib/arch/arch.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <ros/syscall.h>
#include <ros/fs.h>

int mygetifaddrs(struct ifaddrs **ifap)
{
	DIR *net;
	struct dirent *d;
	int addr;
	static char etheraddr[MAX_PATH_LEN];
	struct ifaddrs *ifa;
	uint8_t *v;

	*ifap = NULL;

	net = opendir("/net");
	if (net == NULL) {
		fprintf(stderr, "/net: %r");
		return -1;
	}

	for (d = readdir(net); d; d = readdir(net)) {
		if (strncmp(d->d_name, "ether", 5))
			continue;
		sprintf(etheraddr, "/net/%s/addr", d->d_name);
		addr = open(etheraddr, O_RDONLY);
		if (addr < 0)
			continue;
		if (read(addr, etheraddr, 24) < 0) {
			fprintf(stderr, "Read addr from %s: %r", d->d_name);
			continue;
		}
		/* getifaddrds is a stupid design as it only admits of
		 * one address per interface.  Don't even bother
		 * filling in ifa_{addr,netmask}. They're allowed to
		 * be NULL.  Broadaddr need be set IFF a bit is set
		 * in the flags field. We don't set either one.
		 */
		ifa = calloc(sizeof(*ifa), 1);
		ifa->ifa_next = *ifap;
		*ifap = ifa;
		ifa->ifa_name = strdup(d->d_name);
		v = malloc(6);
		/* oh yeah, another binary thing. It's 1976 all over again. */
		for(int i = 0; i < 6; i++) {
			sscanf(&etheraddr[i*2], "%02x", &v[i]);
		}
		ifa->ifa_data = v;

	}
	return 0;
}

int myfreeifaddrs(struct ifaddrs *ifa)
{
	free(ifa);
}

int
main(int argc, char *argv[])
{
	struct ifaddrs *ifaddr, *ifa;
	int family, s;
	char host[NI_MAXHOST];

	if (mygetifaddrs(&ifaddr) == -1) {
		perror("getifaddrs");
		exit(EXIT_FAILURE);
	}

	/*
	 * Walk through linked list, maintaining head pointer so we
	 * can free list later
	 */

	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		printf("%s\n", ifa->ifa_name);
		for(int i = 0; i < 6; i++)
			printf("%02x:", ((unsigned char *)ifa->ifa_data)[i]);
		if (ifa->ifa_addr == NULL)
			continue;

		family = ifa->ifa_addr->sa_family;

		/*
		 * Display interface name and family (including symbolic
		 * form of the latter for the common families)
		 */

		printf("%s  address family: %d%s\n",
                       ifa->ifa_name, family,
                       (family == AF_PACKET) ? " (AF_PACKET)" :
                       (family == AF_INET) ?   " (AF_INET)" :
                       (family == AF_INET6) ?  " (AF_INET6)" : "");

		/* For an AF_INET* interface address, display the address */

		if (family == AF_INET || family == AF_INET6) {
			s = getnameinfo(ifa->ifa_addr,
					(family == AF_INET) ? sizeof(struct sockaddr_in) :
					sizeof(struct sockaddr_in6),
					host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
			if (s != 0) {
				printf("getnameinfo() failed: %s\n", gai_strerror(s));
				exit(EXIT_FAILURE);
			}
			printf("\taddress: <%s>\n", host);
		}
	}

	myfreeifaddrs(ifaddr);
	exit(EXIT_SUCCESS);
}

