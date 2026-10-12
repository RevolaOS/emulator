/*
 * S5L8702 click wheel, 0x3C200000, IRQ 23.
 *
 * The register map, the two packet formats, the constraints the firmware's own
 * scroll decoder puts on any model, and the measurement that says the
 * interrupt path is already armed all live in the header. Read that first;
 * this file is the mechanism.
 *
 * Two decisions here are worth stating in one place because they are the ones
 * that make the difference between "the model answers" and "the guest moves":
 *
 *  - WHEELMASK reads back what was written. The firmware saves it, zeroes it
 *    around a polled transaction, and restores it; while this block read as
 *    zero the restore wrote zero and the interrupt stayed masked for ever.
 *
 *  - Packets are PACED by a timer, one per tick, instead of being queued in a
 *    burst. The firmware needs to see the arming packet of a gesture as its
 *    own interrupt: its scroll decoder compares the new position word against
 *    the previous one and does nothing at all unless both have the
 *    finger-down bit. Queue two packets back to back and the ISR can consume
 *    both before the wheel task runs once, in which case the task sees only
 *    the second, decides it is a first touch, and throws the movement away.
 *
 * WHEELTRACE=2 additionally logs every key event, queued button edge and
 * packet with its virtual time.
 *
 * WHEELTRACE=1 announces the first command, the first packet and the first
 * time the line is raised, and prints totals at exit -- announcing the first
 * of each rather than every one, because stderr is where the firmware's own
 * semihosting output goes and it has to stay readable.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "hw/arm/ipod_classic/ipod_classic_wheel.h"
#include "hw/arm/ipod_classic/ipod_classic_np_overlay.h"

/*
 * INSTRUMENTATION ONLY -- nothing below changes what the model does. Same
 * pattern as DISPTRACE=1, LCDTRACE=1 and ATATRACE=1 elsewhere in this machine:
 * announce the FIRST of each interesting thing, count all of them, and print
 * the totals at exit. Announcing every one would bury the firmware's own
 * semihosting output, which shares this stream.
 */
static int wheeltrace = -1;
static void wheel_trace_totals(void);

static uint64_t wheel_packets;      /* event and reply words queued */
static uint64_t wheel_dropped;      /* words lost because the FIFO was full */
static uint64_t wheel_commands;     /* WHEELGO kicks */
static uint64_t wheel_irq_raises;   /* rising edges on IRQ 23 */
static uint64_t wheel_ack_timeouts; /* packets sent before the guest task was idle */
static uint64_t wheel_overflows;    /* inputs lost because the event queue was full */

static bool wheel_trace_on(void)
{
    if (wheeltrace < 0) {
        const char *e = getenv("WHEELTRACE");
        wheeltrace = (e && *e && *e != '0') ? atoi(e) : 0;
        if (wheeltrace) {
            atexit(wheel_trace_totals);
        }
    }
    return wheeltrace >= 1;
}

static void wheel_trace_totals(void)
{
    fprintf(stderr, "WHEELTRACE totals: %" PRIu64 " packets queued, %" PRIu64
            " dropped with the FIFO full, %" PRIu64 " commands, %" PRIu64
            " rising edges on IRQ 23, %" PRIu64 " sent before the guest task was idle, %"
            PRIu64 " inputs lost with the queue full\n", wheel_packets,
            wheel_dropped, wheel_commands, wheel_irq_raises,
            wheel_ack_timeouts, wheel_overflows);
}

/* ------------------------------------------------------------------ */
/* The receive FIFO and the interrupt line                            */
/* ------------------------------------------------------------------ */

static bool wheel_fifo_empty(IPodClassicWheelState *s)
{
    return s->fifo_len == 0;
}

/*
 * WHEELPEND bit 0 is not a plain latch: it follows the FIFO. INFERRED, and
 * the inference is forced by the polled path at 0x0832777c, which spins on
 * "if (WHEELPEND & 1) { if (WHEELSTAT & 1) take the word; else WHEELPEND = 1; }"
 * -- if clearing the bit could hide a word that is still queued, that loop
 * would drop the answer it had just been given. Bits 1 and 2 are real latches
 * we never set; what they report is UNDETERMINED and nothing reads them apart.
 */
static uint32_t wheel_pend_value(IPodClassicWheelState *s)
{
    return s->pend | (wheel_fifo_empty(s) ? 0 : S5L8702_WHEEL_PEND_RX);
}

static void wheel_update_irq(IPodClassicWheelState *s)
{
    bool level = (wheel_pend_value(s) & s->mask & S5L8702_WHEEL_PEND_ALL) != 0;

    if (level && !s->irq_level) {
        if (wheel_irq_raises == 0 && wheel_trace_on()) {
            fprintf(stderr, "WHEELTRACE irq 23 raised for the first time "
                    "(pend 0x%x, mask 0x%x)\n", wheel_pend_value(s), s->mask);
        }
        wheel_irq_raises++;
    }
    s->irq_level = level;
    qemu_set_irq(s->irq, level);
}

static void wheel_fifo_push(IPodClassicWheelState *s, uint32_t word)
{
    if (s->fifo_len == S5L8702_WHEEL_FIFO_LEN) {
        /*
         * The guest is not draining. Counted rather than silently dropped:
         * if the ISR never runs this counter is the first thing that says so,
         * which beats guessing at a screen that will not move.
         */
        wheel_dropped++;
        return;
    }
    s->fifo[(s->fifo_head + s->fifo_len) % S5L8702_WHEEL_FIFO_LEN] = word;
    s->fifo_len++;
    wheel_packets++;
    if (wheel_packets == 1 && wheel_trace_on()) {
        fprintf(stderr, "WHEELTRACE first packet queued: 0x%08x\n", word);
    }
    wheel_update_irq(s);
}

static uint32_t wheel_fifo_pop(IPodClassicWheelState *s)
{
    uint32_t word;

    if (wheel_fifo_empty(s)) {
        return 0;
    }
    word = s->fifo[s->fifo_head];
    s->fifo_head = (s->fifo_head + 1) % S5L8702_WHEEL_FIFO_LEN;
    s->fifo_len--;
    wheel_update_irq(s);
    return word;
}

/* ------------------------------------------------------------------ */
/* Packets                                                            */
/* ------------------------------------------------------------------ */

static uint32_t wheel_event_packet(IPodClassicWheelState *s)
{
    uint32_t p = S5L8702_WHEEL_EVENT_ID
               | (s->buttons << S5L8702_WHEEL_BTN_SHIFT);

    if (s->touched) {
        p |= S5L8702_WHEEL_DOWN
           | ((s->position & S5L8702_WHEEL_POS_MASK)
              << S5L8702_WHEEL_POS_SHIFT);
    }
    return p;
}

static void wheel_send_event(IPodClassicWheelState *s)
{
    if (wheel_trace_on() && wheeltrace >= 2) {
        fprintf(stderr, "WHEELTRACE %" PRId64 " ms: packet 0x%08x\n",
                qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), wheel_event_packet(s));
    }
    s->last_packet = wheel_event_packet(s);
    wheel_fifo_push(s, s->last_packet);
    s->ack_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->ack_timeout_ns;
}

/*
 * A command word arriving in WHEELTX.
 *
 * The retailOS writes 0x80000000 | (cmd >> 1) -- measured on the bus as
 * 0xC000011D for the buttons command -- while Rockbox writes the command
 * itself, 0x8000023A (button-clickwheel.c:273). WHY the real hardware takes
 * both encodings is UNDETERMINED; all that is certain is what each driver
 * puts on the wire, so both are decoded. Undoing Apple's shift is exact:
 * (0xC000011D << 1) | 0x80000000 == 0x8000023A.
 */
static uint32_t wheel_decode_command(uint32_t tx)
{
    switch (tx & 0xffff) {
    case S5L8702_WHEEL_CMD_BUTTONS & 0xffff:
    case S5L8702_WHEEL_CMD_POSITION & 0xffff:
    case S5L8702_WHEEL_CMD_ACK & 0xffff:
        return tx;
    default:
        return (tx << 1) | 0x80000000u;
    }
}

static void wheel_run_command(IPodClassicWheelState *s)
{
    uint32_t cmd = wheel_decode_command(s->tx);

    wheel_commands++;
    if (wheel_commands == 1 && wheel_trace_on()) {
        fprintf(stderr, "WHEELTRACE first command: tx 0x%08x -> cmd 0x%08x\n",
                s->tx, cmd);
    }

    switch (cmd) {
    case S5L8702_WHEEL_CMD_BUTTONS:
        /*
         * Payload in bits 20..16: the reader at 0x08327850 does lsl #11 /
         * lsr #27, and so does the ISR's second arm at 0x08047a20. Bit order
         * inside those five bits is Rockbox's (SELECT, RIGHT, LEFT, PLAY,
         * MENU from bit 16 up, button-clickwheel.c:273). The polled reader
         * remaps them again into a private namespace of its own and its
         * result is discarded by the only caller (0x08047a64 ignores r0), so
         * that remap is deliberately NOT reproduced here.
         */
        wheel_fifo_push(s, S5L8702_WHEEL_CMD_BUTTONS | (s->buttons << 16));
        break;

    case S5L8702_WHEEL_CMD_POSITION:
        /* 0x083278f0 keeps (rx >> 16) & 0x7FFF, finger-down bit included. */
        wheel_fifo_push(s, S5L8702_WHEEL_CMD_POSITION
                           | ((s->position & S5L8702_WHEEL_POS_MASK) << 16)
                           | (s->touched ? S5L8702_WHEEL_DOWN : 0));
        break;

    case S5L8702_WHEEL_CMD_ACK:
        /*
         * Sent at 0x0832790c after a non-zero position was read, and nobody
         * waits for an answer. What it means is UNDETERMINED, so nothing is
         * queued: a word the firmware never reads would sit in the FIFO
         * pretending to be an event.
         */
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702.wheel: unknown command 0x%08x "
                      "(tx 0x%08x)\n", cmd, s->tx);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Pacing                                                             */
/* ------------------------------------------------------------------ */

static void wheel_kick(IPodClassicWheelState *s)
{
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->tick_ns);
}

/*
 * Has the guest taken the last packet and finished with it? The ISR read it
 * (the FIFO is empty) and the wheel task copied the state block's new words
 * to its old ones. See "INPUT IS AN ORDERED QUEUE" in the header.
 */
static bool wheel_guest_idle(IPodClassicWheelState *s)
{
    uint32_t w[4];      /* +0x18 old buttons, +0x1c old position, +0x20, +0x24 new */

    if (!wheel_fifo_empty(s)) {
        return false;
    }
    if (!s->state_block) {
        return true;
    }
    cpu_physical_memory_read(s->state_block + 0x18, w, sizeof(w));
    memcpy(s->last_block, w, sizeof(w));
    if (s->last_packet) {
        /*
         * The ISR pops WHEELRX BEFORE it stores the decoded words, and a
         * timer can fire in between (the FIFO is empty and old == new, both
         * still the previous packet's). Without this the next packet goes out
         * at once and the ISR takes both before the task runs: the arming
         * packet of a gesture is merged with the first step, or a button's
         * down with its up. Wait until the block holds THIS packet.
         */
        uint32_t p = s->last_packet;
        uint32_t buttons = (p >> S5L8702_WHEEL_BTN_SHIFT) & 0x1f;
        uint32_t position = (p & S5L8702_WHEEL_DOWN)
            ? S5L8702_WHEEL_DOWN
              | ((p >> S5L8702_WHEEL_POS_SHIFT) & S5L8702_WHEEL_POS_MASK)
            : 0;

        if (w[2] != buttons || w[3] != position) {
            return false;
        }
    }
    return w[0] == w[2] && w[1] == w[3];
}

static void wheel_evq_push(IPodClassicWheelState *s, int32_t entry)
{
    unsigned tail = (s->evq_head + s->evq_len) % S5L8702_WHEEL_EVQ_LEN;

    if (s->evq_len && entry < S5L8702_WHEEL_EVQ_EDGE) {
        int32_t *last = &s->evq[(tail + S5L8702_WHEEL_EVQ_LEN - 1)
                                % S5L8702_WHEEL_EVQ_LEN];
        if (*last < S5L8702_WHEEL_EVQ_EDGE && (*last < 0) == (entry < 0)) {
            *last += entry;     /* same direction: one entry, paid in key_step pieces */
            return;
        }
    }
    if (s->evq_len == S5L8702_WHEEL_EVQ_LEN) {
        wheel_overflows++;
        return;
    }
    s->evq[tail] = entry;
    s->evq_len++;
}

static void wheel_evq_pop(IPodClassicWheelState *s)
{
    s->evq_head = (s->evq_head + 1) % S5L8702_WHEEL_EVQ_LEN;
    s->evq_len--;
}

static void wheel_tick(void *opaque)
{
    IPodClassicWheelState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /*
     * One packet at a time, and only to a guest that has finished with the
     * previous one. Past the deadline it is sent anyway: a stuck guest must
     * not freeze input for ever, and the counter says it happened.
     */
    if (!wheel_guest_idle(s)) {
        if (now < s->ack_deadline) {
            wheel_kick(s);
            return;
        }
        wheel_ack_timeouts++;
        if (wheel_trace_on()) {
            fprintf(stderr, "WHEELTRACE %" PRId64 " ms: ack timeout, last packet "
                    "0x%08x, fifo %u, block +18..+24 %08x %08x %08x %08x\n",
                    qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), s->last_packet,
                    s->fifo_len, s->last_block[0], s->last_block[1],
                    s->last_block[2], s->last_block[3]);
        }
    }

    if (s->touched && !s->armed) {
        /*
         * Constraint 2 from the header: arm the gesture at the position the
         * wheel is already at. The firmware's decoder stores this one and
         * computes nothing, which is exactly the point.
         */
        s->armed = true;
        wheel_send_event(s);
    } else if (s->evq_len && s->evq[s->evq_head] >= S5L8702_WHEEL_EVQ_EDGE) {
        int32_t edge = s->evq[s->evq_head];
        uint32_t bit = edge & 0xff;

        if (now >= s->btn_hold_until) {
            if (edge & 0x100) {
                s->buttons |= bit;
            } else {
                s->buttons &= ~bit;
            }
            wheel_evq_pop(s);
            s->btn_hold_until = now + s->btn_dwell_ns;
            wheel_send_event(s);
        }
    } else if (s->evq_len) {
        int32_t *head = &s->evq[s->evq_head];
        int32_t step = *head;

        if (step > (int32_t)s->key_step) {
            step = s->key_step;
        } else if (step < -(int32_t)s->key_step) {
            step = -(int32_t)s->key_step;
        }
        s->position = (s->position + step + S5L8702_WHEEL_POSITIONS)
                      % S5L8702_WHEEL_POSITIONS;
        *head -= step;
        if (*head == 0) {
            wheel_evq_pop(s);
        }
        s->untouch_at = now + s->untouch_ns;
        wheel_send_event(s);
    } else if (s->touched && now >= s->untouch_at) {
        /*
         * The finger comes off. This is not cosmetic: the ISR uses an
         * untouched packet to put the sensitivity threshold back to 6
         * (0x080479e8), which is what makes the next gesture behave like the
         * first one.
         */
        s->touched = false;
        s->armed = false;
        wheel_send_event(s);
    }

    if (s->touched || s->evq_len) {
        wheel_kick(s);
    }
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                           */
/* ------------------------------------------------------------------ */

/*
 * The same keys as the iPod-Video machine (hw/arm/ipod_video/ipod_video.c),
 * so that one habit drives both:
 *
 *     q / e   turn the wheel anticlockwise / clockwise
 *     enter   SELECT, the centre button
 *     w       MENU
 *     a / d   previous / next  (LEFT / RIGHT, the |<< and >>| buttons)
 *     s       PLAY / PAUSE
 *
 * Raw PC scancodes, which is what qemu_add_kbd_event_handler delivers; bit 7
 * set means the key came back up.
 */
#define KEY_Q       0x10
#define KEY_W       0x11
#define KEY_E       0x12
#define KEY_A       0x1e
#define KEY_S       0x1f
#define KEY_D       0x20
#define KEY_ENTER   0x1c

/* Owe the guest `steps` wheel positions (signed: + is clockwise). */
static void wheel_turn(IPodClassicWheelState *s, int32_t steps)
{
    wheel_evq_push(s, steps);
    s->touched = true;
    s->untouch_at = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->untouch_ns;
    wheel_kick(s);
}

/*
 * A button edge, queued behind everything that arrived before it. An edge
 * that would not change the button's state once the queue has drained (a
 * repeated key-down from the host) is dropped.
 */
static void wheel_button(IPodClassicWheelState *s, uint32_t bit, bool pressed)
{
    bool state = s->buttons & bit;

    for (unsigned i = 0; i < s->evq_len; i++) {
        int32_t e = s->evq[(s->evq_head + i) % S5L8702_WHEEL_EVQ_LEN];

        if (e >= S5L8702_WHEEL_EVQ_EDGE && (e & 0xff) == bit) {
            state = e & 0x100;
        }
    }
    if (wheel_trace_on() && wheeltrace >= 2) {
        fprintf(stderr, "WHEELTRACE %" PRId64 " ms: button 0x%x %s %s (queue %u)\n",
                qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), bit,
                pressed ? "down" : "up", state != pressed ? "queued" : "ignored",
                s->evq_len);
    }
    if (state != pressed) {
        wheel_evq_push(s, S5L8702_WHEEL_EVQ_EDGE | (pressed ? 0x100 : 0) | bit);
        wheel_kick(s);
    }
}

/*
 * The mouse, for the SDL window: the scroll wheel turns the click wheel
 * (scrolling down is clockwise, which moves a menu highlight down), the left
 * button is SELECT and the right button is MENU. Registered as an absolute pointer so SDL forwards
 * events without grabbing the host cursor; the position is ignored.
 */
static void wheel_mouse_event(void *opaque, int dx, int dy, int dz,
                              int buttons_state)
{
    IPodClassicWheelState *s = opaque;
    static const struct { int mouse; uint32_t wheel; } map[] = {
        { MOUSE_EVENT_LBUTTON, S5L8702_WHEEL_BTN_SELECT },
        { MOUSE_EVENT_RBUTTON, S5L8702_WHEEL_BTN_MENU },
    };

    if (dz) {
        wheel_turn(s, dz * (int32_t)s->key_step);
    }
    for (int i = 0; i < ARRAY_SIZE(map); i++) {
        bool down = buttons_state & map[i].mouse;
        if (down != !!(s->buttons & map[i].wheel)) {
            wheel_button(s, map[i].wheel, down);
        }
    }
}

static void wheel_key_event(void *opaque, int keycode)
{
    IPodClassicWheelState *s = opaque;
    bool released = (keycode & 0x80) != 0;
    uint32_t bit;

    if (wheel_trace_on() && wheeltrace >= 2) {
        fprintf(stderr, "WHEELTRACE %" PRId64 " ms: key 0x%02x\n",
                qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), keycode);
    }

    /*
     * Tee every keycode to the HLE Now Playing overlay's control handler first
     * (play/pause, prev/next, seek, show/hide). No-op unless HLE_NP_OVERLAY is
     * set; the key still flows to the guest below, so navtest/Rockbox and the
     * default build are unaffected.
     */
    ipod_classic_np_overlay_key(keycode);

    switch (keycode & 0x7f) {
    case KEY_ENTER: bit = S5L8702_WHEEL_BTN_SELECT; break;
    case KEY_W:     bit = S5L8702_WHEEL_BTN_MENU;   break;
    case KEY_A:     bit = S5L8702_WHEEL_BTN_LEFT;   break;
    case KEY_D:     bit = S5L8702_WHEEL_BTN_RIGHT;  break;
    case KEY_S:     bit = S5L8702_WHEEL_BTN_PLAY;   break;

    case KEY_Q:
    case KEY_E:
        /*
         * One keystroke is one turn of key_step positions, owed to the guest
         * and paid out by the tick. The release is ignored: a wheel has no
         * "up", and stepping on both edges would double every movement.
         */
        if (!released) {
            wheel_turn(s, ((keycode & 0x7f) == KEY_E)
                          ? (int32_t)s->key_step : -(int32_t)s->key_step);
        }
        return;

    default:
        return;
    }

    wheel_button(s, bit, !released);
}

/* ------------------------------------------------------------------ */
/* MMIO                                                               */
/* ------------------------------------------------------------------ */

static uint64_t ipod_classic_wheel_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    IPodClassicWheelState *s = opaque;

    switch (offset) {
    case S5L8702_WHEEL_CON:
        return s->con;
    case S5L8702_WHEEL_GO:
        /*
         * The start bit is self-clearing: 0x08327980 does an
         * orr-with-1 read-modify-write, so leaving it set would make every
         * later kick a no-op that reads as already running.
         */
        return s->go;
    case S5L8702_WHEEL_DIV:
        return s->div;
    case S5L8702_WHEEL_STAT:
        /*
         * Bit 3, transmit busy, is deliberately never set: the transfer is
         * instantaneous here, and 0x08327994 returns as soon as it reads the
         * bit clear.
         */
        return wheel_fifo_empty(s) ? 0 : S5L8702_WHEEL_STAT_RX;
    case S5L8702_WHEEL_MASK:
        return s->mask;
    case S5L8702_WHEEL_PEND:
        return wheel_pend_value(s);
    case S5L8702_WHEEL_RX:
        return wheel_fifo_pop(s);
    case S5L8702_WHEEL_TX:
        return s->tx;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702.wheel: read of +0x%" HWADDR_PRIx
                      "\n", offset);
        return 0;
    }
}

static void ipod_classic_wheel_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    IPodClassicWheelState *s = opaque;

    switch (offset) {
    case S5L8702_WHEEL_CON:
        /*
         * Stored and handed back, nothing else. What its bits mean is
         * UNDETERMINED and the firmware toggles bit 21 around every single
         * transmit; a model that treated any of them as an enable could go
         * silent for a reason no trace would show.
         */
        s->con = value;
        break;

    case S5L8702_WHEEL_GO:
        s->go = value & ~S5L8702_WHEEL_GO_START;
        if (value & S5L8702_WHEEL_GO_START) {
            wheel_run_command(s);
        }
        break;

    case S5L8702_WHEEL_DIV:
        s->div = value;
        break;

    case S5L8702_WHEEL_MASK:
        /* Honest readback. See "THE TRAP" in the header. */
        s->mask = value;
        wheel_update_irq(s);
        break;

    case S5L8702_WHEEL_PEND:
        /* Write one to clear; bit 0 comes back if a word is still queued. */
        s->pend &= ~(uint32_t)value;
        wheel_update_irq(s);
        break;

    case S5L8702_WHEEL_TX:
        s->tx = value;
        break;

    case S5L8702_WHEEL_STAT:
    case S5L8702_WHEEL_RX:
        qemu_log_mask(LOG_UNIMP, "s5l8702.wheel: write of 0x%" PRIx64
                      " to read-only +0x%" HWADDR_PRIx "\n", value, offset);
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "s5l8702.wheel: write of 0x%" PRIx64
                      " to +0x%" HWADDR_PRIx "\n", value, offset);
        break;
    }
}

static const MemoryRegionOps ipod_classic_wheel_ops = {
    .read = ipod_classic_wheel_read,
    .write = ipod_classic_wheel_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ------------------------------------------------------------------ */
/* Device                                                             */
/* ------------------------------------------------------------------ */

static void ipod_classic_wheel_reset(DeviceState *dev)
{
    IPodClassicWheelState *s = IPOD_CLASSIC_WHEEL(dev);

    s->con = 0;
    s->go = 0;
    s->div = 0;
    s->tx = 0;
    s->mask = 0;
    s->pend = 0;
    s->fifo_head = 0;
    s->fifo_len = 0;
    s->position = 0;
    s->buttons = 0;
    s->touched = false;
    s->armed = false;
    s->untouch_at = 0;
    s->evq_head = 0;
    s->evq_len = 0;
    s->btn_hold_until = 0;
    s->ack_deadline = 0;
    s->last_packet = 0;
    s->irq_level = false;
    timer_del(s->tick);
    qemu_set_irq(s->irq, 0);
}

/* An integer from the environment, clamped. Tuning knobs, not hardware. */
static int wheel_env_int(const char *name, int def, int lo, int hi)
{
    const char *v = getenv(name);
    int n;

    if (!v || !*v) {
        return def;
    }
    n = (int)strtol(v, NULL, 0);
    if (n < lo) {
        n = lo;
    }
    if (n > hi) {
        n = hi;
    }
    return n;
}

static void ipod_classic_wheel_realize(DeviceState *dev, Error **errp)
{
    IPodClassicWheelState *s = IPOD_CLASSIC_WHEEL(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &ipod_classic_wheel_ops, s,
                          TYPE_IPOD_CLASSIC_WHEEL, S5L8702_WHEEL_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    /*
     * The firmware delivers the FIRST movement of a gesture only if it is
     * between 6 and 35 positions (threshold at 0x089116d8+0x04, window at
     * 0x0807a6dc), and 6 is the default for that reason. The clamp here is
     * 1..35, not 6..35, deliberately: the upper end is the firmware's hard
     * window and going past it delivers nothing, while values below 6 are
     * legal to ask for -- they are how you exercise Apple's acceleration path,
     * where four sub-threshold clicks drop the threshold to 0. Anything under
     * 6 means the first few keystrokes of a gesture do nothing, which is why
     * it is not the default.
     */
    s->key_step = wheel_env_int("IPOD_WHEEL_STEP",
                                S5L8702_WHEEL_KEY_STEP, 1, 35);
    s->tick_ns = 1000000LL * wheel_env_int("IPOD_WHEEL_TICK_MS",
                                           S5L8702_WHEEL_TICK_MS, 1, 1000);
    s->untouch_ns = 1000000LL * wheel_env_int("IPOD_WHEEL_UNTOUCH_MS",
                                              S5L8702_WHEEL_UNTOUCH_MS,
                                              10, 60000);
    s->btn_dwell_ns = 1000000LL * wheel_env_int("IPOD_WHEEL_BTN_DWELL_MS",
                                                S5L8702_WHEEL_BTN_DWELL_MS,
                                                0, 5000);
    s->ack_timeout_ns = 1000000LL * wheel_env_int("IPOD_WHEEL_ACK_TIMEOUT_MS",
                                                  S5L8702_WHEEL_ACK_TIMEOUT_MS,
                                                  0, 60000);
    /* 0 turns the acknowledgement off (the firmware is not the retailOS). */
    s->state_block = wheel_env_int("IPOD_WHEEL_STATE_BLOCK",
                                   S5L8702_WHEEL_STATE_BLOCK, 0, INT_MAX);

    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, wheel_tick, s);
    qemu_add_kbd_event_handler(wheel_key_event, s);
    qemu_add_mouse_event_handler(wheel_mouse_event, s, 1, "iPod click wheel");
}

/*
 * irq_level is NOT in the field list below, on purpose: it is not state, it is
 * a cache of what we last put on the wire. Recomputing it is both shorter and
 * safer than migrating it, and it has to be recomputed anyway -- a device that
 * comes back with words in the FIFO comes back with a qemu_irq that is low,
 * and without this the guest would never be told and input would be dead for
 * the rest of the run.
 */
static int wheel_post_load(void *opaque, int version_id)
{
    IPodClassicWheelState *s = opaque;

    s->irq_level = false;
    wheel_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_ipod_classic_wheel = {
    .name = TYPE_IPOD_CLASSIC_WHEEL,
    /*
     * Bumped to 3 when the ordered event queue replaced pending_steps and the
     * button repeat window. Nothing migrates this machine today, so the old
     * format is not kept alive.
     */
    .version_id = 3,
    .minimum_version_id = 3,
    .post_load = wheel_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(con, IPodClassicWheelState),
        VMSTATE_UINT32(go, IPodClassicWheelState),
        VMSTATE_UINT32(div, IPodClassicWheelState),
        VMSTATE_UINT32(tx, IPodClassicWheelState),
        VMSTATE_UINT32(mask, IPodClassicWheelState),
        VMSTATE_UINT32(pend, IPodClassicWheelState),
        VMSTATE_UINT32_ARRAY(fifo, IPodClassicWheelState,
                             S5L8702_WHEEL_FIFO_LEN),
        VMSTATE_UINT32(fifo_head, IPodClassicWheelState),
        VMSTATE_UINT32(fifo_len, IPodClassicWheelState),
        VMSTATE_UINT32(position, IPodClassicWheelState),
        VMSTATE_UINT32(buttons, IPodClassicWheelState),
        VMSTATE_BOOL(touched, IPodClassicWheelState),
        VMSTATE_BOOL(armed, IPodClassicWheelState),
        VMSTATE_INT64(untouch_at, IPodClassicWheelState),
        VMSTATE_INT32_ARRAY(evq, IPodClassicWheelState,
                            S5L8702_WHEEL_EVQ_LEN),
        VMSTATE_UINT32(evq_head, IPodClassicWheelState),
        VMSTATE_UINT32(evq_len, IPodClassicWheelState),
        VMSTATE_INT64(btn_hold_until, IPodClassicWheelState),
        VMSTATE_INT64(ack_deadline, IPodClassicWheelState),
        VMSTATE_UINT32(last_packet, IPodClassicWheelState),
        VMSTATE_TIMER_PTR(tick, IPodClassicWheelState),
        VMSTATE_END_OF_LIST()
    },
};

static void ipod_classic_wheel_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S5L8702 click wheel";
    dc->realize = ipod_classic_wheel_realize;
    dc->reset = ipod_classic_wheel_reset;
    dc->vmsd = &vmstate_ipod_classic_wheel;
    dc->user_creatable = false;
}

static const TypeInfo ipod_classic_wheel_info = {
    .name          = TYPE_IPOD_CLASSIC_WHEEL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodClassicWheelState),
    .class_init    = ipod_classic_wheel_class_init,
};

static void ipod_classic_wheel_register_types(void)
{
    type_register_static(&ipod_classic_wheel_info);
}

type_init(ipod_classic_wheel_register_types)
