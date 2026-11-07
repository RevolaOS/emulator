/*
 * NXP PCF50635 power management unit, at I2C address 0xe6 (7-bit 0x73).
 *
 * The first chip Apple's retailOS talks to. 0x08325a20 starts the transfer and
 * then blocks on RTXC event 61 with nothing but a retry loop behind it, so an
 * iPod classic with nothing on I2C0 boots as far as its idle task and stays
 * there for ever -- which is exactly where this machine stopped before this
 * file existed.
 *
 * The bus protocol is a register-pointer device: a write starts with the
 * register number and the pointer advances after every byte; a read carries on
 * from wherever the pointer was left. That the pointer survives the end of a
 * transaction is not a guess -- Rockbox's i2c_rd() sends the register number in
 * one transaction and reads in a second one (i2c-s5l8702.c, i2c_wr_internal
 * with len 0 followed by i2c_rd_internal), and Apple's driver does the same.
 *
 * Register numbers are from Rockbox's firmware/export/pcf5063x.h, with the
 * three undocumented ones (INT6 0x85, INT6M 0x86, GPIOSTAT 0x87) from
 * firmware/target/arm/s5l8702/ipod6g/pmu-target.h.
 *
 * WHAT IS MODELLED
 *
 *   - The register file, which is what most of this chip is to a driver:
 *     regulator voltages, enables, interrupt masks and standby control get
 *     written and, when read back, have to read back.
 *   - The A/D converter. ADCC1 bit 0 starts a conversion; ADCS1/ADCS2/ADCS3
 *     return a 10-bit result with ADCRDY set. The battery inputs return a
 *     reading changeable while running; see pmu_start_adc.
 *   - The interrupt status registers INT1..INT5 and INT6, which are
 *     read-to-clear on the real chip and therefore must not be a plain
 *     register file: a driver that reads them expects the bits to go away.
 *   - External power presence in MBCS1 (battery, USB or adapter), changeable
 *     while running; a change latches the INT1 insert/remove events, and
 *     for USB also the INT2 EXTON2 edge and OOCSTAT EXTON2 presence that
 *     firmware 2.0.5 actually uses (see the header).
 *   - The hold switch in GPIOSTAT, changeable while running; a change latches
 *     the INT6 hold event.
 *   - The INT output, on SoC GPIO 0x7b (see pmu_update_irq).
 *
 * WHAT IS NOT
 *
 *   - Setting the RTC. Reads return host time (see pmu_reg_read); writes are
 *     stored and never read back.
 *   - Charging, the ON/OFF controller and the regulators. Writing them changes
 *     nothing, because there is no voltage in here for a voltage to mean.
 *
 * ASSUMED, and marked as such because nothing derives them:
 *
 *   - VERSION (0x00) and VARIANT (0x01). No dump of a real PCF50635 exists in
 *     any of our sources, and neither Rockbox nor iPodLinux reads them. They
 *     answer zero. If the firmware turns out to compare them against something
 *     the disassembly will say what, and then they get a real value.
 *   - The default battery reading, 844 out of 1023. Through Rockbox's own
 *     conversion (pmu-6g.c: raw * 2000 / 1023 + 2250) that is 3900 mV, an
 *     ordinary charged-but-not-full lithium cell. Nothing in our sources says
 *     what a real one reads; the number is chosen to be unremarkable.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "sysemu/rtc.h"
#include "hw/arm/ipod_classic/ipod_classic_pmu.h"

/*
 * PMUBATT=N sets the raw battery reading at start-up, 0..1023; the machine's
 * "battery" property changes it later (ipod_classic_pmu_set_battery). The
 * default 844 is 3900 mV only through ROCKBOX's conversion; how Apple's power
 * manager scales the same number is not known, so the reading is given raw.
 */
static uint32_t pmu_initial_battery(void)
{
    const char *e = getenv("PMUBATT");
    if (e && *e) {
        return MIN(strtoul(e, NULL, 0), S5L8702_PMU_ADC_MAX);
    }
    return S5L8702_PMU_ADC_BATTERY_RAW;
}

/*
 * PMUCHARGER picks the external power at start-up; the machine's "power"
 * property changes it later (ipod_classic_pmu_set_power).
 *
 * An emulated iPod has no cell to discharge, so "sitting in a dock" is at least
 * as honest a description of it as "running on a battery that never moves".
 * Which of USB or adapter a real device would report depends on what it is
 * plugged into, and we are plugged into nothing, so it is a model choice:
 * unset means battery, "adapter" the wall adapter, anything else USB.
 */
static IPodClassicPMUPower pmu_initial_power(void)
{
    const char *e = getenv("PMUCHARGER");

    if (!e || !*e) {
        return IPOD_CLASSIC_PMU_POWER_BATTERY;
    }
    return strcmp(e, "adapter") ? IPOD_CLASSIC_PMU_POWER_USB
                                : IPOD_CLASSIC_PMU_POWER_ADAPTER;
}

/*
 * The INT output, active low: low while any INT1..INT5/INT6 bit is set that
 * its INTnM mask does not mask. Rockbox's pmu-6g.c registers it as a level-low
 * external interrupt on GPIO 0x7b (GPIO_EINT_PMU), reads INT1..INT6 to clear
 * it, and its comment says that read "will also raise the PMU IRQ pin".
 * Apple's firmware arms the same pin as a standby wake source.
 *
 * MEASURED with firmware 2.0.5: it leaves the INT1 plug events unmasked
 * (INT1M 0xb0) and answers a power change by reading INT1..INT6 and then
 * MBCS1, so plugging or unplugging takes effect at once -- leaving the
 * Charging screen on unplug, and waking from standby on plug-in. The adapter
 * events it acts on are in INT1; the USB ones are in INT2 (see the header).
 */
static void pmu_update_irq(IPodClassicPMUState *s)
{
    bool pending = s->reg[PCF50635_REG_INT6] & ~s->reg[PCF50635_REG_INT6M];

    for (int i = 0; i < 5; i++) {
        pending |= s->reg[PCF5063X_REG_INT1 + i] & ~s->reg[PCF5063X_REG_INT1M + i];
    }
    qemu_set_irq(s->irq, !pending);
}

void ipod_classic_pmu_set_power(IPodClassicPMUState *s, IPodClassicPMUPower power)
{
    static const uint8_t inserted[] = {
        [IPOD_CLASSIC_PMU_POWER_USB] = PCF5063X_INT1_USBINS,
        [IPOD_CLASSIC_PMU_POWER_ADAPTER] = PCF5063X_INT1_ADPINS,
    };
    static const uint8_t removed[] = {
        [IPOD_CLASSIC_PMU_POWER_USB] = PCF5063X_INT1_USBREM,
        [IPOD_CLASSIC_PMU_POWER_ADAPTER] = PCF5063X_INT1_ADPREM,
    };

    if (power == s->power) {
        return;
    }
    /* The events latch in INT1/INT2 until the firmware reads them (read to clear). */
    s->reg[PCF5063X_REG_INT1] |= removed[s->power] | inserted[power];
    if (power == IPOD_CLASSIC_PMU_POWER_USB) {
        s->reg[PCF5063X_REG_INT2] |= PCF5063X_INT2_EXTON2R;
    } else if (s->power == IPOD_CLASSIC_PMU_POWER_USB) {
        s->reg[PCF5063X_REG_INT2] |= PCF5063X_INT2_EXTON2F;
    }
    s->power = power;
    pmu_update_irq(s);
}

void ipod_classic_pmu_set_hold(IPodClassicPMUState *s, bool hold)
{
    if (hold == s->hold) {
        return;
    }
    /* Latches in INT6 until the firmware reads it (read to clear). */
    s->reg[PCF50635_REG_INT6] |= PCF50635_INT6_HOLD;
    s->hold = hold;
    pmu_update_irq(s);
}

void ipod_classic_pmu_set_battery(IPodClassicPMUState *s, uint32_t raw)
{
    s->battery_raw = MIN(raw, S5L8702_PMU_ADC_MAX);
}

/*
 * One conversion, finished at once. Firmware 2.0.5 converts two inputs
 * (PMUTRACE): the battery, as ADCC1 0x1d (BATSNS_SUBTR, average of 16,
 * 10 bits), and ADCIN1, as ADCC1 0x7f with ADCC2 0x02 (ratiometric) and
 * ADCC3 0x01 (ACCSW_EN), which is Rockbox's accessory-identification setup
 * (adc-6g.c). Each time it polls ADCS3 for ADCRDY and then reads ADCS1.
 * The battery inputs return battery_raw; every other input returns the fixed
 * default, which the firmware has always been given for them.
 */
static void pmu_start_adc(IPodClassicPMUState *s)
{
    uint8_t mux = s->reg[PCF5063X_REG_ADCC1] & PCF5063X_ADCC1_MUX_MASK;
    uint32_t raw = (mux == PCF5063X_ADCC1_MUX_BATSNS_RES ||
                    mux == PCF5063X_ADCC1_MUX_BATSNS_SUBTR)
                   ? s->battery_raw : S5L8702_PMU_ADC_BATTERY_RAW;

    s->reg[PCF5063X_REG_ADCS1] = (raw >> 2) & 0xff;
    s->reg[PCF5063X_REG_ADCS2] = 0;
    s->reg[PCF5063X_REG_ADCS3] = PCF5063X_ADCS3_ADCRDY | (raw & 0x03);
}

static uint8_t pmu_reg_read(IPodClassicPMUState *s, uint8_t addr)
{
    uint8_t val = s->reg[addr];

    switch (addr) {
    case PCF5063X_REG_INT1:
    case PCF5063X_REG_INT2:
    case PCF5063X_REG_INT3:
    case PCF5063X_REG_INT4:
    case PCF5063X_REG_INT5:
    case PCF50635_REG_INT6:
        /* Read to clear, which is what releases the INT line. */
        s->reg[addr] = 0;
        pmu_update_irq(s);
        break;
    case PCF50635_REG_GPIOSTAT:
        /*
         * The hold switch, from s->hold rather than s->reg[0x87], so that a
         * guest write to this register cannot move the switch. See the long
         * note at the reset function for what the bit gates.
         */
        val = s->hold ? 0 : PCF50635_GPIOSTAT_HOLD_OFF;
        break;
    case PCF5063X_REG_OOCSTAT:
        /*
         * On/off-controller status, forced to a healthy powered value. Apple's
         * power manager polls this; with BATOK/SYSOK clear it concludes the
         * battery is empty and software-resets ("battery low", tag "--------").
         * A plain register file leaves it stale/zero. See the header.
         */
        val = PCF5063X_OOCSTAT_HEALTHY;
        if (s->power == IPOD_CLASSIC_PMU_POWER_USB) {
            val |= PCF5063X_OOCSTAT_EXTON2;
        }
        break;
    case PCF5063X_REG_MBCS1:
        /*
         * Main-battery-charger status. The stale value reads as a spurious
         * TBAT_BELOW temperature fault; force the honest "temp OK, no charger"
         * state instead. See the header.
         */
        val = PCF5063X_MBCS1_HEALTHY;
        /*
         * External power, from s->power (see pmu_initial_power).
         *
         * MEASURED: on battery the firmware plays and judges the charge from
         * the battery conversion alone. With the reading at 0 it shows
         * "Low Battery -- Connect to Power" (TChargingModeLowPowerCntlr,
         * built at 0x0820ca90) within about two minutes and powers off.
         *
         * WHICH KIND of external power matters, and getting it wrong was
         * MEASURED: reporting the wall ADAPTER puts the device into its
         * full-screen "Charging" mode and it stops playing -- which is what a
         * real iPod does on a charger. USB power leaves it usable.
         */
        if (s->power == IPOD_CLASSIC_PMU_POWER_ADAPTER) {
            val |= PCF5063X_MBCS1_ADAPTPRES | PCF5063X_MBCS1_ADAPTOK;
        } else if (s->power == IPOD_CLASSIC_PMU_POWER_USB) {
            val |= PCF5063X_MBCS1_USBPRES | PCF5063X_MBCS1_USBOK;
        }
        break;
    case PCF5063X_REG_RTCSC:
    case PCF5063X_REG_RTCSC + 1:
    case PCF5063X_REG_RTCSC + 2:
    case PCF5063X_REG_RTCSC + 3:
    case PCF5063X_REG_RTCSC + 4:
    case PCF5063X_REG_RTCSC + 5:
    case PCF5063X_REG_RTCSC + 6:
        /*
         * THE CLOCK RUNS. Measured (PMUTRACE=1): the firmware re-reads this
         * whole block, 0x59..0x5f, over and over, and a plain register file
         * answers zero every time -- a real part's seconds field cannot stay
         * still. That is visible on screen too: the Clock and Calendar screens
         * show a fixed nonsense date.
         *
         * PCF506xx keeps these fields in BCD, and the block is
         * second, minute, hour, weekday, day, month, year (year is 00..99).
         * qemu_get_timedate is the same source hw/rtc/pl031.c uses, so -rtc on
         * the command line works here as it does everywhere else in QEMU.
         */
        {
            struct tm tm;
            unsigned v;

            qemu_get_timedate(&tm, 0);
            switch (addr - PCF5063X_REG_RTCSC) {
            case 0: v = tm.tm_sec;          break;
            case 1: v = tm.tm_min;          break;
            case 2: v = tm.tm_hour;         break;
            case 3: v = tm.tm_wday;         break;
            case 4: v = tm.tm_mday;         break;
            case 5: v = tm.tm_mon + 1;      break;
            default: v = tm.tm_year % 100;  break;
            }
            val = (uint8_t)(((v / 10) << 4) | (v % 10));
        }
        break;
    default:
        break;
    }

    /*
     * PMUTRACE=1: which registers does Apple's power manager actually consult?
     * Counting each address, and printing the first few reads of each, says
     * which ones without burying the firmware's own semihosting output on the
     * same stream.
     */
    if (getenv("PMUTRACE")) {
        static uint32_t seen[256];
        if (seen[addr]++ < 3) {
            fprintf(stderr, "PMUTRACE: read reg 0x%02x -> 0x%02x\n", addr, val);
        }
    }

    return val;
}

static void pmu_reg_write(IPodClassicPMUState *s, uint8_t addr, uint8_t val)
{
    /* PMUTRACE=1 also prints the first write of each value to each register. */
    if (getenv("PMUTRACE")) {
        static bool seen[256][256];
        if (!seen[addr][val]) {
            seen[addr][val] = true;
            fprintf(stderr, "PMUTRACE: write reg 0x%02x <- 0x%02x\n", addr, val);
        }
    }

    switch (addr) {
    case PCF5063X_REG_INT1:
    case PCF5063X_REG_INT2:
    case PCF5063X_REG_INT3:
    case PCF5063X_REG_INT4:
    case PCF5063X_REG_INT5:
    case PCF50635_REG_INT6:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: INT%d is read-only\n", __func__,
                      addr == PCF50635_REG_INT6 ? 6 : addr - 1);
        return;
    case PCF5063X_REG_ADCC1:
        s->reg[addr] = val;
        if (val & PCF5063X_ADCC1_ADCSTART) {
            pmu_start_adc(s);
        }
        return;
    case PCF5063X_REG_INT1M:
    case PCF5063X_REG_INT2M:
    case PCF5063X_REG_INT3M:
    case PCF5063X_REG_INT4M:
    case PCF5063X_REG_INT5M:
    case PCF50635_REG_INT6M:
        s->reg[addr] = val;
        pmu_update_irq(s);
        return;
    default:
        s->reg[addr] = val;
        return;
    }
}

static int ipod_classic_pmu_event(I2CSlave *i2c, enum i2c_event event)
{
    IPodClassicPMUState *s = IPOD_CLASSIC_PMU(i2c);

    switch (event) {
    case I2C_START_SEND:
        /* The next byte is the register number, not data. */
        s->addr_pending = true;
        /*
         * Remember where the pointer stood so a register write can put it back
         * at STOP; see the note above ipod_classic_pmu_send.
         */
        s->addr_saved = s->addr;
        s->wrote_data = false;
        break;
    case I2C_START_RECV:
        s->wrote_data = false;
        break;
    case I2C_FINISH:
        /*
         * A register WRITE (an address byte followed by one or more data bytes)
         * must not leave the shared register pointer moved: another task may
         * have a read in flight that already set the pointer and is about to do
         * its own START_RECV. Restore it. An address-only "set the pointer"
         * write (no data byte) leaves the pointer where it was aimed, which is
         * exactly what the set-pointer-then-read pattern needs.
         */
        if (s->wrote_data) {
            s->addr = s->addr_saved;
        }
        s->wrote_data = false;
        break;
    case I2C_NACK:
        break;
    default:
        break;
    }
    return 0;
}

/*
 * THE REGISTER POINTER, and why a write puts it back at STOP.
 *
 * Apple's driver reads a register as two I2C transactions -- one that sets
 * the register pointer, a STOP, then the read -- and the pointer survives the
 * STOP by design. Firmware 2.0.5 brackets both halves with a pair of
 * acquire calls (through 0x0802cf34, arguments 0x11 and 5) and the matching
 * releases (through 0x0802cf3c); argument 5 alone also brackets the codec
 * driver's I2C accesses, so it guards the bus. The hold read 0x082dc010, the
 * MBCS1 reads 0x082dbe0c and 0x082dbfd0, the interrupt read 0x082dc2bc and
 * the register writers all do this. An I2CTRACE=2 trace of boot and menu
 * navigation shows every set-pointer immediately followed by its read.
 *
 * The model also restores the pointer at STOP after a register write (above),
 * so a write that does land between another read's two halves leaves that
 * read's pointer intact; otherwise the hold read would return the wrong
 * register and drop the key it gates. The pointer still auto-advances within
 * a transaction, so multi-byte reads and writes are unchanged.
 */
static uint8_t ipod_classic_pmu_recv(I2CSlave *i2c)
{
    IPodClassicPMUState *s = IPOD_CLASSIC_PMU(i2c);
    uint8_t val = pmu_reg_read(s, s->addr);

    s->addr++;
    return val;
}

static int ipod_classic_pmu_send(I2CSlave *i2c, uint8_t data)
{
    IPodClassicPMUState *s = IPOD_CLASSIC_PMU(i2c);

    if (s->addr_pending) {
        s->addr = data;
        s->addr_pending = false;
    } else {
        pmu_reg_write(s, s->addr, data);
        s->addr++;
        s->wrote_data = true;
    }
    return 0;
}

static void ipod_classic_pmu_reset(DeviceState *dev)
{
    IPodClassicPMUState *s = IPOD_CLASSIC_PMU(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->addr = 0;
    s->addr_pending = false;
    s->addr_saved = 0;
    s->wrote_data = false;
    pmu_start_adc(s);

    /*
     * THE HOLD SWITCH, and it is not decoration: with this bit at zero every
     * button press on the click wheel is silently thrown away.
     *
     * Two independent sources say what GPIOSTAT bit 1 means, and they agree.
     * Rockbox, firmware/target/arm/s5l8702/ipod6g/pmu-6g.c:244 --
     *
     *     pmu_input_holdswitch = !(pmu_read(PCF50635_REG_GPIOSTAT)
     *                                 & PCF50635_GPIOSTAT_GPIO2);
     *
     * -- so the bit SET means the hold switch is OFF and input is allowed.
     * Apple's retailOS, disassembled: 0x08294f80 reads one byte of register
     * 0x87 over I2C and stores (byte >> 1) & 1 ("mov r0,#0x87" / "bl I2C read"
     * / "and r0,r0,#2" / "lsr r0,r0,#1"); 0x080634a4 turns that into "1 - it"
     * ("rsbs r0,r0,#1" / "movcc r0,#0"); and the keycode dispatcher
     * 0x0803f390 calls that chain at 0x0803f3e0 -- through the tail branch
     * 0x08030d48, which is a bare "b 0x080634a4" -- and returns immediately
     * when the answer is non-zero ("cmp r0,#0" / "popne {r4,r5,r6,pc}" at
     * 0x0803f3e4). Same polarity, arrived at from the other end.
     *
     * (0x0803f3e0 is the 21st instruction of that function, not the second:
     * the entry first remaps the keycode through four compares. Nothing
     * depends on where it sits, but an earlier version of this comment said
     * "one instruction after the entry" and that was simply wrong.)
     *
     * MEASURED, before and after. With the register at 0 the wheel worked and
     * the buttons did not: holding ENTER down for three seconds and reading
     * the firmware's own state block showed 0x089116d8+0x1C = 1 (the packet
     * arrived) and +0x14 = 1 (the edge detector ran and called the dispatcher)
     * -- and the screen never changed, because the dispatcher dropped the key
     * one instruction later. The turn of the wheel does not pass through
     * 0x0803f390 at all, which is exactly why it was not affected and why the
     * failure looked like a wheel bug rather than a hold switch.
     *
     * Reads come from s->hold (see pmu_reg_read), which starts unlocked and
     * moves through the machine's "hold" property. The register keeps its
     * reset value for read-back consistency; the other two bits stay zero --
     * Rockbox marks both "TBC" and nothing we have reads them.
     */
    s->reg[PCF50635_REG_GPIOSTAT] = PCF50635_GPIOSTAT_HOLD_OFF;

    /*
     * Power-status registers seeded to their healthy powered values so a
     * read-back before the first poll is coherent; pmu_reg_read forces them on
     * every read anyway (see the OOCSTAT/MBCS1 cases and the header note). This
     * is what stops Apple's power manager from deciding the battery is empty and
     * software-resetting to the "--------" image a few seconds into the boot.
     */
    s->reg[PCF5063X_REG_OOCSTAT] = PCF5063X_OOCSTAT_HEALTHY;
    s->reg[PCF5063X_REG_MBCS1] = PCF5063X_MBCS1_HEALTHY;
    pmu_update_irq(s);
}

static void ipod_classic_pmu_init(Object *obj)
{
    IPodClassicPMUState *s = IPOD_CLASSIC_PMU(obj);

    /*
     * Not in reset: a power change or hold position set at run time survives
     * a guest reset.
     */
    s->power = pmu_initial_power();
    s->hold = false;
    s->battery_raw = pmu_initial_battery();
    qdev_init_gpio_out_named(DEVICE(obj), &s->irq, "irq", 1);
}

static const VMStateDescription vmstate_ipod_classic_pmu = {
    .name = TYPE_IPOD_CLASSIC_PMU,
    .version_id = 4,
    .minimum_version_id = 4,
    .fields = (VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, IPodClassicPMUState),
        VMSTATE_UINT8_ARRAY(reg, IPodClassicPMUState, S5L8702_PMU_NUM_REGS),
        VMSTATE_UINT8(addr, IPodClassicPMUState),
        VMSTATE_BOOL(addr_pending, IPodClassicPMUState),
        VMSTATE_UINT8(addr_saved, IPodClassicPMUState),
        VMSTATE_BOOL(wrote_data, IPodClassicPMUState),
        VMSTATE_BOOL(hold, IPodClassicPMUState),
        VMSTATE_UINT32(battery_raw, IPodClassicPMUState),
        VMSTATE_END_OF_LIST()
    },
};

static void ipod_classic_pmu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->desc = "NXP PCF50635 power management unit";
    dc->reset = ipod_classic_pmu_reset;
    dc->vmsd = &vmstate_ipod_classic_pmu;
    k->event = ipod_classic_pmu_event;
    k->recv = ipod_classic_pmu_recv;
    k->send = ipod_classic_pmu_send;
}

static const TypeInfo ipod_classic_pmu_info = {
    .name          = TYPE_IPOD_CLASSIC_PMU,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(IPodClassicPMUState),
    .instance_init = ipod_classic_pmu_init,
    .class_init    = ipod_classic_pmu_class_init,
};

static void ipod_classic_pmu_register_types(void)
{
    type_register_static(&ipod_classic_pmu_info);
}

type_init(ipod_classic_pmu_register_types)
