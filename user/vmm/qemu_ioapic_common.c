/*
 *  IOAPIC emulation logic - common bits of emulated and KVM kernel model
 *
 *  Copyright (c) 2004-2005 Fabrice Bellard
 *  Copyright (c) 2009      Xiantao Zhang, Intel
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
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <vmm/pic.h>

/* ioapic_no count start from 0 to MAX_IOAPICS,
 * remove as static variable from ioapic_common_init.
 * now as a global variable, let child to increase the counter
 * then we can drop the 'instance_no' argument
 * and convert to our QOM's realize function
 */
int ioapic_no;

void ioapic_reset_common(ioapic *s)
{
    int i;

    s->id = 0;
    s->ioregsel = 0;
    s->irr = 0;
    for (i = 0; i < IOAPIC_NUM_PINS; i++) {
        s->ioredtbl[i] = 1 << IOAPIC_LVT_MASKED_SHIFT;
    }
}

static void ioapic_common_realize(ioapic *s, Error **errp)
{

    if (ioapic_no >= MAX_IOAPICS) {
        error_setg(errp, "Only %d ioapics allowed", MAX_IOAPICS);
        return;
    }

    realize(s);

    ioapic_no++;
}

