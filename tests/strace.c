/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

#include <stdlib.h>
#include <stdio.h>
#include <parlib/parlib.h>
#include <unistd.h>
#include <signal.h>
#include <iplib/iplib.h>
#include <iplib/icmp.h>
#include <ctype.h>
#include <pthread.h>
#include <parlib/spinlock.h>
#include <parlib/timing.h>
#include <parlib/tsc-compat.h>
#include <parlib/printf-ext.h>
#include <benchutil/alarm.h>
#include <ndblib/ndb.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/wait.h>

int verbose;

void
usage(void)
{
	fprintf(stderr, "usage: strace command [args...]\n");
	exit(1);
}

void
main(int argc, char **argv)
{
	int fd;
	int pid;
	int amt;
	static char p[128];
	static char buf[16384];
	int pfd[2];

	/* we can't get exits soon enough for me ... */
	if (pipe(pfd) < 0) {
		fprintf(stderr, "pipe(): %r\n");
		exit(1);
	}

	pid = fork();
	if (pid == 0) {
		/* block until the parent is ready to watch us. */
		if (read(pfd[1], buf, 1) < 1) {
			fprintf(stderr, "Read from child pipe %d: %r\n", pfd[1]);
			exit(1);
		}
		if (execv(argv[1], argv+1)) {
			fprintf(stderr, "Exec %s: %r\n", argv[1]);
			exit(1);
		}
	}

	snprintf(p, sizeof(p), "/proc/%d/ctl", pid);
	fd = open(p, O_WRITE);
	if (fd < 0) {
		fprintf(stderr, "open %s: %r\n", p);
		exit(1);
	}

	snprintf(p, sizeof(p), "strace");
	if (write(fd, p, strlen(p)) < strlen(p)) {
		fprintf(stderr, "write to ctl %s %d: %r\n", p, fd);
		exit(1);
	}
	close(fd);

	snprintf(p, sizeof(p), "/proc/%d/strace", pid);
	fd = open(p, O_READ);
	if (fd < 0) {
		fprintf(stderr, "open %s: %r\n", p);
		exit(1);
	}

	if (write(pfd[0], buf, 1) < 1) {
		fprintf(stderr, "write to parent pipe %d: %r\n", pfd[0], p);
		exit(1);
	}
	while ((amt = read(fd, buf, sizeof(buf))) > 0) {
		if (write(1, buf, amt) < amt) {
			fprintf(stderr, "Write to stdout: %r\n");
			exit(1);
		}
	}
	if (amt < 0)
		fprintf(stderr, "Read fd %d for %s: %r\n", fd, p);
}
