/*
 *  APIC support - common bits of emulated and KVM kernel model
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
#include "hw/i386/apic.h"
#include "hw/i386/apic_internal.h"
#include "trace.h"
#include "sysemu/kvm.h"
#include "hw/qdev.h"
#include "hw/sysbus.h"

static int apic_irq_delivered;
bool apic_report_tpr_access;

void cpu_set_apic_base(struct apic *apic, uint64_t val)
{
    trace_cpu_set_apic_base(val);

    set_base(apic, val);
}

uint64_t cpu_get_apic_base(apic *s)
{
    if (s) {
        trace_cpu_get_apic_base((uint64_t)s->apicbase);
        return s->apicbase;
    } else {
        trace_cpu_get_apic_base(MSR_IA32_APICBASE_BSP);
        return MSR_IA32_APICBASE_BSP;
    }
}

void cpu_set_apic_tpr(apic *s, uint8_t val)
{
	set_tpr(s, val);
}

uint8_t cpu_get_apic_tpr(apic *s)
{
    APICCommonState *s;
    APICCommonClass *info;

    if (!dev) {
        return 0;
    }

    s = APIC_COMMON(dev);
    info = APIC_COMMON_GET_CLASS(s);

    return info->get_tpr(s);
}

void apic_enable_tpr_access_reporting(apic *s, bool enable)
{
    APICCommonState *s = APIC_COMMON(dev);
    APICCommonClass *info = APIC_COMMON_GET_CLASS(s);

    apic_report_tpr_access = enable;
    if (info->enable_tpr_reporting) {
        info->enable_tpr_reporting(s, enable);
    }
}

void apic_enable_vapic(apic *s, uint64_t paddr)
{
    APICCommonState *s = APIC_COMMON(dev);
    APICCommonClass *info = APIC_COMMON_GET_CLASS(s);

    s->vapic_paddr = paddr;
    info->vapic_base_update(s);
}

void apic_handle_tpr_access_report(apic *s, target_ulong ip,
                                   TPRAccess access)
{
    APICCommonState *s = APIC_COMMON(dev);

    vapic_report_tpr_access(s->vapic, CPU(s->cpu), ip, access);
}

void apic_report_irq_delivered(int delivered)
{
    apic_irq_delivered += delivered;

    trace_apic_report_irq_delivered(apic_irq_delivered);
}

void apic_reset_irq_delivered(void)
{
    /* Copy this into a local variable to encourage gcc to emit a plain
     * register for a sys/sdt.h marker.  For details on this workaround, see:
     * https://sourceware.org/bugzilla/show_bug.cgi?id=13296
     */
    volatile int a_i_d = apic_irq_delivered;
    trace_apic_reset_irq_delivered(a_i_d);

    apic_irq_delivered = 0;
}

int apic_get_irq_delivered(void)
{
    trace_apic_get_irq_delivered(apic_irq_delivered);

    return apic_irq_delivered;
}

void apic_deliver_nmi(apic *s)
{
    APICCommonState *s = APIC_COMMON(dev);
    APICCommonClass *info = APIC_COMMON_GET_CLASS(s);

    info->external_nmi(s);
}

bool apic_next_timer(APICCommonState *s, int64_t current_time)
{
    int64_t d;

    /* We need to store the timer state separately to support APIC
     * implementations that maintain a non-QEMU timer, e.g. inside the
     * host kernel. This open-coded state allows us to migrate between
     * both models. */
    s->timer_expiry = -1;

    if (s->lvt[APIC_LVT_TIMER] & APIC_LVT_MASKED) {
        return false;
    }

    d = (current_time - s->initial_count_load_time) >> s->count_shift;

    if (s->lvt[APIC_LVT_TIMER] & APIC_LVT_TIMER_PERIODIC) {
        if (!s->initial_count) {
            return false;
        }
        d = ((d / ((uint64_t)s->initial_count + 1)) + 1) *
            ((uint64_t)s->initial_count + 1);
    } else {
        if (d >= s->initial_count) {
            return false;
        }
        d = (uint64_t)s->initial_count + 1;
    }
    s->next_time = s->initial_count_load_time + (d << s->count_shift);
    s->timer_expiry = s->next_time;
    return true;
}

void apic_init_reset(apic *s)
{
    APICCommonState *s;
    APICCommonClass *info;
    int i;

    if (!dev) {
        return;
    }
    s = APIC_COMMON(dev);
    s->tpr = 0;
    s->spurious_vec = 0xff;
    s->log_dest = 0;
    s->dest_mode = 0xf;
    memset(s->isr, 0, sizeof(s->isr));
    memset(s->tmr, 0, sizeof(s->tmr));
    memset(s->irr, 0, sizeof(s->irr));
    for (i = 0; i < APIC_LVT_NB; i++) {
        s->lvt[i] = APIC_LVT_MASKED;
    }
    s->esr = 0;
    memset(s->icr, 0, sizeof(s->icr));
    s->divide_conf = 0;
    s->count_shift = 0;
    s->initial_count = 0;
    s->initial_count_load_time = 0;
    s->next_time = 0;
    s->wait_for_sipi = !cpu_is_bsp(s->cpu);

    if (s->timer) {
        timer_del(s->timer);
    }
    s->timer_expiry = -1;

    info = APIC_COMMON_GET_CLASS(s);
    if (info->reset) {
        info->reset(s);
    }
}

void apic_designate_bsp(apic *s, bool bsp)
{
    if (dev == NULL) {
        return;
    }

    APICCommonState *s = APIC_COMMON(dev);
    if (bsp) {
        s->apicbase |= MSR_IA32_APICBASE_BSP;
    } else {
        s->apicbase &= ~MSR_IA32_APICBASE_BSP;
    }
}

static void apic_reset_common(apic *s)
{
    APICCommonState *s = APIC_COMMON(dev);
    APICCommonClass *info = APIC_COMMON_GET_CLASS(s);
    uint32_t bsp;

    bsp = s->apicbase & MSR_IA32_APICBASE_BSP;
    s->apicbase = APIC_DEFAULT_ADDRESS | bsp | MSR_IA32_APICBASE_ENABLE;

    s->vapic_paddr = 0;
    info->vapic_base_update(s);

    apic_init_reset(dev);
}

static void apic_common_realize(apic *s, Error **errp)
{
    APICCommonState *s = APIC_COMMON(dev);
    APICCommonClass *info;
    static DeviceState *vapic;
    static int apic_no;
    static bool mmio_registered;

    if (apic_no >= MAX_APICS) {
        error_setg(errp, "%s initialization failed.",
                   object_get_typename(OBJECT(dev)));
        return;
    }
    s->idx = apic_no++;

    info = APIC_COMMON_GET_CLASS(s);
    info->realize(dev, errp);
    if (!mmio_registered) {
        ICCBus *b = ICC_BUS(qdev_get_parent_bus(dev));
        memory_region_add_subregion(b->apic_address_space, 0, &s->io_memory);
        mmio_registered = true;
    }

    /* Note: We need at least 1M to map the VAPIC option ROM */
    if (!vapic && s->vapic_control & VAPIC_ENABLE_MASK &&
        ram_size >= 1024 * 1024) {
        vapic = sysbus_create_simple("kvmvapic", -1, NULL);
    }
    s->vapic = vapic;
    if (apic_report_tpr_access && info->enable_tpr_reporting) {
        info->enable_tpr_reporting(s, true);
    }

}

