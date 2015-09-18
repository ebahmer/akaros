/*
 * IOAPIC emulation
 *
 * Copyright 2015 Google Inc.
 *
 * See LICENSE for details.
 */

#include <stdio.h>
#include <sys/types.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <parlib/arch/arch.h>
#include <parlib/ros_debug.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <stdint.h>
#include <err.h>
#include <sys/mman.h>
#include <ros/vmm.h>
#include <vmm/virtio.h>
#include <vmm/virtio_mmio.h>
#include <vmm/virtio_ids.h>
#include <vmm/virtio_config.h>

#define IOAPIC_CONFIG 0x100

int debug_ioapic = 1;
#define DPRINTF(fmt, ...) \
	if (debug_ioapic) { fprintf(stderr, "ioapic: " fmt , ## __VA_ARGS__); }


static struct ioapicinfo ioapicinfo;

enum {
	reserved,
	readonly = 1,
	readwrite = 3,
	writeonly = 2
};

struct {
	char *name;
	int mode;
	uint32_t value;
} ioapicregs[256] = {
[0x00] {.name = "IO", .mode =  readwrite},
[0x01] {.name = "fix me", .mode =  readwrite},
[0x02] {.name = "fix me", .mode = readwrite},
[0x03] {.name = "fix me", .mode = readonly},
[0x04] {.name = "fix me", .mode =  reserved},
[0x05] {.name = "fix me", .mode =  reserved},
[0x06] {.name = "fix me", .mode =  reserved},
[0x07] {.name = "fix me", .mode =  reserved},
[0x08] {.name = "fix me", .mode = readwrite},
[0x09] {.name = "fix me", .mode = readonly},
[0x0A] {.name = "fix me", .mode = readonly},
[0x0B] {.name = "fix me", .mode = writeonly},
[0x0C] {.name = "fix me", .mode = readonly},
[0x0D] {.name = "fix me", .mode = readwrite},
[0x0E] {.name = "fix me", .mode = readwrite},
[0x0F] {.name = "fix me", .mode = readwrite},
[0x10] {.name = "fix me", .mode = readonly},
[0x11] {.name = "fix me", .mode = readonly},
[0x12] {.name = "fix me", .mode = readonly},
[0x13] {.name = "fix me", .mode = readonly},
[0x14] {.name = "fix me", .mode = readonly},
[0x15] {.name = "fix me", .mode = readonly},
[0x16] {.name = "fix me", .mode = readonly},
[0x17] {.name = "fix me", .mode = readonly},
[0x18] {.name = "fix me", .mode = readonly},
[0x19] {.name = "fix me", .mode = readonly},
[0x1A] {.name = "fix me", .mode = readonly},
[0x1B] {.name = "fix me", .mode = readonly},
[0x1C] {.name = "fix me", .mode = readonly},
[0x1D] {.name = "fix me", .mode = readonly},
[0x1E] {.name = "fix me", .mode = readonly},
[0x1F] {.name = "fix me", .mode = readonly},
[0x20] {.name = "fix me", .mode = readonly},
[0x21] {.name = "fix me", .mode = readonly},
[0x22] {.name = "fix me", .mode = readonly},
[0x23] {.name = "fix me", .mode = readonly},
[0x24] {.name = "fix me", .mode = readonly},
[0x25] {.name = "fix me", .mode = readonly},
[0x26] {.name = "fix me", .mode = readonly},
[0x27] {.name = "fix me", .mode = readonly},
[0x28] {.name = "fix me", .mode = readonly},
[0x29 ] {.name = "fix me", .mode =  reserved},
[0x2a] {.name = "fix me", .mode =  reserved},
[0x2b] {.name = "fix me", .mode =  reserved},
[0x2c] {.name = "fix me", .mode =  reserved},
[0x2d] {.name = "fix me", .mode =  reserved},
[0x2E] {.name = "fix me", .mode =  reserved},
[0x2F] {.name = "fix me", .mode = readwrite},
[0x30] {.name = "fix me", .mode = readwrite},
[0x31] {.name = "fix me", .mode = readwrite},
[0x32] {.name = "fix me", .mode = readwrite},
[0x33] {.name = "fix me", .mode = readwrite},
[0x34] {.name = "fix me", .mode = readwrite},
[0x35] {.name = "fix me", .mode = readwrite},
[0x36] {.name = "fix me", .mode = readwrite},
[0x37] {.name = "fix me", .mode = readwrite},
[0x38] {.name = "fix me", .mode = readwrite},
[0x39] {.name = "fix me", .mode = readonly},
[0x3A] {.name = "fix me", .mode =  reserved},
[0x3a]{.name = "fix me", .mode =  reserved},
[0x3b]{.name = "fix me", .mode =  reserved},
[0x3c]{.name = "fix me", .mode =  reserved},
[0x3D]{.name = "fix me", .mode =  reserved},
[0x3E] {.name = "fix me", .mode = readwrite},
[0x3F] {.name = "fix me", .mode =  reserved},
};

static uint32_t ioapic_read(uint64_t offset)
{

	uint32_t low;

	DPRINTF("ioapic_read offset %s 0x%x\n", ioapicregs[offset].name, (int)offset);

	if (! ioapicregs[offset].mode & 1) {
		fprintf(stderr, "Attempt to read %s, which is %s\n", ioapicregs[offset].name,
			ioapicregs[offset].mode == 0 ?  "reserved" : "writeonly");
		// panic? what to do?
		return (uint32_t) -1;
	}

	// no special cases yet.
	switch (offset) {
	default:
		DPRINTF("%s: return %08x\n", ioapicregs[offset].name, ioapicregs[offset].value);
		return ioapicregs[offset].value;
		break;
	}
	return 0;
}

static void ioapic_write(uint64_t offset, uint32_t value)
{
	uint64_t val64;
	uint32_t low, high;

	DPRINTF("ioapic_write offset %s 0x%x value 0x%x\n", ioapicregs[offset].name, (int)offset, value);

	if (! ioapicregs[offset].mode & 2) {
		fprintf(stderr, "Attempt to write %s, which is %s\n", ioapicregs[offset].name,
			ioapicregs[offset].mode == 0 ?  "reserved" : "readonly");
		// panic? what to do?
		return;
	}

	switch (offset) {
	default:
		DPRINTF("%s: Set to %08x\n", ioapicregs[offset].name, value);
		ioapicregs[offset].value = value;
		break;
	}

}

int ioapic(struct vmctl *v, uint64_t gpa, int destreg, uint64_t *regp, int store)
{
	uint32_t offset = gpa & 0xfffff;
	/* basic sanity tests. */
	// TODO: Should be minus the base but FIXME
	offset = gpa & 0xfffff;
	if (offset & 0xf) {
		DPRINTF("bad register offset; low nibl is non-zero\n");
		return -1;
	}
	offset >>= 4;
	if (offset > IOAPIC_CONFIG) {
		DPRINTF("Bad register offset: 0x%x and max is 0x%x\n", gpa, gpa + IOAPIC_CONFIG);
		return -1;
	}

	if (store) {
		ioapic_write(offset, *regp);
		DPRINTF("Write: mov %s to %s @%p val %p\n", regname(destreg), ioapicregs[offset].name, gpa, *regp);
	} else {
		*regp = ioapic_read(offset);
		DPRINTF("Read: Set %s from %s @%p to %p\n", regname(destreg), ioapicregs[offset].name, gpa, *regp);
	}

}
