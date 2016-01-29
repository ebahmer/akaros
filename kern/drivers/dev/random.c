/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

#include <vfs.h>
#include <kfs.h>
#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <cpio.h>
#include <pmap.h>
#include <smp.h>
#include <ip.h>
#include <fortuna.h>

static qlock_t rl;
static spinlock_t tap_lock;
static int len;
static struct queue *bits;
/* This is only applicable to the random file. urandom will not block. */
struct fdtap_slist data_tap;

/*
 * Add entropy
 */
void random_add(void *xp)
{
	ERRSTACK(1);

	if (waserror()) {
		qunlock(&rl);
		nexterror();
	}

	qlock(&rl);
	fortuna_add_entropy(xp, sizeof(xp));
	qunlock(&rl);

	poperror();
}

/*
 *  consume random bytes
 */
static uint32_t _randomread(void *xp, uint32_t n)
{
	ERRSTACK(1);

	if (waserror()) {
		qunlock(&rl);
		nexterror();
	}

	qlock(&rl);
	fortuna_get_bytes(n, xp);
	qunlock(&rl);

	poperror();

	return n;
}

/**
 * Fast random generator
 **/
uint32_t urandomread(void *xp, uint32_t n)
{
	ERRSTACK(1);
	uint64_t seed[16];
	uint8_t *e, *p;
	uint32_t x = 0;
	uint64_t s0;
	uint64_t s1;

	if (waserror())
		nexterror();

	// The initial seed is from a good random pool.
	_randomread(seed, sizeof(seed));
	p = xp;
	for (e = p + n; p < e;) {
		s0 = seed[x];
		s1 = seed[x = (x + 1) & 15];
		s1 ^= s1 << 31;
		s1 ^= s1 >> 11;
		s0 ^= s0 >> 30;
		*p++ = (seed[x] = s0 ^ s1) * 1181783497276652981LL;
	}
	poperror();
	return n;
}

struct dev randomdevtab;

static char *devname(void)
{
	return randomdevtab.name;
}

enum {
	Qdir,
	Qrandom,
	Qurandom
};

static
struct dirtab randomdir[] = {
	{".", {Qdir, 0, QTDIR}, 0, DMDIR | 0500},
	{"random", {Qrandom}, 0, 0444},
	{"urandom", {Qurandom}, 0, 0444},
};

static void randominit(void)
{
	qlock_init(&rl);
	/* taps. */
	spinlock_init(&tap_lock);
	bits = qopen(1<<20, 0, 0, 0);
}

/*
 *  create a random, no streams are created until an open
 */
static struct chan *randomattach(char *spec)
{
	return devattach(devname(), spec);
}

static struct walkqid *randomwalk(struct chan *c, struct chan *nc, char **name,
								int nname)
{
	return devwalk(c, nc, name, nname, randomdir, ARRAY_SIZE(randomdir), devgen);
}

static int randomstat(struct chan *c, uint8_t *dp, int n)
{
	return devstat(c, dp, n, randomdir, ARRAY_SIZE(randomdir), devgen);
}

/*
 *  if the stream doesn't exist, create it
 */
static struct chan *randomopen(struct chan *c, int omode)
{
	return devopen(c, omode, randomdir, ARRAY_SIZE(randomdir), devgen);
}

static void randomclose(struct chan *c)
{
}

static long randomread(struct chan *c, void *va, long n, int64_t ignored)
{
	switch (c->qid.path) {
		case Qdir:
			return devdirread(c, va, n, randomdir,
					  ARRAY_SIZE(randomdir), devgen);
		case Qrandom:
			return qread(bits, va, n);
		case Qurandom:
			return qread(bits, va, n);
		default:
			panic("randomread: qid %d is impossible", c->qid.path);
	}
	return -1;	/* not reached */
}

static struct block *randombread(struct chan *c, long n, uint32_t offset)
{
	switch (c->qid.path) {
		case Qrandom:
			return qbread(bits, n);
		case Qurandom:
			return qbread(bits, n);
	}

	return devbread(c, n, offset);
}

/*
 *  A write to a closed random causes an ERANDOM error to be thrown.
 */
static long randomwrite(struct chan *c, void *va, long n, int64_t ignored)
{
	error(EPERM, "No use for writing random just yet");
	return -1;
}

static long randombwrite(struct chan *c, struct block *bp, uint32_t junk)
{
	error(EPERM, "No use for writing random just yet");
	return -1;
}

static int randomwstat(struct chan *c, uint8_t *dp, int n)
{
	error(EPERM, "No use for wstat random just yet");
	return -1;
}

static void random_wake_cb(struct queue *q, void *data, int filter)
{
	/* If you allocate structs like this on odd byte boundaries, you
	 * deserve what you get.  */
	struct fd_tap *tap_i;

	spin_lock(&tap_lock);
	SLIST_FOREACH(tap_i, &data_tap, link)
		fire_tap(tap_i, filter);
	spin_unlock(&tap_lock);
}

static int randomtapfd(struct chan *chan, struct fd_tap *tap, int cmd)
{
	int ret;

#define DEVRANDOM_LEGAL_DATA_TAPS (FDTAP_FILT_READABLE | FDTAP_FILT_WRITABLE | \
                                 FDTAP_FILT_HANGUP | FDTAP_FILT_ERROR)

	switch (chan->qid.path) {
	case Qurandom:
		error(EINVAL, "Can't tap urandom");
		break;
		/* fall through */
	case Qrandom:
		if (tap->filter & ~DEVRANDOM_LEGAL_DATA_TAPS) {
			set_errno(ENOSYS);
			set_errstr("Unsupported #%s data tap %p, must be %p", devname(),
			           tap->filter, DEVRANDOM_LEGAL_DATA_TAPS);
			return -1;
		}
		spin_lock(&tap_lock);
		switch (cmd) {
		case (FDTAP_CMD_ADD):
			if (SLIST_EMPTY(&data_tap))
				qio_set_wake_cb(bits, random_wake_cb, (void *)0);
			SLIST_INSERT_HEAD(&data_tap, tap, link);
			ret = 0;
			break;
		case (FDTAP_CMD_REM):
			SLIST_REMOVE(&data_tap, tap, fd_tap, link);
			if (SLIST_EMPTY(&data_tap))
				qio_set_wake_cb(bits, 0, (void *)0);
			ret = 0;
			break;
		default:
			set_errno(ENOSYS);
			set_errstr("Unsupported #%s data tap command %p", devname(), cmd);
			ret = -1;
		}
		spin_unlock(&tap_lock);
		return ret;
	default:
		set_errno(ENOSYS);
		set_errstr("Can't tap #%s file type %d", devname(), chan->qid.path);
		return -1;
	}
}

struct dev randomdevtab __devtab = {
	.name = "random",

	.reset = devreset,
	.init = randominit,
	.shutdown = devshutdown,
	.attach = randomattach,
	.walk = randomwalk,
	.stat = randomstat,
	.open = randomopen,
	.create = devcreate,
	.close = randomclose,
	.read = randomread,
	.bread = randombread,
	.write = randomwrite,
	.bwrite = randombwrite,
	.remove = devremove,
	.wstat = randomwstat,
	.power = devpower,
	.chaninfo = devchaninfo,
	.tapfd = randomtapfd,
};

/* This is something used by the TCP stack.
 * I have no idea of why these numbers were chosen. */
static uint32_t randn;

static void seedrand(void)
{
	ERRSTACK(2);
	if (!waserror()) {
		_randomread((void *)&randn, sizeof(randn));
		poperror();
	}
}

int nrand(int n)
{
	if (randn == 0)
		seedrand();
	randn = randn * 1103515245 + 12345 + read_tsc();
	return (randn >> 16) % n;
}

int rand(void)
{
	nrand(1);
	return randn;
}

