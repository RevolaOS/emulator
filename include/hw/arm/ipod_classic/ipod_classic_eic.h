/*
 * S5L8702 external interrupt controller and system-alive control, 0x39A00000
 * (EIC_BASE / SYSALV_BASE).
 *
 * Register map from Rockbox, firmware/export/s5l87xx.h:
 *
 *   +0x00        ALVCON       always-on domain clock control
 *   +0x80 + 4g   INTLEVEL(g)  active level per pin, 0 low, 1 high
 *   +0xA0 + 4g   INTSTAT(g)   pending bits, write 1 to clear
 *   +0xC0 + 4g   INTEN(g)     enables
 *   +0xE0 + 4g   INTTYPE(g)   0 edge, 1 level
 *
 * Seven groups of 32. GPIO n is group 6 - (n >> 5), bit
 * (0x18 - (n & 0x18)) | (n & 7) (EIC_GROUP/EIC_INDEX). Groups 6..0 drive VIC
 * sources 0, 1, 2, 3, 31, 32, 33 (Rockbox's INT_EXT0..6 handlers).
 *
 * Inputs are GPIO-numbered qdev GPIO lines. Only connected pins can raise an
 * interrupt; an unconnected pin never sets its INTSTAT bit, which keeps every
 * pin without a model reading as it did when the block was unimplemented.
 */
#ifndef HW_ARM_IPOD_CLASSIC_EIC_H
#define HW_ARM_IPOD_CLASSIC_EIC_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_IPOD_CLASSIC_EIC "s5l8702.eic"
OBJECT_DECLARE_SIMPLE_TYPE(IPodClassicEICState, IPOD_CLASSIC_EIC)

#define S5L8702_EIC_MEM_SIZE    0x100000
#define S5L8702_EIC_GROUPS      7
#define S5L8702_EIC_PINS        (S5L8702_EIC_GROUPS * 32)

#define S5L8702_EIC_ALVCON      0x00
#define S5L8702_EIC_INTLEVEL    0x80
#define S5L8702_EIC_INTSTAT     0xA0
#define S5L8702_EIC_INTEN       0xC0
#define S5L8702_EIC_INTTYPE     0xE0

/* GPIO numbers of the pins Apple's firmware arms as wake sources. */
#define S5L8702_EIC_GPIO_WAKE   0x04    /* driven by click-wheel key presses */
#define S5L8702_EIC_GPIO_PMU    0x7b    /* PCF50635 INT, active low */

struct IPodClassicEICState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t alvcon;
    uint32_t level[S5L8702_EIC_GROUPS];
    uint32_t stat[S5L8702_EIC_GROUPS];
    uint32_t en[S5L8702_EIC_GROUPS];
    uint32_t type[S5L8702_EIC_GROUPS];
    uint32_t input[S5L8702_EIC_GROUPS];     /* electrical level per bit */
    uint32_t driven[S5L8702_EIC_GROUPS];    /* bits with a connected source */

    qemu_irq irq[S5L8702_EIC_GROUPS];
};

/* The input for GPIO `gpio`, marking the pin as having a source. */
qemu_irq ipod_classic_eic_pin(IPodClassicEICState *s, unsigned gpio);

#endif
