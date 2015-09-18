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

static uint32_t ioapic_read(uint64_t offset)
{

	uint32_t low;
  if((a20addr & ~0x3) != ((a20addr+len-1) & ~0x3)) {
    BX_PANIC(("I/O APIC read at address 0x" FMT_PHY_ADDRX " spans 32-bit boundary !", a20addr));
    return 1;
  }
  Bit32u value = theIOAPIC->read_aligned(a20addr & ~0x3);
  if(len == 4) { // must be 32-bit aligned
    *((Bit32u *)data) = value;
    return 1;
  }
  // handle partial read, independent of endian-ness
  value >>= (a20addr&3)*8;
  if (len == 1)
    *((Bit8u *) data) = value & 0xff;
  else if (len == 2)
    *((Bit16u *)data) = value & 0xffff;
  else
    BX_PANIC(("Unsupported I/O APIC read at address 0x" FMT_PHY_ADDRX ", len=%d", a20addr, len));


	DPRINTF("ioapic_read offset %s 0x%x\n", ioapicregs[offset].name, (int)offset);

	if (! ioapicregs[offset].mode & 1) {
		fprintf(stderr, "Attempt to read %s, which is %s\n", ioapicregs[offset].name,
			ioapicregs[offset].mode == 0 ?  "reserved" : "writeonly");
		// panic? what to do?
		return (uint32_t) -1;
	}

	// no special cases yet.
	switch (offset) {
	case 1:
		return 0x170011;
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
    case 0x00: // set APIC ID
      {
        Bit8u newid = (value >> 24) & apic_id_mask;
        BX_INFO(("IOAPIC: setting id to 0x%x", newid));
        set_id (newid);
        return;
      }
    case 0x01: // version
    case 0x02: // arbitration id
      BX_INFO(("IOAPIC: could not write, IOREGSEL=0x%02x", ioregsel));
      return;
    default:
      int index = (ioregsel - 0x10) >> 1;
      if (index >= 0 && index < BX_IOAPIC_NUM_PINS) {
        bx_io_redirect_entry_t *entry = ioredtbl + index;
        if (ioregsel&1)
          entry->set_hi_part(value);
        else
          entry->set_lo_part(value);
        char buf[1024];
        entry->sprintf_self(buf);
        BX_DEBUG(("IOAPIC: now entry[%d] is %s", index, buf));
        service_ioapic();
        return;
      }
      BX_PANIC(("IOAPIC: IOREGSEL points to undefined register %02x", ioregsel));

	switch (offset) {
	case 0:
		ioapicregs.id
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
