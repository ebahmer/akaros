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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

char*
smprint(const char *fmt, ...)
{
	va_list args;
	char *p;

	p = malloc(256);
	va_start(args, fmt);
	snprintf(p, 256, fmt, args);
	va_end(args);
	return p;
}


void sysfatal(char *s)
{
	fprintf(stderr, "%s\n", s);
	exit(1);
}

/*
 * no threads, just old school fork.
 *Thus we get almost the same code on Plan 9 and Linux.
 */
int cons = -1;
void unraw(void)
{
	if (write(cons, "rawoff", 6) < 6)
		fprintf(stderr, "rawoff: %r");
}
void
main(void)
{
	int pid, dirfd, ctl, m, s;
	char c[1];

	cons = open("/dev/consctl", O_WRONLY);
	if (cons < 0) {
		//sysfatal(smprint("%r"));
		fprintf(stderr, "/dev/consctl: %r, continuing anyway");
	} else {
		if (write(cons, "rawon", 5) < 5)
			fprintf(stderr,
				"Ignoring rawon failure but there's no raw mode: %r");
		else
			atexit(unraw);
	}

	if ((dirfd = open("#fdmux", O_PATH)) < 0)
		sysfatal(smprint("%r"));

	if ((ctl = openat(dirfd, "ctl", O_RDWR)) < 0)
		sysfatal(smprint("%r"));
	if ((m = openat(dirfd, "m", O_RDWR)) < 0)
		sysfatal(smprint("%r"));
	if ((s = openat(dirfd, "s", O_RDWR)) < 0)
		sysfatal(smprint("%r"));

	pid = fork();
	if (pid < 0)
		sysfatal(smprint("%r"));

	if (pid == 0) {
		dup2(s, 0);
		dup2(s, 1);
		dup2(s, 2);
		close(s);
		close(m);
		execl("/bin/sh", "sh", NULL);
		sysfatal(smprint("%r"));
	}

	close(s);

	pid = fork();
	if (pid < 0)
		sysfatal(smprint("%r"));
	if (pid == 0) {
		int amt;

		while ((amt = read(m, c, 1)) > 0) {
			if (write(1, c, 1) < 1)
				sysfatal(smprint("%r"));
		}
	}

	for (;;) {
		int amt;

		amt = read(0, c, 1);
		if (amt < 0) {
			fprintf(stderr, "EOF from fd 0\n");
			break;
		}
		/* we indicated ^D to the child with a zero byte write. */
		if (c[0] == 4) {
			write(m, c, 0);
			write(1, "^D", 2);
			continue;
		}
		if (c[0] == 3) {
			write(1, "^C", 2);
			write(ctl, "n", 1);
			continue;
		}
		if (c[0] == 26) {
			write(1, "^Z", 2);
			write(ctl, "s", 1);
			continue;
		}
		if (c[0] == '\r') {
			write(1, c, 1);
			c[0] = '\n';
		}

		if (0) /* we probably don't want to echo on akaros */
		if (write(1, c, 1) < 1)
			sysfatal(smprint("%r"));
		if (write(m, c, amt) < amt) {
			fprintf(stderr, "%r");
			break;
		}
		/*
		 * for the record: it's totally legal to try to keep
		 * reading after a 0 byte read, even on unix.
		 */
	}

	if (write(cons, "rawoff", 6) < 6)
		fprintf(stderr, "rawoff: %r");
}
