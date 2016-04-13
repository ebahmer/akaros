/* Copyright (c) 2010 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * Devfs: filesystem interfaces to devices.  For now, we just create the
 * needed/discovered devices in KFS in its /dev/ folder.  In the future, we
 * might want to do something like nodes like other Unixes. */

#pragma once

#include <vfs.h>
#include <kfs.h>

struct file *make_device(char *path, int mode, int type,
                         struct file_operations *fop);

