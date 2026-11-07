/*
 * NXP PCF50605 power management unit, at I2C address 0x08.
 *
 * Everything the iPod's power supply, its real time clock, its battery gauge
 * and its accessory detection go through. Apple's retailOS will not finish
 * booting without it: 0x28341c starts an A/D conversion and then spins on the
 * result-ready flag with no timeout at all, so with nothing on the bus the
 * firmware simply stops there for ever.
 *
 * Register numbers are from Rockbox's firmware/export/pcf5060x.h ("PCF50605/6
 * Registers from datasheet rev2.2"), cross-checked against OpenMoko's
 * include/pcf50606.h, which was written from the same datasheet.
 *
 * The bus protocol is a plain register-pointer device: a write starts with the
 * register number and the pointer advances after every byte, a read carries on
 * from wherever the pointer was left. That the pointer survives the end of a
 * transaction is not a guess -- both drivers depend on it. Rockbox's
 * i2c_readbytes() sends the register number in one transaction and then reads
 * in separate ones, and Apple's 0x283540 does the same and splits the read
 * into four-byte chunks, so a seven-byte RTC read is one write and two reads
 * with the pointer running across all three.
 *
 * WHAT IS MODELLED
 *
 *   - The register file, which is what almost every register is: the guest
 *     writes regulator voltages, interrupt masks and GPO settings and never
 *     reads them back for anything that matters.
 *   - The A/D converter: ADCC2 starts a conversion and selects the input,
 *     ADCS1/ADCS2 return a 10-bit result with the ready flag.
 *   - The real time clock, from the host clock through QEMU's own -rtc plumbing.
 *
 * WHAT IS NOT
 *
 *   - The RTC alarm, 0x11..0x17. Those registers store and read back and
 *     nothing ever fires.
 *   - Charging (MBCC1..3, MBCS1) and the ON/OFF controller (OOCC1, including
 *     GOSTDBY, which is how the guest powers the iPod down). A guest that
 *     writes GOSTDBY here is quietly ignored rather than shutting the machine
 *     down.
 *   - Interrupts. INT1..INT3 stay at zero and the chip's interrupt output is
 *     not wired anywhere. Neither firmware uses the ADC's interrupt; both poll.
 *   - The regulators. Writing DCDC1 does not change any clock or voltage here,
 *     because there is nothing for a voltage to mean.
 *
 * ASSUMPTIONS, called out because they are not derived from anything:
 *
 *   - The ID register (0x00) reads 0x00. What a real PCF50605 answers there is
 *     not in Rockbox, in iPodLinux, or in OpenMoko's header, and no dump of the
 *     real chip exists. What the firmware needs is only that it is NOT 0x24:
 *     0x283704 compares it against that and switches to a different register
 *     map if it matches (control at 0x30, status at 0x32 -- some other chip).
 *     Any other value behaves identically, so zero is chosen precisely because
 *     it adds no information we do not have.
 *   - The conversion is instantaneous. The real chip takes some time that no
 *     source we have states; all we know is what the firmware waits, which is
 *     20-35 us before it starts polling. Rockbox does not wait at all
 *     (adc-ipod-pcf.c writes ADCC2 and reads ADCS1/ADCS2 straight after), so
 *     the result has to be there by the time the write returns anyway.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "sysemu/rtc.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/ipod_video/ipod_video_pcf50605.h"

/* The registers with behaviour. Names from pcf5060x.h. */
#define PCF50605_ID         0x00

/*
 * The clock: seven registers in the order seconds, minutes, hours, weekday,
 * day, month, year, all BCD, read and written as one seven-byte block --
 * Rockbox's rtc_pcf50605.c:43 does pcf50605_read_multiple(0x0a, buf, 7) and
 * unpacks exactly that.
 */
#define PCF50605_RTCSC      0x0a
#define PCF50605_RTCMN      0x0b
#define PCF50605_RTCHR      0x0c
#define PCF50605_RTCWD      0x0d
#define PCF50605_RTCDT      0x0e
#define PCF50605_RTCMT      0x0f
#define PCF50605_RTCYR      0x10
#define PCF50605_ADCC1      0x2e
#define PCF50605_ADCC2      0x2f
#define PCF50605_ADCS1      0x30
#define PCF50605_ADCS2      0x31
#define PCF50605_ADCS3      0x32

/*
 * ADCC2, from OpenMoko's pcf50606.h and confirmed by what three independent
 * drivers write into it -- Rockbox's adc-ipod-pcf.c:51, iPodLinux's
 * pcf50605.c and Apple's 0x283484 all build "(channel << 1) | 1".
 */
#define PCF50605_ADCC2_START    0x01
#define PCF50605_ADCC2_MUX      0x1e
#define PCF50605_ADCC2_MUX_SH   1
#define PCF50605_ADCC2_RES8     0x80

/* ADCS2 bit 7 is ADCRDY. This is the bit the retailOS spins on. */
#define PCF50605_ADCS2_ADCRDY   0x80

/* Multiplexer inputs, pcf5060x.h:94-109. */
#define PCF50605_MUX_ADCIN1_RES     0x2
#define PCF50605_MUX_ADCIN1_SUBTR   0x3
#define PCF50605_MUX_BATTEMP        0x4
#define PCF50605_MUX_ADCIN3_RATIO   0x7

#define PCF50605_ADC_FULL_SCALE     0x3ff

/*
 * The battery, as Rockbox measures it: ADCIN1 through the resistive divider.
 *
 * Rockbox turns this straight into millivolts with its own scale factor, and
 * that is the whole derivation -- powermgmt-ipod-pcf.c:103-110:
 *
 *     #define BATTERY_SCALE_FACTOR 6000     -- full-scale (2^10) in millivolts
 *     int _battery_voltage(void)
 *     { return (adc_read(ADC_UNREG_POWER) * BATTERY_SCALE_FACTOR) >> 10; }
 *
 * so 4000 mV, a healthy and nearly full Li-ion by the same file's own
 * percent_to_volt_discharge[] table for the iPod Video (3600 mV at 0%, 4180 at
 * 100%), is 4000 * 1024 / 6000 = 682 counts. It also has to clear
 * battery_level_shutoff, which that table gives as 3300 mV; before this device
 * existed the reading came out at about 1131 mV, well under it.
 */
#define PCF50605_ADC_BATTERY_RES    682

/*
 * The battery, as Apple measures it: ADCIN1 through the subtractor. A
 * different analogue path with a different scale, so the number is different
 * even though it is the same cell.
 *
 * There is no way to convert this to volts and no need to: the retailOS never
 * does either. 0x264ee8 walks a fifteen-entry table of raw counts and returns
 * the index, and 0x9b24c turns that index into index*256/15. The table for
 * this hardware generation (the top half of the hardware id is 0x000b, which
 * is 11) lives at 0x1081df10 and is put there by the scatter-load, so it can
 * only be read out of a running machine:
 *
 *     283 302 311 317 319 325 330 338 350 365 382 395 404 436 454
 *
 * and there is one more hard number, in the image this time: 0x264cb0 refuses
 * to bring up a power rail unless the reading is at least 0x12a = 298, out of
 * the literal at 0x264d20.
 *
 * ASSUMED: 390 is a choice within what the firmware's own table allows, not
 * a measurement. It sits between entries 10 (382) and 11 (395), so it reports
 * level 11 of 15, comfortably above the 298 that gates the boot and comfortably
 * clear of both ends of the table, where a rounding error either way would
 * change the answer. What 390 is in volts is not known and nothing here
 * depends on knowing it.
 */
#define PCF50605_ADC_BATTERY_SUBTR  390

static uint16_t pcf50605_adc_sample(PCF50605State *s, unsigned mux)
{
    switch (mux) {
    case PCF50605_MUX_ADCIN1_RES:
        return PCF50605_ADC_BATTERY_RES;

    case PCF50605_MUX_ADCIN1_SUBTR:
        return PCF50605_ADC_BATTERY_SUBTR;

    case PCF50605_MUX_BATTEMP:
    case PCF50605_MUX_ADCIN3_RATIO:
        /*
         * On this board these are accessory identification lines rather than
         * the battery thermistor the datasheet names them for: 0x266034 reads
         * one of them -- BATTEMP on hardware generation 11, ADCIN3_RATIO
         * otherwise -- takes the top four bits of the eight-bit result as a
         * bucket number, and turns bucket 15 into the code 100. That is the
         * value the task at 0xfec04 starts itself at, and the string table at
         * 0x25d780 says what the task is called: "RsistrAccsryMgr", the
         * resistor accessory manager.
         *
         * An identification line with nothing plugged into it is an open
         * circuit and reads full scale, which is bucket 15: nothing connected,
         * which is the truth about this machine. The two agree, so this is one
         * of the few numbers here that is not a choice.
         */
        return PCF50605_ADC_FULL_SCALE;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: nothing is wired to multiplexer input %u; "
                      "reporting an open circuit\n", __func__, mux);
        return PCF50605_ADC_FULL_SCALE;
    }
}

static void pcf50605_adc_start(PCF50605State *s)
{
    uint8_t adcc2 = s->regs[PCF50605_ADCC2];
    unsigned mux = (adcc2 & PCF50605_ADCC2_MUX) >> PCF50605_ADCC2_MUX_SH;
    uint16_t raw = pcf50605_adc_sample(s, mux);

    /*
     * The result is ten bits split across two registers: the top eight in
     * ADCS1 and the bottom two in ADCS2[1:0]. Every implementation we have
     * reassembles it the same way -- Rockbox's pcf50606.c:102
     * ("*res1 = (adcs1 << 2) | (adcs2 & 3)"), iPodLinux's pcf_adc_read(), and
     * Apple's 0x2834f0 ("orreq r0, r1, r0, lsl #2").
     *
     * In eight-bit mode (ADCC2 bit 7) the firmware takes ADCS1 alone and drops
     * the low two bits, so the same value works for both; there is no separate
     * eight-bit conversion to model.
     *
     * ADCS3 stays at zero: it is the second result of the dual-measurement mux
     * settings (0xc, 0xe, 0xf), none of which anything on this board selects.
     */
    s->regs[PCF50605_ADCS1] = raw >> 2;
    s->regs[PCF50605_ADCS2] = PCF50605_ADCS2_ADCRDY | (raw & 3);
    s->regs[PCF50605_ADCS3] = 0;

    /*
     * ADCSTART clears itself once sampling begins. Neither Rockbox nor the
     * retailOS looks, but iPodLinux does -- "wait for ADCSTART = 0 (wait for
     * sampling to start)" spins on it -- so a chip that left it set would hang
     * that guest.
     */
    s->regs[PCF50605_ADCC2] &= ~PCF50605_ADCC2_START;
}

static uint8_t to_bcd(int v)
{
    return ((v / 10) << 4) | (v % 10);
}

static int from_bcd(uint8_t v)
{
    return ((v >> 4) & 0xf) * 10 + (v & 0xf);
}

/*
 * The year register counts from 2000: Rockbox's rtc_pcf50605.c:53 does
 * "tm->tm_year = buf[6] + 100", and struct tm counts from 1900.
 */
#define PCF50605_RTC_EPOCH_YEAR 100

static uint8_t pcf50605_rtc_read(PCF50605State *s, uint8_t reg)
{
    struct tm tm;

    qemu_get_timedate(&tm, s->rtc_offset);

    switch (reg) {
    case PCF50605_RTCSC: return to_bcd(tm.tm_sec);
    case PCF50605_RTCMN: return to_bcd(tm.tm_min);
    case PCF50605_RTCHR: return to_bcd(tm.tm_hour);
    case PCF50605_RTCWD: return to_bcd(tm.tm_wday);
    case PCF50605_RTCDT: return to_bcd(tm.tm_mday);
    case PCF50605_RTCMT: return to_bcd(tm.tm_mon + 1);
    default:             return to_bcd(tm.tm_year - PCF50605_RTC_EPOCH_YEAR);
    }
}

/*
 * The guest sets the clock by writing all seven registers in one go, so the
 * year -- the last of them -- is what commits the new time. Anything else is
 * a partial update we have no way to interpret, and the alternative to this is
 * worse: without it the clock snaps back to the host's the moment it is read,
 * and a guest that has just set it sees its write vanish.
 */
static void pcf50605_rtc_write(PCF50605State *s, uint8_t reg, uint8_t value)
{
    struct tm tm;

    s->regs[reg] = value;
    if (reg != PCF50605_RTCYR) {
        return;
    }

    qemu_get_timedate(&tm, s->rtc_offset);
    tm.tm_sec  = from_bcd(s->regs[PCF50605_RTCSC]);
    tm.tm_min  = from_bcd(s->regs[PCF50605_RTCMN]);
    tm.tm_hour = from_bcd(s->regs[PCF50605_RTCHR]);
    tm.tm_mday = from_bcd(s->regs[PCF50605_RTCDT]);
    tm.tm_mon  = from_bcd(s->regs[PCF50605_RTCMT]) - 1;
    tm.tm_year = from_bcd(value) + PCF50605_RTC_EPOCH_YEAR;
    s->rtc_offset = qemu_timedate_diff(&tm);
}

static uint8_t pcf50605_read_reg(PCF50605State *s, uint8_t reg)
{
    if (reg >= PCF50605_NUM_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: register 0x%02x does not exist\n",
                      __func__, reg);
        return 0;
    }

    if (reg >= PCF50605_RTCSC && reg <= PCF50605_RTCYR) {
        return pcf50605_rtc_read(s, reg);
    }
    return s->regs[reg];
}

static void pcf50605_write_reg(PCF50605State *s, uint8_t reg, uint8_t value)
{
    if (reg >= PCF50605_NUM_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: register 0x%02x does not exist "
                      "(wrote 0x%02x)\n", __func__, reg, value);
        return;
    }

    if (reg >= PCF50605_RTCSC && reg <= PCF50605_RTCYR) {
        pcf50605_rtc_write(s, reg, value);
        return;
    }

    s->regs[reg] = value;

    if (reg == PCF50605_ADCC2 && (value & PCF50605_ADCC2_START)) {
        pcf50605_adc_start(s);
    }
}

static int pcf50605_event(I2CSlave *i2c, enum i2c_event event)
{
    PCF50605State *s = PCF50605(i2c);

    switch (event) {
    case I2C_START_SEND:
        /* The first byte of a write is the register number. */
        s->expect_ptr = true;
        break;

    case I2C_START_RECV:
        /*
         * A read carries on from wherever the pointer was left by the write
         * that preceded it, possibly several transactions ago.
         */
        break;

    default:
        break;
    }

    return 0;
}

static uint8_t pcf50605_recv(I2CSlave *i2c)
{
    PCF50605State *s = PCF50605(i2c);

    return pcf50605_read_reg(s, s->ptr++);
}

static int pcf50605_send(I2CSlave *i2c, uint8_t data)
{
    PCF50605State *s = PCF50605(i2c);

    if (s->expect_ptr) {
        s->ptr = data;
        s->expect_ptr = false;
        return 0;
    }

    pcf50605_write_reg(s, s->ptr++, data);
    return 0;
}

static void pcf50605_reset(DeviceState *dev)
{
    PCF50605State *s = PCF50605(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->ptr = 0;
    s->expect_ptr = false;
    s->rtc_offset = 0;

    /*
     * Not 0x24; see the note at the top. Written out rather than left implicit
     * so that anyone who ever gets a dump of a real chip knows exactly which
     * byte to replace.
     */
    s->regs[PCF50605_ID] = 0x00;
}

static const VMStateDescription vmstate_pcf50605 = {
    .name = TYPE_PCF50605,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, PCF50605State),
        VMSTATE_UINT8_ARRAY(regs, PCF50605State, PCF50605_NUM_REGS),
        VMSTATE_UINT8(ptr, PCF50605State),
        VMSTATE_BOOL(expect_ptr, PCF50605State),
        VMSTATE_INT64(rtc_offset, PCF50605State),
        VMSTATE_END_OF_LIST()
    }
};

static void pcf50605_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = pcf50605_event;
    k->recv = pcf50605_recv;
    k->send = pcf50605_send;

    dc->reset = pcf50605_reset;
    dc->vmsd = &vmstate_pcf50605;
    dc->desc = "NXP PCF50605 power management unit";
}

static const TypeInfo pcf50605_type_info = {
    .name          = TYPE_PCF50605,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(PCF50605State),
    .class_init    = pcf50605_class_init,
};

static void pcf50605_register_types(void)
{
    type_register_static(&pcf50605_type_info);
}

type_init(pcf50605_register_types)
