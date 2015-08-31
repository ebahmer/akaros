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

int debug_apic = 0;
#define DPRINTF(fmt, ...) \
	if (debug_apic) { printf("virtio_mmio: " fmt , ## __VA_ARGS__); }


typedef struct {
	int state; // not used yet. */
	uint64_t apicbase;
	uint64_t ioapicbase;
} apic;

static apic apic;

char *apic_names[] = {
	[0x20] "APICID",
};

static uint32_t apic_read(uint64_t gpa)
{

	unsigned int offset = gpa - apic.apicbase;
	uint32_t low;
	
	DPRINTF("apic_read offset %s 0x%x\n", virtio_names[offset],(int)offset);

	if (offset >= APIC_CONFIG) {
	    fprintf(stderr, "Whoa. Reading past apic space? What gives?\n");
	    return -1;
    }


    if (size != 4) {
        DPRINTF("wrong size access to register!\n");
        return 0;
    }

    switch (offset) {
    case 0x20: 
	    return 0;
    default:
	    fprintf(stderr, "bad register offset@%p\n", (void *)gpa);
	    return 0;
    }
    return 0;
}

static void apic_write(uint64_t gpa, uint32_t value)
{
	uint64_t val64;
	uint32_t low, high;
	unsigned int offset = gpa - apic.apicbase;
	
	DPRINTF("apic_write offset %s 0x%x value 0x%x\n", virtio_names[offset], (int)offset, value);

    if (offset >= APIC_CONFIG) {
	    fprintf(stderr, "Whoa. Writing past mmio config space? What gives?\n");
	    return;
    }

    if (size != 4) {
        DPRINTF("wrong size access to register!\n");
        return;
    }

    switch (offset) {
    default:
        DPRINTF("bad register offset 0x%x\n", offset);
    }

}

int apic(struct vmctl *v, uint64_t gpa, int destreg, uint64_t *regp, int store)
{
	if (store) {
		apic_write(gpa, *regp);
		DPRINTF("Write: mov %s to %s @%p val %p\n", regname(destreg), virtio_names[(uint8_t)gpa], gpa, *regp);
	} else {
		*regp = apic_read(gpa);
		DPRINTF("Read: Set %s from %s @%p to %p\n", regname(destreg), virtio_names[(uint8_t)gpa], gpa, *regp);
	}

}
