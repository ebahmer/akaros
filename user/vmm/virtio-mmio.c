/*
 * Virtio MMIO bindings
 *
 * Copyright (c) 2011 Linaro Limited
 *
 * Author:
 *  Peter Maydell <peter.maydell@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <sys/types.h>
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

int debug_virtio_mmio = 1;
#define DPRINTF(fmt, ...) \
	if (debug_virtio_mmio) { printf("virtio_mmio: " fmt , ## __VA_ARGS__); }


#define VIRT_MAGIC 0x74726976 /* 'virt' */
#define VIRT_VERSION 1
#define VIRT_VENDOR 0x554D4551 /* 'QEMU' */


typedef struct {
	int state; // not used yet. */
	uint64_t bar;
	uint32_t status;
	int qsel; // queue we are on.
	int pagesize;
	int page_shift;
	int host_features_sel;
	int guest_features_sel;
	struct vqdev *vqs;
	int ndevs;
} mmiostate;

static mmiostate mmio;

void register_virtio_mmio(struct vqdev *vqs, int nvq, uint64_t virtio_base)
{
	mmio.bar = virtio_base;
	mmio.vqs = vqs;
	mmio.ndevs = nvq;
}

#if 0
static void setupconsole(void *v)
{
	// try to make linux happy.
	// this is not really endian safe but ... well ... WE'RE ON THE SAME MACHINE
	write32(v+VIRTIO_MMIO_MAGIC_VALUE, ('v' | 'i' << 8 | 'r' << 16 | 't' << 24));
	// no constant for this is defined anywhere. It's just 1.
	write32(v+VIRTIO_MMIO_VERSION, 1);
	write32(v+VIRTIO_MMIO_DEVICE_ID, VIRTIO_ID_CONSOLE);
	write32(v+VIRTIO_MMIO_QUEUE_NUM_MAX, 1);
	write32(v+VIRTIO_MMIO_QUEUE_PFN, 0);
}
#endif


static uint32_t virtio_mmio_read(uint64_t gpa);
void dumpvirtio_mmio(FILE *f, uint64_t v)
{
	fprintf(f, "VIRTIO_MMIO_MAGIC_VALUE: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_MAGIC_VALUE));
	fprintf(f, "VIRTIO_MMIO_VERSION: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_VERSION));
	fprintf(f, "VIRTIO_MMIO_DEVICE_ID: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_DEVICE_ID));
	fprintf(f, "VIRTIO_MMIO_VENDOR_ID: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_VENDOR_ID));
	fprintf(f, "VIRTIO_MMIO_DEVICE_FEATURES: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_DEVICE_FEATURES));
	fprintf(f, "VIRTIO_MMIO_DEVICE_FEATURES_SEL: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_DEVICE_FEATURES_SEL));
	fprintf(f, "VIRTIO_MMIO_DRIVER_FEATURES: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_DRIVER_FEATURES));
	fprintf(f, "VIRTIO_MMIO_DRIVER_FEATURES_SEL: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_DRIVER_FEATURES_SEL));
	fprintf(f, "VIRTIO_MMIO_GUEST_PAGE_SIZE: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_GUEST_PAGE_SIZE));
	fprintf(f, "VIRTIO_MMIO_QUEUE_SEL: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_QUEUE_SEL));
	fprintf(f, "VIRTIO_MMIO_QUEUE_NUM_MAX: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_QUEUE_NUM_MAX));
	fprintf(f, "VIRTIO_MMIO_QUEUE_NUM: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_QUEUE_NUM));
	fprintf(f, "VIRTIO_MMIO_QUEUE_ALIGN: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_QUEUE_ALIGN));
	fprintf(f, "VIRTIO_MMIO_QUEUE_PFN: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_QUEUE_PFN));
	fprintf(f, "VIRTIO_MMIO_QUEUE_READY: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_QUEUE_READY));
	fprintf(f, "VIRTIO_MMIO_QUEUE_NOTIFY: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_QUEUE_NOTIFY));
	fprintf(f, "VIRTIO_MMIO_INTERRUPT_STATUS: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_INTERRUPT_STATUS));
	fprintf(f, "VIRTIO_MMIO_INTERRUPT_ACK: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_INTERRUPT_ACK));
	fprintf(f, "VIRTIO_MMIO_STATUS: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_STATUS));
	fprintf(f, "VIRTIO_MMIO_QUEUE_DESC_LOW: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_QUEUE_DESC_LOW));
	fprintf(f, "VIRTIO_MMIO_QUEUE_DESC_HIGH: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_QUEUE_DESC_HIGH));
	fprintf(f, "VIRTIO_MMIO_QUEUE_AVAIL_LOW: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_QUEUE_AVAIL_LOW));
	fprintf(f, "VIRTIO_MMIO_QUEUE_AVAIL_HIGH: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_QUEUE_AVAIL_HIGH));
	fprintf(f, "VIRTIO_MMIO_QUEUE_USED_LOW: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_QUEUE_USED_LOW));
	fprintf(f, "VIRTIO_MMIO_QUEUE_USED_HIGH: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_QUEUE_USED_HIGH));
	fprintf(f, "VIRTIO_MMIO_CONFIG_GENERATION: 0x%x\n", virtio_mmio_read(v+VIRTIO_MMIO_CONFIG_GENERATION));
}

/* We're going to attempt to make mmio stateless, since the real machine is in
 * the guest kernel. From what we know so far, all IO to the mmio space is 32 bits.
 */
static uint32_t virtio_mmio_read(uint64_t gpa)
{

	unsigned int offset = gpa - mmio.bar;
	
	DPRINTF("virtio_mmio_read offset 0x%x\n", (int)offset);

	/* If no backend is present, we treat most registers as
	 * read-as-zero, except for the magic number, version and
	 * vendor ID. This is not strictly sanctioned by the virtio
	 * spec, but it allows us to provide transports with no backend
	 * plugged in which don't confuse Linux's virtio code: the
	 * probe won't complain about the bad magic number, but the
	 * device ID of zero means no backend will claim it.
	 */
	if (mmio.ndevs == 0) {
		switch (offset) {
		case VIRTIO_MMIO_MAGIC_VALUE:
			return VIRT_MAGIC;
		case VIRTIO_MMIO_VERSION:
			return VIRT_VERSION;
		case VIRTIO_MMIO_VENDOR_ID:
			return VIRT_VENDOR;
		default:
			return 0;
		}
	}


    // WTF? Does this happen? 
    if (offset >= VIRTIO_MMIO_CONFIG) {
	    fprintf(stderr, "Whoa. Reading past mmio config space? What gives?\n");
	    return -1;
#if 0
	    offset -= VIRTIO_MMIO_CONFIG;
	    switch (size) {
	    case 1:
		    return virtio_config_readb(vdev, offset);
	    case 2:
		    return virtio_config_readw(vdev, offset);
	    case 4:
		    return virtio_config_readl(vdev, offset);
	    default:
		    abort();
	    }
#endif
    }

#if 0
    if (size != 4) {
        DPRINTF("wrong size access to register!\n");
        return 0;
    }
#endif
    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:
	    return VIRT_MAGIC;
    case VIRTIO_MMIO_VERSION:
	    return VIRT_VERSION;
    case VIRTIO_MMIO_DEVICE_ID:
	    return mmio.vqs[mmio.qsel].dev;
    case VIRTIO_MMIO_VENDOR_ID:
	    return VIRT_VENDOR;
    case VIRTIO_MMIO_DEVICE_FEATURES:
// ???	    if (proxy->host_features_sel) {
	    return 0;
//	    }
	    return mmio.vqs[mmio.qsel].features;
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
//	    if (!virtio_queue_get_num(vdev, vdev->queue_sel)) {
//            return 0;
//        }
	    // no clue. 
	    return mmio.vqs[mmio.qsel].qnum;
    case VIRTIO_MMIO_QUEUE_PFN:
	    return mmio.vqs[mmio.qsel].pfn;
    case VIRTIO_MMIO_INTERRUPT_STATUS:
	    return mmio.vqs[mmio.qsel].isr;
    case VIRTIO_MMIO_STATUS:
	    return mmio.vqs[mmio.qsel].status;
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
    case VIRTIO_MMIO_DRIVER_FEATURES:
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
    case VIRTIO_MMIO_GUEST_PAGE_SIZE:
    case VIRTIO_MMIO_QUEUE_SEL:
    case VIRTIO_MMIO_QUEUE_NUM:
    case VIRTIO_MMIO_QUEUE_ALIGN:
    case VIRTIO_MMIO_QUEUE_READY:
    case VIRTIO_MMIO_INTERRUPT_ACK:
	    fprintf(stderr, "read of write-only register@%p\n", (void *)gpa);
        return 0;
    default:
	    fprintf(stderr, "bad register offset@%p\n", (void *)gpa);
        return 0;
    }
    return 0;
}

static void virtio_mmio_write(uint64_t gpa, uint32_t value)
{
	unsigned int offset = gpa - mmio.bar;
	
	DPRINTF("virtio_mmio_write offset 0x%x\n", (int)offset);

    if (offset >= VIRTIO_MMIO_CONFIG) {
	    fprintf(stderr, "Whoa. Writing past mmio config space? What gives?\n");
#if 0
        offset -= VIRTIO_MMIO_CONFIG;
        switch (size) {
        case 1:
            virtio_config_writeb(vdev, offset, value);
            break;
        case 2:
            virtio_config_writew(vdev, offset, value);
            break;
        case 4:
            virtio_config_writel(vdev, offset, value);
            break;
        default:
            abort();
        }
#endif
        return;
    }
#if 0
    if (size != 4) {
        DPRINTF("wrong size access to register!\n");
        return;
    }
#endif
    switch (offset) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        mmio.host_features_sel = value;
        break;
#if 0
    case VIRTIO_MMIO_GUESTFEATURES:
        if (!mmio.guest_features_sel) {
            mmio.guest_features_sel = value;
        }
        break;
    case VIRTIO_MMIO_GUESTFEATURESSEL:
        proxy->guest_features_sel = value;
        break;
#endif
    case VIRTIO_MMIO_GUEST_PAGE_SIZE:
	    mmio.pagesize = value;
	    DPRINTF("guest page size %d bytes\n", mmio.pagesize);
        break;
#if 0
    case VIRTIO_MMIO_QUEUESEL:
        if (value < VIRTIO_QUEUE_MAX) {
            vdev->queue_sel = value;
        }
        break;
    case VIRTIO_MMIO_QUEUENUM:
        DPRINTF("mmio_queue write %d max %d\n", (int)value, VIRTQUEUE_MAX_SIZE);
        virtio_queue_set_num(vdev, vdev->queue_sel, value);
        break;
    case VIRTIO_MMIO_QUEUEALIGN:
        virtio_queue_set_align(vdev, vdev->queue_sel, value);
        break;
    case VIRTIO_MMIO_QUEUEPFN:
        if (value == 0) {
            virtio_reset(vdev);
        } else {
            virtio_queue_set_addr(vdev, vdev->queue_sel,
                                  value << proxy->guest_page_shift);
        }
        break;
    case VIRTIO_MMIO_QUEUENOTIFY:
        if (value < VIRTIO_QUEUE_MAX) {
            virtio_queue_notify(vdev, value);
        }
        break;
    case VIRTIO_MMIO_INTERRUPTACK:
        vdev->isr &= ~value;
        virtio_update_irq(vdev);
        break;
#endif
    case VIRTIO_MMIO_STATUS:
        if (!(value & VIRTIO_CONFIG_S_DRIVER_OK)) {
            printf("VIRTIO_MMIO_STATUS write: NOT OK! 0x%x\n", value);
        }

	mmio.status |= value & 0xff;

        if (value & VIRTIO_CONFIG_S_DRIVER_OK) {
            printf("VIRTIO_MMIO_STATUS write: OK! 0x%x\n", value);
        }

        break;
    case VIRTIO_MMIO_MAGIC_VALUE:
    case VIRTIO_MMIO_VERSION:
    case VIRTIO_MMIO_DEVICE_ID:
    case VIRTIO_MMIO_VENDOR_ID:
//    case VIRTIO_MMIO_HOSTFEATURES:
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        DPRINTF("write to readonly register\n");
        break;

    default:
        DPRINTF("bad register offset 0x%x\n", offset);
    }

}

static char *modrmreg[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"};

void virtio_mmio(struct vmctl *v)
{
	int advance = 3; /* how much to move the IP forward at the end. 3 is a good default. */
	// All virtio accesses seem to be 32 bits.
	// valp points to a place to get or put the value. 
	uint32_t *valp;
	DPRINTF("v is %p\n", v);
	// regp points to the register in hw_trapframe from which
	// to load or store a result.
	uint64_t *regp;

	// Duh, which way did he go George? Which way did he go? 
	// First hit on Google gets you there!
	// This is the guest physical address of the access.
	// This is nice, because if we ever go with more complete
	// instruction decode, knowing this gpa reduces our work:
	// we don't have to find the source address in registers,
	// only the register holding or receiving the value.
	uint64_t gpa = v->gpa;
	DPRINTF("gpa is %p\n", gpa);

	// To find out what to do, we have to look at
	// RIP. Technically, we should read RIP, walk the page tables
	// to find the PA, and read that. But we're in the kernel, so
	// we take a shortcut for now: read the low 30 bits and use
	// that as the kernel PA, or our VA, and see what's
	// there. Hokey. Works.
	uint8_t *kva = (void *)(v->regs.tf_rip & 0x3fffffff);
	DPRINTF("kva is %p\n", kva);

	if ((kva[0] != 0x8b) && (kva[0] != 0x89)) {
		fprintf(stderr, "%s: can't handle instruction 0x%x\n", kva[0]);
		return;
	}

	uint16_t ins = *(uint16_t *)kva;
	DPRINTF("ins is %04x\n", ins);
	
	int write = (kva[0] == 0x8b) ? 0 : 1;
	if (write)
		valp = (uint32_t *)gpa;

	int mod = kva[1]>>6;
	switch (mod) {
		case 0: 
		case 3:
			advance = 2;
			break;
		case 1:
			advance = 3;
			break;
		case 2: 
			advance = 6;
			break;
	}
	/* the dreaded mod/rm byte. */
	int destreg = (ins>>11) & 7;
	// Our primitive approach wins big here.
	// We don't have to decode the register or the offset used
	// in the computation; that was done by the CPU and is the gpa.
	// All we need to know is which destination or source register it is.
	switch (destreg) {
	case 0:
		regp = &v->regs.tf_rax;
		break;
	case 1:
		regp = &v->regs.tf_rcx;
		break;
	case 2:
		regp = &v->regs.tf_rdx;
		break;
	case 3:
		regp = &v->regs.tf_rbx;
		break;
	case 4:
		regp = &v->regs.tf_rsp; // uh, right.
		break;
	case 5:
		regp = &v->regs.tf_rbp;
		break;
	case 6:
		regp = &v->regs.tf_rsi;
		break;
	case 7:
		regp = &v->regs.tf_rdi;
		break;
	}

	if (write) {
		virtio_mmio_write(gpa, *regp);
		DPRINTF("Write: mov %s to from @%p val %p\n", modrmreg[destreg], gpa, *regp);
	} else {
		*regp = virtio_mmio_read(gpa);
		DPRINTF("Read: Set %s from @%p to %p\n", modrmreg[destreg], gpa, *regp);
	}

	DPRINTF("Advance rip by %d bytes to %p\n", advance, v->regs.tf_rip);
	v->regs.tf_rip += advance;
}
