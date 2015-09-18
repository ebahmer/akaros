/*
 *  ioapic.c IOAPIC emulation logic
 *
 *  Copyright (c) 2011 Jan Kiszka, Siemens AG
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#define IOAPIC_NUM_PINS 24
#define IO_APIC_DEFAULT_ADDRESS 0xfec00000

void ioapic_eoi_broadcast(int vector);

/*
 *  IOAPIC emulation logic - internal interfaces
 *
 *  Copyright (c) 2004-2005 Fabrice Bellard
 *  Copyright (c) 2009      Xiantao Zhang, Intel
 *  Copyright (c) 2011 Jan Kiszka, Siemens AG
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#define MAX_IOAPICS                     1

#define IOAPIC_VERSION                  0x11

#define IOAPIC_LVT_DEST_SHIFT           56
#define IOAPIC_LVT_MASKED_SHIFT         16
#define IOAPIC_LVT_TRIGGER_MODE_SHIFT   15
#define IOAPIC_LVT_REMOTE_IRR_SHIFT     14
#define IOAPIC_LVT_POLARITY_SHIFT       13
#define IOAPIC_LVT_DELIV_STATUS_SHIFT   12
#define IOAPIC_LVT_DEST_MODE_SHIFT      11
#define IOAPIC_LVT_DELIV_MODE_SHIFT     8

#define IOAPIC_LVT_MASKED               (1 << IOAPIC_LVT_MASKED_SHIFT)
#define IOAPIC_LVT_REMOTE_IRR           (1 << IOAPIC_LVT_REMOTE_IRR_SHIFT)

#define IOAPIC_TRIGGER_EDGE             0
#define IOAPIC_TRIGGER_LEVEL            1

/*io{apic,sapic} delivery mode*/
#define IOAPIC_DM_FIXED                 0x0
#define IOAPIC_DM_LOWEST_PRIORITY       0x1
#define IOAPIC_DM_PMI                   0x2
#define IOAPIC_DM_NMI                   0x4
#define IOAPIC_DM_INIT                  0x5
#define IOAPIC_DM_SIPI                  0x6
#define IOAPIC_DM_EXTINT                0x7
#define IOAPIC_DM_MASK                  0x7

#define IOAPIC_VECTOR_MASK              0xff

#define IOAPIC_IOREGSEL                 0x00
#define IOAPIC_IOWIN                    0x10

#define IOAPIC_REG_ID                   0x00
#define IOAPIC_REG_VER                  0x01
#define IOAPIC_REG_ARB                  0x02
#define IOAPIC_REG_REDTBL_BASE          0x10
#define IOAPIC_ID                       0x00

#define IOAPIC_ID_SHIFT                 24
#define IOAPIC_ID_MASK                  0xf

#define IOAPIC_VER_ENTRIES_SHIFT        16

struct ioapic {
	uint64_t ioapicbase;
	uint8_t id;
	uint8_t ioregsel;
	uint32_t irr;
	uint64_t ioredtbl[IOAPIC_NUM_PINS];
};

void ioapic_reset_common(struct ioapic *);

// APIC
/* cpu.c */
// TODO
typedef int X86CPU;
bool cpu_is_bsp(X86CPU *cpu);

/*
 *  APIC support - internal interfaces
 *
 *  Copyright (c) 2004-2005 Fabrice Bellard
 *  Copyright (c) 2011      Jan Kiszka, Siemens AG
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>
 */

/* APIC Local Vector Table */
#define APIC_LVT_TIMER                  0
#define APIC_LVT_THERMAL                1
#define APIC_LVT_PERFORM                2
#define APIC_LVT_LINT0                  3
#define APIC_LVT_LINT1                  4
#define APIC_LVT_ERROR                  5
#define APIC_LVT_NB                     6

/* APIC delivery modes */
#define APIC_DM_FIXED                   0
#define APIC_DM_LOWPRI                  1
#define APIC_DM_SMI                     2
#define APIC_DM_NMI                     4
#define APIC_DM_INIT                    5
#define APIC_DM_SIPI                    6
#define APIC_DM_EXTINT                  7

/* APIC destination mode */
#define APIC_DESTMODE_FLAT              0xf
#define APIC_DESTMODE_CLUSTER           1

#define APIC_TRIGGER_EDGE               0
#define APIC_TRIGGER_LEVEL              1

#define APIC_LVT_TIMER_PERIODIC         (1<<17)
#define APIC_LVT_MASKED                 (1<<16)
#define APIC_LVT_LEVEL_TRIGGER          (1<<15)
#define APIC_LVT_REMOTE_IRR             (1<<14)
#define APIC_INPUT_POLARITY             (1<<13)
#define APIC_SEND_PENDING               (1<<12)

#define ESR_ILLEGAL_ADDRESS (1 << 7)

#define APIC_SV_DIRECTED_IO             (1<<12)
#define APIC_SV_ENABLE                  (1<<8)

#define VAPIC_ENABLE_BIT                0
#define VAPIC_ENABLE_MASK               (1 << VAPIC_ENABLE_BIT)

#define MAX_APICS 255

// TODO -- do we need this?
struct X86CPU {
	int env;
};

struct apic {

//    MemoryRegion io_memory;
    X86CPU *cpu;
	uint32_t apicbase;
	uint32_t ioapicbase;
	uint8_t id;
	uint8_t version;
	uint8_t arb_id;
	uint8_t tpr;
	uint32_t spurious_vec;
	uint8_t log_dest;
	uint8_t dest_mode;
	uint32_t isr[8];  /* in service register */
	uint32_t tmr[8];  /* trigger mode register */
	uint32_t irr[8]; /* interrupt request register */
	uint32_t lvt[APIC_LVT_NB];
	uint32_t esr; /* error register */
	uint32_t icr[2];
	
	uint32_t divide_conf;
	int count_shift;
	uint32_t initial_count;
	int64_t initial_count_load_time;
	int64_t next_time;
	int idx;
	//QEMUTimer *timer;
	int64_t timer_expiry;
	int sipi_vector;
	int wait_for_sipi;
	
	uint32_t vapic_control;
	struct ioapic *vapic;
	uint64_t vapic_paddr; /* note: persistence via kvmvapic */
};

void set_base(struct apic *s, uint64_t val);
void set_tpr(struct apic *s, uint8_t val);
uint8_t get_tpr(struct apic *s);
void enable_tpr_reporting(struct apic *s, bool enable);
void vapic_base_update(struct apic *s);
void external_nmi(struct apic *s);
void reset(struct apic *s);


typedef struct VAPICState {
	uint8_t tpr;
	uint8_t isr;
	uint8_t zero;
	uint8_t irr;
	uint8_t enabled;
} VAPICState;

//extern bool apic_report_tpr_access;

void apic_report_irq_delivered(int delivered);
bool apic_next_timer(struct apic *s, int64_t current_time);
void apic_enable_tpr_access_reporting(struct ioapic *d, bool enable);
void apic_enable_vapic(struct ioapic *d, uint64_t paddr);

//void vapic_report_tpr_access(struct ioapic *dev, CPUState *cpu, target_ulong ip,
//                             TPRAccess access);


/* apic.c */
void apic_deliver_irq(uint8_t dest, uint8_t dest_mode, uint8_t delivery_mode,
                      uint8_t vector_num, uint8_t trigger_mode);
int apic_accept_pic_intr(struct apic *s);
void apic_deliver_pic_intr(struct apic *s, int level);
void apic_deliver_nmi(struct apic *d);
int apic_get_interrupt(struct apic *s);
void apic_reset_irq_delivered(void);
int apic_get_irq_delivered(void);
void cpu_set_apic_base(struct apic *s, uint64_t val);
uint64_t cpu_get_apic_base(struct apic *s);
void cpu_set_apic_tpr(struct apic *s, uint8_t val);
uint8_t cpu_get_apic_tpr(struct apic *s);
void apic_init_reset(struct apic *s);
void apic_sipi(struct apic *s);
//void apic_handle_tpr_access_report(struct ioapic *d, target_ulong ip,
//                                   TPRAccess access);
void apic_poll_irq(struct apic *d);
void apic_designate_bsp(struct apic *d, bool bsp);

/* pc.c */
struct ioapic *cpu_get_current_apic(void);

