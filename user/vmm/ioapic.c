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
#define IOAPIC_NUM_PINS 24

int debug_ioapic = 1;
int apic_id_mask = 0xf0;

#define DPRINTF(fmt, ...) \
	if (debug_ioapic) { fprintf(stderr, "ioapic: " fmt , ## __VA_ARGS__); }


struct ioapic {
	int id;
	int reg;
	uint32_t arbid;
	uint64_t value[256];
};

static struct ioapic ioapic[1];

enum {
	reserved,
	readonly = 1,
	readwrite = 3,
	writeonly = 2
};

struct {
	char *name;
	int mode;
} ioapicregs[256] = {
[0x00] {.name = "ID", .mode =  readwrite},
[0x01] {.name = "version", .mode =  readonly},
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

static uint32_t ioapic_read(int ix, uint64_t offset)
{
	uint32_t ret = (uint32_t)-1;
	uint32_t reg = ioapic[ix].reg;
	int index;


	if (offset == 0) {
		DPRINTF("ioapic_read ix %x return 0x%x\n", ix, reg);
		return reg;
	}

	DPRINTF("ioapic_read %s 0x%x\n", ioapicregs[reg].name, (int)reg);
	if (! ioapicregs[reg].mode & 1) {
		fprintf(stderr, "Attempt to read %s, which is %s\n", ioapicregs[reg].name,
			ioapicregs[reg].mode == 0 ?  "reserved" : "writeonly");
		// panic? what to do?
		return (uint32_t) -1;
	}

	switch (reg) {
	case 0:
		return ioapic[ix].id;
		break;
	case 1:
		return 0x170011;
		break;
	case 2:
		return ioapic[ix].arbid;
		break;
	default:
		index = (reg - 0x10) >> 1;
		if (index >= 0 && index < IOAPIC_NUM_PINS) {
			//bx_io_redirect_entry_t *entry = ioredtbl + index;
			//data = (ioregsel&1) ? entry->get_hi_part() : entry->get_lo_part();
			ret = (reg & 1) ? ioapic[ix].value[index]>>32 : ioapic[ix].value[index];
			DPRINTF("%s: return %08x\n", ioapicregs[reg].name, ret);
			return ret;
		}
		return ret;
		break;
	}
	return 0;
}

static void ioapic_write(int ix, uint64_t offset, uint32_t value)
{
	uint32_t ret;
	uint32_t reg = ioapic[ix].reg;
	int index;

	if (offset == 0) {
		DPRINTF("ioapic_write ix %x set reg 0x%x\n", ix, value);
		ioapic[ix].reg = value;
		return;
	}

	DPRINTF("ioapic_write reg %s 0x%x\n", ioapicregs[reg].name, (int)reg);
	if (! ioapicregs[reg].mode & 2) {
		fprintf(stderr, "Attempt to write %s, which is %s\n", ioapicregs[reg].name,
			ioapicregs[reg].mode == 0 ?  "reserved" : "readonly");
		// panic? what to do?
	}

	switch (reg) {
	case 0:
		ioapic[ix].id = value;
		break;
	default:
		index = (reg - 0x10) >> 1;
		if (index >= 0 && index < IOAPIC_NUM_PINS) {
			//bx_io_redirect_entry_t *entry = ioredtbl + index;
			//data = (ioregsel&1) ? entry->get_hi_part() : entry->get_lo_part();
			uint64_t val = ioapic[ix].value[index];
			if (reg & 1) {
				val = (uint32_t) val | (((uint64_t)value) << 32);
			} else {
				val = ((val>>32)<<32) | value;
			}
			ioapic[ix].value[index] = val;
			DPRINTF("%s: set %08x to %016x\n", ioapicregs[reg].name, reg, val);
			}
		break;
	}

}

int do_ioapic(struct vmctl *v, uint64_t gpa, int destreg, uint64_t *regp, int store)
{
	// TODO: compute an index for the ioapic array. 
	int ix = 0;
	uint32_t offset = gpa & 0xfffff;
	/* basic sanity tests. */
	DPRINTF("%s: %p 0x%x %p %s\n", __func__, (void *)gpa, destreg, regp, store ? "write" : "read");

	if ((offset != 0) && (offset != 0x10)) {
		DPRINTF("Bad register offset: 0x%x and has to be 0x0 or 0x10\n", offset);
		return -1;
	}

	if (store) {
		ioapic_write(ix, offset, *regp);
		DPRINTF("Write: mov %s to %s @%p val %p\n", regname(destreg), ioapicregs[offset].name, gpa, *regp);
	} else {
		*regp = ioapic_read(ix, offset);
		DPRINTF("Read: Set %s from %s @%p to %p\n", regname(destreg), ioapicregs[offset].name, gpa, *regp);
	}

}
