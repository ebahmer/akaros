/*
 * APIC emulation
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

#define APIC_CONFIG 0x100

int debug_apic = 1;
#define DPRINTF(fmt, ...) \
	if (debug_apic) { fprintf(stderr, "apic: " fmt , ## __VA_ARGS__); }


struct apicinfo {
	int state; // not used yet. */
	int id;
	uint64_t apicbase;
	uint64_t ioapicbase;

	uint32_t dfr;
	uint32_t icrhigh;
	uint32_t icrlow;
	uint32_t irr[8];
	uint32_t lvterr;
	uint32_t lvttimer;
	uint32_t icount;
};

static struct apicinfo apicinfo;

char *apic_names[256] = {
[0x00] "Reserved",
[0x01] "Reserved",
[0x02] "Local APIC ID Register Read/Write.",
[0x03] "Local APIC Version Register Read Only.",
[0x04] "Reserved",
[0x05] "Reserved",
[0x06] "Reserved",
[0x07] "Reserved",
[0x08] "Task Priority Register (TPR) Read/Write.",
[0x09] "Arbitration Priority Register1 (APR) Read Only.",
[0x0A] "Processor Priority Register (PPR) Read Only.",
[0x0B] "EOI Register Write Only.",
[0x0C] "Remote Read Register1 (RRD) Read Only",
[0x0D] "Logical Destination Register Read/Write.",
[0x0E] "Destination Format Register Read/Write (see Section",
[0x0F] "Spurious Interrupt Vector Register Read/Write (see Section 10.9.",
[0x10] "In-Service Register (ISR); bits 31:0 Read Only.",
[0x11] "In-Service Register (ISR); bits 63:32 Read Only.",
[0x12] "In-Service Register (ISR); bits 95:64 Read Only.",
[0x13] "In-Service Register (ISR); bits 127:96 Read Only.",
[0x14] "In-Service Register (ISR); bits 159:128 Read Only.",
[0x15] "In-Service Register (ISR); bits 191:160 Read Only.",
[0x16] "In-Service Register (ISR); bits 223:192 Read Only.",
[0x17] "In-Service Register (ISR); bits 255:224 Read Only.",
[0x18] "Trigger Mode Register (TMR); bits 31:0 Read Only.",
[0x19] "Trigger Mode Register (TMR); bits 63:32 Read Only.",
[0x1A] "Trigger Mode Register (TMR); bits 95:64 Read Only.",
[0x1B] "Trigger Mode Register (TMR); bits 127:96 Read Only.",
[0x1C] "Trigger Mode Register (TMR); bits 159:128 Read Only.",
[0x1D] "Trigger Mode Register (TMR); bits 191:160 Read Only.",
[0x1E] "Trigger Mode Register (TMR); bits 223:192 Read Only.",
[0x1F] "Trigger Mode Register (TMR); bits 255:224 Read Only.",
[0x20] "Interrupt Request Register (IRR); bits 31:0 Read Only.",
[0x21] "Interrupt Request Register (IRR); bits 63:32 Read Only.",
[0x22] "Interrupt Request Register (IRR); bits 95:64 Read Only.",
[0x23] "Interrupt Request Register (IRR); bits 127:96 Read Only.",
[0x24] "Interrupt Request Register (IRR); bits 159:128 Read Only.",
[0x25] "Interrupt Request Register (IRR); bits 191:160 Read Only.",
[0x26] "Interrupt Request Register (IRR); bits 223:192 Read Only.",
[0x27] "Interrupt Request Register (IRR); bits 255:224 Read Only.",
[0x28] "Error Status Register Read Only.",
[0x29 ] "Reserved",
[0x2a] "Reserved",
[0x2b] "Reserved",
[0x2c] "Reserved",
[0x2d] "Reserved",
[0x2E] "Reserved",
[0x2F] "LVT CMCI Register Read/Write.",
[0x30] "Interrupt Command Register (ICR); bits 0-31 Read/Write.",
[0x31] "Interrupt Command Register (ICR); bits 32-63 Read/Write.",
[0x32] "LVT Timer Register Read/Write.",
[0x33] "LVT Thermal Sensor Register2 Read/Write.",
[0x34] "LVT Performance Monitoring Counters Register3 Read/Write.",
[0x35] "LVT LINT0 Register Read/Write.",
[0x36] "LVT LINT1 Register Read/Write.",
[0x37] "LVT Error Register Read/Write.",
[0x38] "Initial Count Register (for Timer) Read/Write.",
[0x39] "Current Count Register (for Timer) Read Only.",
[0x3A] "Reserved",
[0x3a]"Reserved",
[0x3b]"Reserved",
[0x3c]"Reserved",
[0x3D]"Reserved",
[0x3E] "Divide Configuration Register (for Timer) Read/Write.",
[0x3F] "Reserved",
};

static uint32_t apic_read(uint64_t offset)
{

	uint32_t low;
	
	DPRINTF("apic_read offset %s 0x%x\n", apic_names[offset],(int)offset);

	if (offset >= APIC_CONFIG) {
		fprintf(stderr, "Whoa. Reading past apic space? What gives?\n");
		return -1;
	}
	
	// We don't use the x16 addresses because the x2apic doesn't have them. This needs to be for x2apics.
	switch (offset) {
	case 0x2: 
		// return our PIC.
		return apicinfo.id;
		break;
	case 0x3: 
		return 2; // VERSION?
		break;
	case 0xd: 
		return 0;
		break;
	case 0x10: 
		return 0;
		break;
	case 0x20:
	case 0x21:
	case 0x22:
	case 0x23:
	case 0x24:
	case 0x25:
	case 0x26:
	case 0x27:
		DPRINTF("%s: return %08x\n", apic_names[offset], apicinfo.irr[offset-0x20]);
		return apicinfo.irr[offset-0x20];
		break;
	case 0x28:
		return 0;
		break;
	case 0x30:
		return apicinfo.icrlow;
		break;
	case 0x31:
		return apicinfo.icrhigh;
		break;
	case 0x32:
		return apicinfo.lvttimer;
		break;
	case 0x37:
		return apicinfo.lvterr;
		break;
	case 0x38:
		return apicinfo.icount;
		break;
	default:
		fprintf(stderr, "%s: Attempt to read register@0x%x: %s\n", __func__, offset, apic_names[offset]);
		return 0;
	}
	return 0;
}

static void apic_write(uint64_t offset, uint32_t value)
{
	uint64_t val64;
	uint32_t low, high;
	
	DPRINTF("apic_write offset %s 0x%x value 0x%x\n", apic_names[offset], (int)offset, value);

	switch (offset) {
	case 0x2: 
		// return our PIC.
		apicinfo.id = value;
		break;
	case 0x20:
	case 0x21:
	case 0x22:
	case 0x23:
	case 0x24:
	case 0x25:
	case 0x26:
	case 0x27:
		DPRINTF("%s: set to  %08x\n", apic_names[offset], value);
		apicinfo.irr[offset-0x20] = value;
		break;
	case 0x30:
		apicinfo.icrlow = value;
		DPRINTF("ICR LOW: set to %x\n", value>>24);
		break;
	case 0x31:
		apicinfo.icrhigh = value;
		DPRINTF("ICR HIGH: set dest to %x\n", value>>24);
		break;
	case 0x32:
		apicinfo.lvttimer = value;
		break;
	case 0x37:
		apicinfo.lvterr = value;
		break;
	case 0x38:
		apicinfo.icount = value;
		break;
	default:
		// FIXME ... lots to do here. we only support one apic.
		fprintf(stderr, "%s: Attempt to write register@0x%x: %s\n", __func__, offset, apic_names[offset]);
	}

}

int apic(struct vmctl *v, uint64_t gpa, int destreg, uint64_t *regp, int store)
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
	if (offset > APIC_CONFIG) {
		DPRINTF("Bad register offset: 0x%x and max is 0x%x\n", gpa, gpa + APIC_CONFIG);
		return -1;
	}

	if (store) {
		apic_write(offset, *regp);
		DPRINTF("Write: mov %s to %s @%p val %p\n", regname(destreg), apic_names[(uint8_t)offset], gpa, *regp);
	} else {
		*regp = apic_read(offset);
		DPRINTF("Read: Set %s from %s @%p to %p\n", regname(destreg), apic_names[(uint8_t)offset], gpa, *regp);
	}

}
