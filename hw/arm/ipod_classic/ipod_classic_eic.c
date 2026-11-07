/*
 * S5L8702 external interrupt controller and system-alive control. See
 * include/hw/arm/ipod_classic/ipod_classic_eic.h for the register map.
 *
 * WHY THIS DEVICE EXISTS: the retailOS standby path. After about two minutes
 * idle on battery or USB power the firmware calls 0x2200279c (IRAM): caches
 * off, SDRAM into self-refresh, then 0x22002dcc, which
 *
 *   saves the clock registers and gates the clocks,
 *   ALVCON |= 0x3f00000, then ALVCON |= 1, and spins until bit 0 reads 1,
 *   WFI,
 *   ALVCON |= 0x1002 and spins until bit 12 reads 1,
 *   clears bits 0 and 25..20 and restores the clocks.
 *
 * Before that the firmware arms its wake sources here and in the VIC: EIC
 * group 3 bit 3 (GPIO 0x7b, the PMU interrupt, level low) and group 6 bit 28
 * (GPIO 4), with VIC0 sources 0, 3 and 19 the only ones left enabled. An
 * unimplemented block reads ALVCON as zero, so the first spin never ends and
 * the machine hangs with the screen frozen.
 *
 * ALVCON is therefore a plain register: Rockbox documents bit 0 as the
 * oscillator select and bits 25..20 as the alive-clock divider, and both
 * spins wait for the guest's own write to take effect. Nothing models the
 * clock change itself.
 *
 * The interrupt groups follow Rockbox's driver (gpio-s5l8702.c): an edge pin
 * latches INTSTAT on a transition to its INTLEVEL; a level pin's INTSTAT
 * follows the line while it matches INTLEVEL, and clearing it does not stick
 * while the line is still active. Only pins with a connected source can set
 * INTSTAT, so every other pin behaves as it did with the block unimplemented.
 * Other offsets read zero and log under -d unimp.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "hw/arm/ipod_classic/ipod_classic_eic.h"

static unsigned eic_group(unsigned gpio)
{
    return 6 - (gpio >> 5);
}

static unsigned eic_bit(unsigned gpio)
{
    return (0x18 - (gpio & 0x18)) | (gpio & 7);
}

static void eic_update(IPodClassicEICState *s)
{
    for (unsigned g = 0; g < S5L8702_EIC_GROUPS; g++) {
        /* Level pins: pending while the line sits at its active level. */
        uint32_t lvl = s->type[g] & s->driven[g];
        uint32_t active = ~(s->input[g] ^ s->level[g]);
        s->stat[g] = (s->stat[g] & ~lvl) | (active & lvl);

        qemu_set_irq(s->irq[g], (s->stat[g] & s->en[g]) != 0);
    }
}

static void eic_set_input(void *opaque, int gpio, int value)
{
    IPodClassicEICState *s = opaque;
    unsigned g = eic_group(gpio);
    uint32_t bit = 1u << eic_bit(gpio);
    uint32_t old = s->input[g];

    s->input[g] = value ? (old | bit) : (old & ~bit);

    /* Edge pins latch a transition onto their active level. */
    if ((old ^ s->input[g]) & bit & ~s->type[g] & s->driven[g] &&
        !((s->input[g] ^ s->level[g]) & bit)) {
        s->stat[g] |= bit;
    }
    eic_update(s);
}

qemu_irq ipod_classic_eic_pin(IPodClassicEICState *s, unsigned gpio)
{
    assert(gpio < S5L8702_EIC_PINS);
    s->driven[eic_group(gpio)] |= 1u << eic_bit(gpio);
    return qdev_get_gpio_in(DEVICE(s), gpio);
}

static uint32_t *eic_group_reg(IPodClassicEICState *s, hwaddr offset)
{
    if (offset < S5L8702_EIC_INTLEVEL ||
        offset >= S5L8702_EIC_INTTYPE + 4 * S5L8702_EIC_GROUPS) {
        return NULL;
    }
    unsigned g = (offset & 0x1f) / 4;
    if (g >= S5L8702_EIC_GROUPS || (offset & 3)) {
        return NULL;
    }
    switch (offset & ~0x1f) {
    case S5L8702_EIC_INTLEVEL:
        return &s->level[g];
    case S5L8702_EIC_INTSTAT:
        return &s->stat[g];
    case S5L8702_EIC_INTEN:
        return &s->en[g];
    case S5L8702_EIC_INTTYPE:
        return &s->type[g];
    }
    return NULL;
}

static uint64_t ipod_classic_eic_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    IPodClassicEICState *s = opaque;
    uint32_t *reg;

    if (offset == S5L8702_EIC_ALVCON) {
        return s->alvcon;
    }
    reg = eic_group_reg(s, offset);
    if (reg) {
        return *reg;
    }
    qemu_log_mask(LOG_UNIMP, "%s: read  (size %d, offset 0x%05" HWADDR_PRIx
                  ")\n", TYPE_IPOD_CLASSIC_EIC, size, offset);
    return 0;
}

static void ipod_classic_eic_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IPodClassicEICState *s = opaque;
    uint32_t *reg;

    if (offset == S5L8702_EIC_ALVCON) {
        s->alvcon = value;
        return;
    }
    reg = eic_group_reg(s, offset);
    if (!reg) {
        qemu_log_mask(LOG_UNIMP, "%s: write (size %d, offset 0x%05"
                      HWADDR_PRIx ", value 0x%08" PRIx64 ")\n",
                      TYPE_IPOD_CLASSIC_EIC, size, offset, value);
        return;
    }
    if ((offset & ~0x1f) == S5L8702_EIC_INTSTAT) {
        *reg &= ~value;
    } else {
        *reg = value;
    }
    eic_update(s);
}

static const MemoryRegionOps ipod_classic_eic_ops = {
    .read = ipod_classic_eic_read,
    .write = ipod_classic_eic_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void ipod_classic_eic_reset(DeviceState *dev)
{
    IPodClassicEICState *s = IPOD_CLASSIC_EIC(dev);

    s->alvcon = 0;
    for (unsigned g = 0; g < S5L8702_EIC_GROUPS; g++) {
        s->level[g] = s->stat[g] = s->en[g] = s->type[g] = 0;
        s->input[g] = ~0u;      /* sources idle high until they say otherwise */
    }
    eic_update(s);
}

static void ipod_classic_eic_init(Object *obj)
{
    IPodClassicEICState *s = IPOD_CLASSIC_EIC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_classic_eic_ops, s,
                          TYPE_IPOD_CLASSIC_EIC, S5L8702_EIC_MEM_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    for (unsigned g = 0; g < S5L8702_EIC_GROUPS; g++) {
        sysbus_init_irq(sbd, &s->irq[g]);
    }
    qdev_init_gpio_in(DEVICE(obj), eic_set_input, S5L8702_EIC_PINS);
}

static const VMStateDescription vmstate_ipod_classic_eic = {
    .name = TYPE_IPOD_CLASSIC_EIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(alvcon, IPodClassicEICState),
        VMSTATE_UINT32_ARRAY(level, IPodClassicEICState, S5L8702_EIC_GROUPS),
        VMSTATE_UINT32_ARRAY(stat, IPodClassicEICState, S5L8702_EIC_GROUPS),
        VMSTATE_UINT32_ARRAY(en, IPodClassicEICState, S5L8702_EIC_GROUPS),
        VMSTATE_UINT32_ARRAY(type, IPodClassicEICState, S5L8702_EIC_GROUPS),
        VMSTATE_UINT32_ARRAY(input, IPodClassicEICState, S5L8702_EIC_GROUPS),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_classic_eic_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = ipod_classic_eic_reset;
    dc->vmsd = &vmstate_ipod_classic_eic;
    dc->desc = "S5L8702 external interrupt controller";
}

static const TypeInfo ipod_classic_eic_info = {
    .name          = TYPE_IPOD_CLASSIC_EIC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodClassicEICState),
    .instance_init = ipod_classic_eic_init,
    .class_init    = ipod_classic_eic_class_init,
};

static void ipod_classic_eic_register_types(void)
{
    type_register_static(&ipod_classic_eic_info);
}

type_init(ipod_classic_eic_register_types)
