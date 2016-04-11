/* Copyright © 1994-1999 Lucent Technologies Inc.  All rights reserved.
 * Portions Copyright © 1997-1999 Vita Nuova Limited
 * Portions Copyright © 2000-2007 Vita Nuova Holdings Limited
 *                                (www.vitanuova.com)
 * Revisions Copyright © 2000-2007 Lucent Technologies Inc. and others
 *
 * Modified for the Akaros operating system:
 * Copyright (c) 2013-2014 The Regents of the University of California
 * Copyright (c) 2013-2015 Google Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */

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

struct dev fdmuxdevtab;

static char *devname(void)
{
	return fdmuxdevtab.name;
}

struct FDMux {
	qlock_t qlock;
	struct FDMux *next;
	struct kref ref;
	uint32_t path;
	struct queue *q[2];
	int qref[2];
	struct dirtab *fdmuxdir;
	char *user;
	struct fdtap_slist data_taps[2];
	spinlock_t tap_lock;
	struct rendez r;
	/* pid of owner, e.g. regress/fdmux. */
	int	owner;
	/*
	 * id of processes allowed to read/write fd[1]. Non-matchers
	 * sleep (for now)
	 */
	int	pgrpid;
	/* session leader. If > 0, we send them a note if anyone blocks on read/write.*/
	int	slpid;
	/* The active pid. Useful for interrupt.*/
	int	active;
	int     dead;
	int	debug;
};

static struct {
	spinlock_t lock;
	uint32_t path;
	int fdmuxqsize;
} fdmuxalloc;

enum {
	Qdir,
	Qm,
	Qs,
	Qctl,
};

static
struct dirtab fdmuxdir[] = {
	{".", {Qdir, 0, QTDIR}, 0, DMDIR | 0500},
	{"m", {Qm}, 0, 0660},
	{"s", {Qs}, 0, 0660},
	{"ctl",		{Qctl},	0,		0600},
};

static void freefdmux(struct FDMux *p)
{
	if (p != NULL) {
		kfree(p->user);
		kfree(p->q[0]);
		kfree(p->q[1]);
		kfree(p->fdmuxdir);
		kfree(p);
	}
}

static void fdmux_release(struct kref *kref)
{
	struct FDMux *fdmux = container_of(kref, struct FDMux, ref);

	freefdmux(fdmux);
}

static void fdmuxinit(void)
{
	fdmuxalloc.fdmuxqsize = 32 * 1024;
}

static int testready(void *v)
{
	struct chan *c = v;
	struct FDMux *p;

	p = c->aux;
	if (current->pgrp->pgrpid == p->pgrpid)
		return 1;
	return 0;
}

/*
 *  create a fdmux, no streams are created until an open
 */
static struct chan *fdmuxattach(char *spec)
{
	ERRSTACK(2);
	struct FDMux *p;
	struct chan *c;

	c = devattach(devname(), spec);
	p = kzmalloc(sizeof(struct FDMux), 0);
	if (p == 0)
		error(ENOMEM, "No memory for an FDmux struct");
	if (waserror()) {
		freefdmux(p);
		nexterror();
	}
	p->fdmuxdir = kzmalloc(sizeof(fdmuxdir), 0);
	if (p->fdmuxdir == 0)
		error(ENOMEM, ERROR_FIXME);
	memmove(p->fdmuxdir, fdmuxdir, sizeof(fdmuxdir));
	kstrdup(&p->user, current->user);
	kref_init(&p->ref, fdmux_release, 1);
	qlock_init(&p->qlock);
	rendez_init(&p->r);
	/* N.B. No Qcoalesce as 0-byte reads/writes need to go to programs. */
	p->q[0] = qopen(fdmuxalloc.fdmuxqsize, 0, 0, 0);
	if (p->q[0] == 0)
		error(ENOMEM, ERROR_FIXME);
	p->q[1] = qopen(fdmuxalloc.fdmuxqsize, 0, 0, 0);
	if (p->q[1] == 0)
		error(ENOMEM, ERROR_FIXME);
	poperror();

	spin_lock(&(&fdmuxalloc)->lock);
	p->path = ++fdmuxalloc.path;
	spin_unlock(&(&fdmuxalloc)->lock);

	c->qid.path = NETQID(2 * p->path, Qdir);
	c->qid.vers = 0;
	c->qid.type = QTDIR;
	c->aux = p;
	c->dev = 0;

	/* taps. */
	SLIST_INIT(&p->data_taps[0]);	/* already = 0; set to be futureproof */
	SLIST_INIT(&p->data_taps[1]);
	spinlock_init(&p->tap_lock);
	return c;
}

static int
fdmuxgen(struct chan *c, char *unused,
		struct dirtab *tab, int ntab, int i, struct dir *dp)
{
	int id, len;
	struct qid qid;
	struct FDMux *p;

	if (i == DEVDOTDOT) {
		devdir(c, c->qid, devname(), 0, eve, 0555, dp);
		return 1;
	}
	i++;	/* skip . */
	if (tab == 0 || i >= ntab)
		return -1;
	tab += i;
	p = c->aux;
	switch (NETTYPE(tab->qid.path)) {
		case Qm:
			len = qlen(p->q[0]);
			break;
		case Qs:
			len = qlen(p->q[1]);
			break;
		default:
			len = tab->length;
			break;
	}
	id = NETID(c->qid.path);
	qid.path = NETQID(id, tab->qid.path);
	qid.vers = 0;
	qid.type = QTFILE;
	devdir(c, qid, tab->name, len, eve, tab->perm, dp);
	return 1;
}

static struct walkqid *fdmuxwalk(struct chan *c, struct chan *nc, char **name,
								int nname)
{
	struct walkqid *wq;
	struct FDMux *p;

	p = c->aux;
	wq = devwalk(c, nc, name, nname, p->fdmuxdir, ARRAY_SIZE(fdmuxdir), fdmuxgen);
	if (wq != NULL && wq->clone != NULL && wq->clone != c) {
		qlock(&p->qlock);
		kref_get(&p->ref, 1);
		if (c->flag & COPEN) {
			switch (NETTYPE(c->qid.path)) {
				case Qm:
					p->qref[0]++;
					break;
				case Qs:
					p->qref[1]++;
					break;
			}
		}
		qunlock(&p->qlock);
	}
	return wq;
}

static int fdmuxstat(struct chan *c, uint8_t *db, int n)
{
	struct FDMux *p;
	struct dir dir;
	struct dirtab *tab;

	p = c->aux;
	tab = p->fdmuxdir;

	switch (NETTYPE(c->qid.path)) {
		case Qdir:
			devdir(c, c->qid, ".", 0, eve, DMDIR | 0555, &dir);
			break;
		case Qm:
			devdir(c, c->qid, tab[1].name, qlen(p->q[0]), eve, tab[1].perm,
				   &dir);
			break;
		case Qs:
			devdir(c, c->qid, tab[2].name, qlen(p->q[1]), eve, tab[2].perm,
				   &dir);
			break;
	case Qctl:
			devdir(c, c->qid, tab[3].name, 0, eve, tab[3].perm,
				   &dir);
			break;
		default:
			panic("fdmuxstat");
	}
	n = convD2M(&dir, db, n);
	if (n < BIT16SZ)
		error(ENODATA, "convD2M in fdmuxstat failed: %r");
	return n;
}

/*
 *  if the stream doesn't exist, create it
 */
static struct chan *fdmuxopen(struct chan *c, int omode)
{
	ERRSTACK(2);
	struct FDMux *p;

	if (c->qid.type & QTDIR) {
		if (omode & O_WRITE)
			error(EINVAL,
			      "Can only open directories O_READ, mode is %o oct",
			      omode);
		c->mode = openmode(omode);
		c->flag |= COPEN;
		c->offset = 0;
		return c;
	}

	openmode(omode);

	p = c->aux;
	qlock(&p->qlock);
	if (waserror()) {
		qunlock(&p->qlock);
		nexterror();
	}
	switch (NETTYPE(c->qid.path)) {
		case Qm:
			devpermcheck(p->user, p->fdmuxdir[1].perm, omode);
			p->qref[0]++;
			break;
		case Qs:
			devpermcheck(p->user, p->fdmuxdir[2].perm, omode);
			p->qref[1]++;
			break;
	}
	poperror();
	qunlock(&p->qlock);

	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	c->iounit = qiomaxatomic;
	return c;
}

static void fdmuxclose(struct chan *c)
{
	struct FDMux *p;

	p = c->aux;
	qlock(&p->qlock);

	if (c->flag & COPEN) {
		/*
		 *  closing either side hangs up the stream
		 */
		switch (NETTYPE(c->qid.path)) {
			case Qm:
				p->qref[0]--;
				if (p->qref[0] == 0) {
					qhangup(p->q[1], 0);
					qclose(p->q[0]);
				}
				break;
			case Qs:
				p->qref[1]--;
				if (p->qref[1] == 0) {
					qhangup(p->q[0], 0);
					qclose(p->q[1]);
				}
				break;
		}
	}

	qunlock(&p->qlock);
	/*
	 *  free the structure on last close
	 */
	kref_put(&p->ref);
}

static long fdmuxread(struct chan *c, void *va, long n, int64_t m)
{
	struct FDMux *p;
	char *buf;

	p = c->aux;

	switch (NETTYPE(c->qid.path)) {
		case Qdir:
			return devdirread(c, va, n, p->fdmuxdir, ARRAY_SIZE(fdmuxdir),
							  fdmuxgen);
	case Qctl:
		buf = kzmalloc(128, 0);
		snprintf(buf, sizeof(buf),
			 "{pgripid: %d, pid: %d}", p->pgrpid, p->slpid);
		n = readstr(m, va, n, buf);
		kfree(buf);
		return n;

		case Qm:
			if (p->debug)
				printk("pid %d reads m\n", current->pid);
			if (p->dead)
				return -1;
			if (c->flag & O_NONBLOCK)
				return qread_nonblock(p->q[0], va, n);
			else
				return qread(p->q[0], va, n);
		case Qs:
			if (p->dead)
				return -1;
			if (current->pgrp->pgrpid != p->pgrpid)
				rendez_sleep_timeout(&p->r, testready, c, 1000);
			p->active = current->pid;
			if (p->debug)
				printk("pid %d reads s\n", current->pid);
			if (c->flag & O_NONBLOCK)
				return qread_nonblock(p->q[1], va, n);
			else
				return qread(p->q[1], va, n);
		default:
			panic("fdmuxread: impossible qid path");
	}
	return -1;	/* not reached */
}

/*
 *  A write to a closed fdmux causes an EFDMUX error to be thrown.
 */
static long fdmuxwrite(struct chan *c, void *va, long n, int64_t ignored)
{
	ERRSTACK(2);
	struct FDMux *p;
	char buf[16];
	struct proc *target;
	int id;
	int signal = SIGINT;

	if (waserror()) {
		set_errno(EPIPE);
		nexterror();
	}

	p = c->aux;

	switch (NETTYPE(c->qid.path)) {
	/* single letter command a number. */
	case Qctl:
		if (n >= sizeof(buf))
			n = sizeof(buf)-1;
		strncpy(buf, va, n);
		buf[n] = 0;
		id = strtoul(&buf[1], 0, 0);
		switch (buf[0]) {
			case 'k':
				break;
			case 'p':
			case 'l':
				break;
			case 'n':
				if (id == 0)
					id = p->active;
				break;
			case 's':
				signal = SIGSTOP;
				if (id == 0)
					id = p->slpid;
			break;
			default:
				error(EINVAL,
				      "usage: k (kill) or d (debug) or [lnps][optional number]");
		}
		if (p->debug)
			printk("pid %d writes cmd :%s:\n", current->pid, buf);
		switch (buf[0]) {
			case 'd':
				p->debug++;
			case 'k':
				p->dead++;
			case 'p':
				// NO checking. How would we know?
				if (p->debug)
					printk("Set pgrpid to %d\n", id);
				p->pgrpid = id;
				break;
			case 'l':
				if (p->debug)
					printk("Set sleader to %d\n", id);
				p->slpid = id;
				break;
			case 'n':
				target = pid2proc(id);
				if (!target)
					error(ENOENT, "fdmux: no pid %d", id);
				send_posix_signal(target, SIGINT);
				proc_decref(target);
				if (p->debug)
					printk("signalled %d with SIGINT\n", id);
				p->pgrpid = current->pgrp->pgrpid;
				break;
		}
		break;
		case Qm:
		if (p->debug)
			printk("pid %d writes m\n", current->pid);
		if (p->dead) {
			n = -1;
			break;
		}
			if (c->flag & O_NONBLOCK)
				n = qwrite_nonblock(p->q[1], va, n);
			else
				n = qwrite(p->q[1], va, n);
			break;

		case Qs:
		if (p->dead) {
			n = -1;
			break;
		}
		if (current->pgrp->pgrpid != p->pgrpid)
			rendez_sleep_timeout(&p->r, testready, c, 1000);
		p->active = current->pid;
		if (p->debug)
			printk("pid %d writes s\n", current->pid);
			if (c->flag & O_NONBLOCK)
				n = qwrite_nonblock(p->q[0], va, n);
			else
				n = qwrite(p->q[0], va, n);
			break;

		default:
			panic("fdmuxwrite");
	}

	poperror();
	return n;
}

static int fdmuxwstat(struct chan *c, uint8_t *dp, int n)
{
	ERRSTACK(2);
	struct dir *d;
	struct FDMux *p;
	int d1;

	if (c->qid.type & QTDIR)
		error(EPERM, ERROR_FIXME);
	p = c->aux;
	if (strcmp(current->user, p->user) != 0)
		error(EPERM, ERROR_FIXME);
	d = kzmalloc(sizeof(*d) + n, 0);
	if (waserror()) {
		kfree(d);
		nexterror();
	}
	n = convM2D(dp, n, d, (char *)&d[1]);
	if (n == 0)
		error(ENODATA, ERROR_FIXME);
	d1 = NETTYPE(c->qid.path) == Qs;
	if (!emptystr(d->name)) {
		validwstatname(d->name);
		if (strlen(d->name) >= KNAMELEN)
			error(ENAMETOOLONG, ERROR_FIXME);
		if (strncmp(p->fdmuxdir[1 + !d1].name, d->name, KNAMELEN) == 0)
			error(EEXIST, ERROR_FIXME);
		strncpy(p->fdmuxdir[1 + d1].name, d->name, KNAMELEN);
	}
	if (d->mode != ~0UL)
		p->fdmuxdir[d1 + 1].perm = d->mode & 0777;
	poperror();
	kfree(d);
	return n;
}

static void fdmux_wake_cb(struct queue *q, void *data, int filter)
{
	/* If you allocate structs like this on odd byte boundaries, you
	 * deserve what you get.  */
	uintptr_t kludge = (uintptr_t) data;
	int which = kludge & 1;
	struct FDMux *p = (struct FDMux*)(kludge & ~1ULL);
	struct fd_tap *tap_i;

	spin_lock(&p->tap_lock);
	SLIST_FOREACH(tap_i, &p->data_taps[which], link)
		fire_tap(tap_i, filter);
	spin_unlock(&p->tap_lock);
}

static int fdmuxtapfd(struct chan *chan, struct fd_tap *tap, int cmd)
{
	int ret;
	struct FDMux *p;
	int which = 1;
	uint64_t kludge;

	p = chan->aux;
	kludge = (uint64_t)p;
#define DEVFDMUX_LEGAL_DATA_TAPS (FDTAP_FILT_READABLE | FDTAP_FILT_WRITABLE | \
                                 FDTAP_FILT_HANGUP | FDTAP_FILT_ERROR)

	switch (NETTYPE(chan->qid.path)) {
	case Qm:
		which = 0;
		/* fall through */
	case Qs:
		kludge |= which;

		if (tap->filter & ~DEVFDMUX_LEGAL_DATA_TAPS) {
			set_errno(ENOSYS);
			set_errstr("Unsupported #%s data tap %p, must be %p", devname(),
			           tap->filter, DEVFDMUX_LEGAL_DATA_TAPS);
			return -1;
		}
		spin_lock(&p->tap_lock);
		switch (cmd) {
		case (FDTAP_CMD_ADD):
			if (SLIST_EMPTY(&p->data_taps[which]))
				qio_set_wake_cb(p->q[which], fdmux_wake_cb, (void *)kludge);
			SLIST_INSERT_HEAD(&p->data_taps[which], tap, link);
			ret = 0;
			break;
		case (FDTAP_CMD_REM):
			SLIST_REMOVE(&p->data_taps[which], tap, fd_tap, link);
			if (SLIST_EMPTY(&p->data_taps[which]))
				qio_set_wake_cb(p->q[which], 0, (void *)kludge);
			ret = 0;
			break;
		default:
			set_errno(ENOSYS);
			set_errstr("Unsupported #%s data tap command %p", devname(), cmd);
			ret = -1;
		}
		spin_unlock(&p->tap_lock);
		return ret;
	default:
		set_errno(ENOSYS);
		set_errstr("Can't tap #%s file type %d", devname(),
		           NETTYPE(chan->qid.path));
		return -1;
	}
}

struct dev fdmuxdevtab __devtab = {
	.name = "fdmux",

	.reset = devreset,
	.init = fdmuxinit,
	.shutdown = devshutdown,
	.attach = fdmuxattach,
	.walk = fdmuxwalk,
	.stat = fdmuxstat,
	.open = fdmuxopen,
	.create = devcreate,
	.close = fdmuxclose,
	.read = fdmuxread,
//	.bread = NULL,
	.write = fdmuxwrite,
//	.bwrite = NULL,
	.remove = devremove,
	.wstat = fdmuxwstat,
	.power = devpower,
	.chaninfo = devchaninfo,
	.tapfd = fdmuxtapfd,
};
