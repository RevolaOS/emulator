# PortalPlayer PP5020 / PP5022 — hardware

Primary source: Rockbox's `firmware/export/pp5020.h` (476 defines) plus
`firmware/target/arm/pp/` and `bootloader/ipod.c`. Local copy in
`assets/rockbox/`.

The chip in the iPod we have is a **PP5022C** (read from the silicon by Rockbox).
The PP5020 and the PP5022 share the same register map; Rockbox uses a
single header for both.

## What makes it different from the S5L8702

**Two ARM7TDMI cores.** They are called CPU and COP (coprocessor), run the same
code from the same RAM, and tell each other apart by reading an identity register:

| Macro | Value |
|---|---|
| `PROC_ID_CPU` | `0x55` |
| `PROC_ID_COP` | `0xaa` |

They are controlled with `CPU_CTL` (`0x60007000`) and `COP_CTL` (`0x60007004`). At
boot, one of the two goes to sleep and the other carries on. This is the main
risk of this port: the S5L8702 was single-core.

## Memory map

Everything comes from `pp5020.h` and `ata-target.h` unless stated otherwise. The
"model" column says what exists in `hw/arm/ipod_video/`.

| Base | Block | Model |
|---|---|---|
| `0x00000000` | NOR flash while booting; afterwards DRAM (remapped by MMAP0) | blank window with the HW revision at `+0x405c` |
| `0x04000000` | the low window **again, with bit 26 set** — twins of the NOR and of the remapped DRAM | alias, see below |
| `0x10000000` | **`DRAM_START`**, 64 MB (disputed figure, see below). Matches the `addr` of the firmware directory | RAM |
| `0x14000000` | DRAM with **bit 26** set | permanent alias |
| `0x20000000` | the same NOR, already remapped (`hwcompat.h` reads the revision at `0x2000405c`) | alias |
| `0x30000000` | **Broadcom BCM2722**, the one that drives the panel. Eight registers spaced 64 KB apart, two independent ports | `ipod_video_bcm.c` |
| `0x40000000` | IRAM | RAM |
| `0x60000000` | `PROCESSOR_ID` | yes |
| `0x60001000` | CPU-COP mailbox (`MBX_MSG_STAT/SET/CLR` at `+0`/`+4`/`+8`) | **no**, falls on the `-d unimp` floor |
| `0x60004000` | Interrupt controller. Low bank at `+0x000`, **high bank at `+0x100`** | `ipod_video_intc.c` |
| `0x60005000` | Timers. `TIMER1_CFG` `+0x00`, **`USEC_TIMER` `+0x10`** | yes |
| `0x60006000` | Clock and power. `DEV_RS` `+0x04`, `DEV_EN` `+0x0c`, `CLOCK_SOURCE` `+0x20`, `PLL_CONTROL` `+0x34`, **`PLL_STATUS` `+0x3c`** | yes |
| `0x60007000` | Core control. `CPU_CTL` `+0x00`, `COP_CTL` `+0x04` | yes: **both cores really sleep and wake** |
| `0x60008000` | **DMA, block A** — master here, channels at `0x60009000`. No public source names it; only Apple's firmware, which uses it to upload the Broadcom firmware | `ipod_video_dma.c` |
| `0x6000a000` | **DMA, block B** — master here, channels at `0x6000b000`. This is the one documented by `pp5020.h` and the one Rockbox uses for audio | `ipod_video_dma.c` |
| `0x6000c000` | Cache. `CACHE_CTL` with `VECT_REMAP` (`0x0010`) and `READY` (`0x4000`) | yes |
| `0x6000d000` | GPIO. Per group: `ENABLE` `+0x00`, `OUTPUT_EN` `+0x10`, `OUTPUT_VAL` `+0x20`, `INPUT_VAL` `+0x30` | yes |
| `0x70000000` | **Device control**. `PP_VER1` `+0x00`, `DEV_INIT1` `+0x10`, `DEV_INIT2` `+0x20`, **`+0x28` unnamed in `pp5020.h`**, `DEV_TIMING1` `+0x34`, `GPO32_VAL` `+0x80`, `GPO32_ENABLE` `+0x84` | yes |
| `0x70002800` | I2S | no |
| `0x70003000` | LCD | no, the panel goes through the Broadcom |
| `0x70006000` | **Serial UART 0**, 16550 with a stride of 4 | QEMU's `hw/char/serial.c`, **with the IRQ unconnected** |
| `0x7000c000` | I2C **and** click wheel share a window: I2C at `+0x00`, wheel at `+0x100`, `+0x104` and `+0x140` | `ipod_video_i2c.c` + `ipod_video_pcf50605.c`, and the wheel in `ipod_video.c` |
| `0xc3000000` | **IDE**. Timings `+0x00`, `IDE0_CFG` `+0x28`, task file `+0x1e0`, control `+0x3f8`, DMA `+0x400` | `ipod_video_ide.c` |
| `0xc5000000` | USB | no, `create_unimplemented_device` |
| `0x90000000` / `0x94000000` | DRAM with **bit 31** set (the uncached view), and its combination with bit 26 | alias, see below |
| `0xf0000000` | **Cache arrays**: data `+0`, mirror `+0x2000`, CPU state `+0x4000`, COP state `+0x6000`, flush `+0x8000`, invalidate `+0xc000` | RAM |
| `0xf000f000` | Remap unit `MMAP0..7` (logical/physical pairs) plus `CACHE_MASK` `+0x40` | yes, except the `mask` field, which **is not applied** |

> The UART is a real 16550, so QEMU already had it. It was the first free
> piece of the port.

### The aliases, in a single view

Five regions that are the same memory seen from another address. Bit 26 and
bit 31 are explained at the end of this document; this is just where they are.

| Address | Is | Why it exists |
|---|---|---|
| `0x04000000` | the low window (NOR or remapped DRAM) with bit 26 | the retailOS forms addresses with that bit in its scatter-load |
| `0x14000000` | DRAM with bit 26 | same |
| `0x90000000` | DRAM with bit 31 | the **uncached** view; the retailOS marks all its buffers this way except those in IRAM |
| `0x94000000` | DRAM with both bits | consequence of the previous two |
| `0x20000000` | the remapped NOR | `hwcompat.h` reads it there |

The window at address 0 does **not** have a bit-31 twin, and that is a reasoned
decision: every tagged pointer seen so far has the form `0x9xxxxxxx`
(`ipod_video.c:1039-1050`).

> **The DRAM size is an open question.** The model uses **64 MB**
> because that is what Rockbox reads from the silicon of the real device, but **two sources
> say 32**: the internal default of Apple's own firmware (`fp = 0x700`) and
> iPodLinux's table for `hw_ver 0xb`. Both figures pass the retailOS
> assert and the machine is self-consistent with either of them. The
> reasoning is in `ipod_video.h:31-43`. It is not resolved.

### IRQ numbers

Low bank (0..31) and high bank (32..63); the high one is chained via `HI_IRQ`.

| IRQ | Source |
|---|---|
| 0, 1 | `TIMER1_IRQ`, `TIMER2_IRQ` |
| 4 | `MAILBOX_IRQ` |
| 10 | `IIS_IRQ` |
| 20 | `USB_IRQ` |
| **23** | `IDE_IRQ`, the disk |
| 25, 26 | `FIREWIRE_IRQ`, `DMA_IRQ` — 26 is the one for DMA **block B** |
| **27** | DMA, **block A**. Not in `pp5020.h`; comes from the retailOS table (see below) |
| 30 | `HI_IRQ`, chains the high bank — **it must be driven**, see below |
| 32..34 | `GPIO0/1/2_IRQ` |
| 36, 37 | `SER0_IRQ`, `SER1_IRQ` |
| **40** | `I2C_IRQ`, **and also the click wheel** |

> **Careful**: this table is the hardware, not the model. The **only** lines
> the model raises are **0, 1, 23, 26, 27, 30 (synthetic) and 40**. The
> rest are mapped in the controller but nobody drives them: the UART's is
> passed as `NULL` to `serial_mm_init`, and the GPIO, the mailbox, the I2S and the USB do not
> generate interrupts. `PP_SER0_IRQ` is defined and not used in any
> `.c`.

#### `HI_IRQ` is not a source, it is a summary

Line 30 of the low bank is set by the **high bank** when it has something. Rockbox
names it in `pp5020.h` alongside the real ones and then **never reads it**: its
handler goes straight to `CPU_HI_INT_STAT` (`system-pp502x.c:76` onwards). With
only Rockbox to go by, a model that leaves line 30 undriven looks
correct.

The retailOS does the opposite. Its dispatcher `0x277128` reads both status
words and **refuses to look at the high bank** if the low one does not have bit 30:

```
277134:  ldr r4, [0x60004000]     ; low bank
277138:  ldr r5, [0x60004100]     ; high bank
2771a4:  tst r4, #0x40000000      ; anything up there?
2771a8:  beq  <espuria>
2771dc:  tst r5, #0x100           ; the I2C line, i.e. the click wheel
2771e4:  bl 0x281350              ; the click wheel handler
```

In other words, **every press was taken and then thrown away by the guest itself**. It is
exactly the kind of failure that only the second firmware reveals.

## Registers with behavior

Most registers of this SoC can be stored and returned as
is. The ones that cannot are exactly the ones that block boot, and they all fail
the same way: the emulator looks alive, no exception fires, and the firmware is
waiting for a bit that never arrives. The story of how each one was found
is in [04-roadmap.md](04-roadmap.md).

| Register | Bit | What it means | Who waits for it |
|---|---|---|---|
| `PLL_STATUS` `0x6000603c` | 31 | PLL locked | `system-pp502x.c`: `while (!(PLL_STATUS & 0x80000000));` |
| `0x70000028` | 7 | the USB block came out of reset | `usb-fw-pp502x.c`: `while ((inl(0x70000028) & 0x80) == 0);` |
| `GPO32_VAL` `0x70000080` | 14 | the Broadcom is powered | `lcd-video.c`. If it is 0 it tries to upload the `vmcs` firmware from the NOR, gives up, leaves `display_on = false`, and **`lcd_update_rect()` silently discards all drawing** |
| `IDE0_CFG` `0xc3000028` | 3 | `INTRQ`, transfer finished | `ata-pp5020.c`. It is a **latch**, not a mirror of the line, and is cleared by writing a one |
| `0x7000c104` | 26 | there is a wheel packet to read | `button-clickwheel.c`. Without it `ipod_4g_button_read()` does not even look at the data |
| `CACHE_CTL` `0x6000c000` | 14 | `READY` | always returned set: there is no cache to wait for |
| `I2C_STATUS` `0x7000c01c` | 6 and 2:0 | `BUSY` and the result code | Apple's driver (`0x282f44`) returns `STATUS & 7` as the error and aborts if it is not zero |
| PCF50605 `ADCS2` (reg `0x31`) | 7 | `ADCRDY`, conversion finished | `pmu_adc_read` (`0x28341c`) spins on it **with no timeout** |
| `BCM_ALT_CONTROL` `0x30070000` | 7 and 6 | busy / ready, the Broadcom's boot | Rockbox's `bcm_init()` and Apple's `0x2876d0`/`0x2876dc` |

And two areas that are not registers but addressable memory, and that must be
backed with RAM or the first access is rejected:

- **`0xf0000000`-`0xf000efff`**, the cache arrays.
  `cache_invalidate_special()` walks the whole state array by hand, marking
  each line as valid, as a workaround for a silicon bug.
- **All of `0x60000000` and `0x70000000`**. Apple's firmware touches registers
  that do not appear in `pp5020.h` (the first was `0x60009000`). An unknown
  register that returns zero is survivable; an unmapped one is a data
  abort, and with the vector table not yet installed the CPU does not come back. That is why
  there is a `create_unimplemented_device` covering both windows underneath.

## The IDE and its DMA

The task file is standard ATA with registers spaced four bytes apart, so
QEMU's IDE core handles it. What is specific to the PP is what lies beneath:

| Register | Address | Function |
|---|---|---|
| `IDE0_PRI_TIMING0/1` | `0xc3000000` and `0xc3000004` | PIO and DMA timings |
| `IDE0_CFG` | `0xc3000028` | bit 3 `INTRQ`, bit 15 active during DMA |
| `IDE_DMA_CONTROL` | `0xc3000400` | bit 0 starts, bit 1 arms, bit 3 direction (1 = to memory) |
| `IDE_DMA_LENGTH` | `0xc3000408` | bytes **minus 4** |
| `IDE_DMA_ADDR` | `0xc300040c` | destination physical address |

It is not PCI bus-master: it is a flat buffer. The address and length are
programmed **before** the ATA command and the start bit **after**, so
`start_dma` only stores the core's callback, and the write of bit 0 is what
triggers the transfer.

The Rockbox bootloader reads with `READ MULTIPLE` (PIO) and the application with
`READ DMA`, so without this engine the bootloader works and the app does not.

## The I²C and the PCF50605

The block is at `0x7000c000`, below the click wheel. **It is not a
bit-banger**: you give it a slave address, up to four data bytes and
a length, and a start bit runs the whole transfer. Both
firmwares drive it with the same five steps in the same order — Rockbox's `i2c-pp.c`
and Apple's driver at `0x282d90` (read) and `0x282f58`
(write).

| Register | Offset | Width | Bits |
|---|---|---|---|
| `CTRL` | `+0x00` | byte | bit 7 `SEND` (starts, and **self-clears**), bit 5 = read, bits 2:1 = length−1 |
| `ADDR` | `+0x04` | byte | first byte on the bus: `(dir7 << 1) \| R/W` |
| `ADDR2` | `+0x08` | byte | **Apple only**: address of the read phase of a combined transaction |
| `DATA0..3` | `+0x0c`…`+0x18` | byte | |
| `STATUS` | `+0x1c` | byte | bit 6 `BUSY`; **bits 2:0 = result code** |

Three things that only Apple's firmware reveals, and all three matter:

- **`STATUS[2:0]` is an error code.** `0x282f44` does `and r0, r0, #7` and
  `0x28358c` aborts the transaction if it is not zero. Rockbox does not even look at it. The
  flat register file that existed before satisfied it by accident, which is
  why nobody noticed there was no controller.
- **`+0x08` is a second address**, for the combined write-plus-read
  of `0x282e30` (`CTRL = 0x48 | SEND`). The received byte appears in **`DATA1`,
  not in `DATA0`**: the data registers are consumed in order, the transmitted ones
  first.
- **The block must work without being initialized.** Apple's
  `DEV_EN`/`DEV_RS`/`0x600060a4` routine (`0x282e98`) is only reached from
  `0x282f0c`, which calls it when a transfer times out. If our `STATUS`
  never says `BUSY`, that path is never executed.

**Neither of them uses the interrupt**; both poll. And on this SoC the
line the header calls `I2C_IRQ` is the click wheel's, so the model
does not raise it.

**Undetermined**: what bits 6 and 3 of `CTRL` mean separately (they are only
seen together, as `0x48`), what codes 1..6 of `STATUS[2:0]` are, and which
chip lives at address `0x2a`, which Apple talks to (`0x0e4164`,
`0x0e9410`) and which no public source names. The audio codec is at
`0x1a` according to Rockbox and **there is not a single access to that address in Apple's
image**.

### The slave: PCF50605 at 7-bit address `0x08`

It is a register-pointer device: a write starts with the register
number and the pointer advances after each byte; a read continues from where it
left off. **That the pointer survives the end of a transaction is not an
assumption**: both drivers depend on it. A read of the seven
RTC registers is one write and two reads, with the pointer running across
all three.

Modeled: the register file, the A/D converter, and the clock (from the host
clock, via QEMU's `-rtc` option). Not modeled: charging, the
power-on controller (a guest that writes `GOSTDBY` is ignored instead of
shutting the machine down), the regulators, the RTC alarm, and interrupts.

The ADC: `ADCC2` (`0x2f`) bit 0 starts the conversion and self-clears, bits 4:1
select the input, bit 7 requests 8 bits. The result is ten bits split up:
the high eight in `ADCS1` (`0x30`) and the low two in `ADCS2[1:0]` (`0x31`),
with **`ADCS2` bit 7 = `ADCRDY`** — the bit the retailOS spins on with no
timeout. The values each input returns and their justification are in
[04-roadmap.md](04-roadmap.md).

## The DMA

**There are two blocks, not one**, identical register for register:

| Block | Master | Channels | Channels Apple instantiates | IRQ | Source |
|---|---|---|---|---|---|
| A | `0x60008000` | `0x60009000` + n·`0x20` | 2 | **27** | Apple's firmware only (`0x1da198`) |
| B | `0x6000a000` | `0x6000b000` + n·`0x20` | 4 | **26** | `pp5020.h:621-633` |

`grep -rn "60008000\|60009000" assets/rockbox/` gives zero results: block A
does not appear in any public source. The relationship between the two pages is given
by Apple's channel constructor, `0x1da2b4`: *master base + 0x1000 +
channel·0x20*, which is the same as between `DMA_MASTER_CONTROL` and
`DMA0_BASE_ADDR` in the Rockbox header.

The boot shutdown routine sweeps 4 channels of block A and 8 of B, more than
the driver instantiates. It is defensive and does not prove that that many exist, but
the model backs 8 in each so they don't complain.

### The interrupt numbers

`0x1d98d0` registers both handlers under indices **27** and **28** of the
firmware's own interrupt table, not under hardware lines. The
table is built at `0x1fcaa0`, entry *n* at `objeto+0x208+4n`:

```
0x1fcc38   entry 26 = -1 (does not exist), entry 27 = 26
0x1fcc44   entry 28 = 27
```

Index 27 → handler `0x1d9c40` (block B) → line **26**, which is exactly
the `DMA_IRQ` that Rockbox documents for that same block. Index 28 →
`0x1d9be0` (block A) → line **27**. The running machine confirms it:
`CPU_INT_EN_STAT` reads `0xcc002017`, with 26 and 27 set and 28 not.

### Master registers

| Offset | Name | What it does |
|---|---|---|
| `+0x00` | `DMA_MASTER_CONTROL` | bit 31 enables the block. Both firmwares set it before using it |
| `+0x04` | `DMA_MASTER_STATUS` | bit 24+n = channel n active. **Apple never reads it** |
| `+0x08` | `DMA_REQ_STATUS` | `1 << DMA_REQ_xxx`. Only Rockbox touches it |

### Channel registers

| Offset | Name | Function |
|---|---|---|
| `+0x00` | `CMD` | control and count |
| `+0x04` | `STATUS` | remaining count, `INTR` (30), `BUSY` (31). **Reading it clears the interrupt**. *Only `INTR` comes out of the model*: it never writes the remaining count or `BUSY`, so `MASTER_STATUS` always reads 0 |
| `+0x10` | `RAM_ADDR` | memory address; this is the one that advances |
| `+0x14` | memory-side descriptor (`FLAGS` in Rockbox) | |
| `+0x18` | `PER_ADDR` | peripheral port; **does not advance** |
| `+0x1c` | peripheral-side descriptor (`INCR` in Rockbox) | |

`+0x08` and `+0x0c` are not touched by either firmware. **Undetermined.**

How we know which end is which: at `0x28e914` Apple swaps the two pointers when
changing direction — for peripheral→memory the first is `base+0x18` and the
second `base+0x10` (`0x28e984`/`0x28e994`), and for memory→peripheral the other way round
(`0x28e9e8`/`0x28e9f8`) — and bit 27 of `CMD` goes with them.

`CMD` bits that matter: 15:0 = **bytes − 4** with the low two ignored
(which is why the maximum for one transfer is 64 KB, and why Apple splits its
upload into `0x10000` chunks), 19:16 `REQ_ID`, 24 `WAIT_REQ`, 26 `SINGLE`,
27 `RAM_TO_PER`, 30 `INTR`, 31 `START`.

The real sequence, as the retailOS programs it for the Broadcom
(`0x28e248` and `0x28ea14`), with `CMD` equal to `0xCC00FFFC` at the moment of
starting:

```
MASTER_CONTROL |= 1<<31
CMD &= ~START            ; stop the channel
CMD: only SINGLE set     ; init
CMD |= RAM_TO_PER
RAM_ADDR = source ; PER_ADDR = 0x30000000
CMD = (CMD & 0xffff0000) | (bytes-4)
read STATUS              ; clears any pending interrupt
CMD |= INTR
CMD |= START             ; and only this moves anything
```

**That reading `STATUS` clears the interrupt** is not an assumption: it is the reason
for the warning at the top of `pcm-pp.c` —*"DMA status cannot be viewed from outside
code in control because that can clear the interrupt from outside the handler"*—
and both handlers depend on it (`pcm-pp.c:203`, Apple `0x1d9bf4`).

And Apple's handler **uses neither `DMA_MASTER_STATUS` nor the channel number**:
it walks its own list of active channels, reads each one's `STATUS` and checks
bit 30 (`0x1d9bf8`). A model that sets that bit and raises the line is enough.

### What is inferred and what is missing

- **That `PER_ADDR` is a fixed port is INFERRED.** No source says so; it is the
  only thing compatible with the two real uses, Rockbox's I²S FIFO and Apple's
  Broadcom data port.
- Bits 26:24 of the two descriptors: **undetermined**. Rockbox calls them
  `ALIGNED`/`UNK26` and itself marks the names as guesswork; Apple's mappings
  don't match between the two sides.
- Whether the hardware clears `START` only on completion: **undetermined**. Not
  clearing it is compatible with both firmwares — Apple turns it off by hand
  when the chain ends (`0x1d9ecc`) and Rockbox reads `CMD` back expecting to
  find its configuration (`pcm-pp.c:161`).
- **The peripheral request lines are not modeled.** A channel with `WAIT_REQ`
  set is left idle, because there is no peripheral here to request. That is
  what keeps Rockbox's audio path the same as before.

### A detail that cost a cycle

The retailOS uploads the Broadcom firmware **byte by byte**. The
peripheral-side descriptor it programs for that transfer leaves the width field
(30:28) at zero, and `pp5020.h:699` spells zero as 8 bits. Verified by reading
the register with the machine running: `0x6000901c = 0x02000000`. The BCM model
rejected single-byte accesses and all four transfers failed on the first
access.

## The Broadcom BCM2722

The panel is not driven by the PP502x: it is driven by this chip, which also
runs its own processor —the VideoCore— with a firmware (`vmcs`, 201,376 bytes)
that the host uploads to it at boot.

| Register | Address | Function |
|---|---|---|
| `BCM_DATA` | `0x30000000` | data port, as wide as the access |
| `BCM_WR_ADDR` | `0x30010000` | write pointer, **16 bits**, loaded as two halfwords |
| `BCM_RD_ADDR` | `0x30020000` | read pointer; bit 0 read = accepts a new address |
| `BCM_CONTROL` | `0x30030000` | bit 1 ready to write, bit 4 ready to read, bit 6 booted, bit 7 busy |
| `BCM_ALT_*` | `+0x40000` from each | **the same set again**: it is a second, independent port |

Both ports address the same chip memory but have **their own pointers**.
Rockbox only uses the primary one; the retailOS uses both at once and pushes
image data through the alternate one (`0x164524` and `0x165024` pass port 1).

Addresses inside the chip that matter:

| Address | What it is |
|---|---|
| `0x00000000` | `BCMA_SRAM_BASE`, where the `vmcs` firmware is uploaded |
| `0x000001F0` | `BCMA_HANDSHAKE`. The retailOS writes `{1, 0xA5A5}` and expects to read `{2, 0xA5A5}`; the model implements it (`ipod_video_bcm.c:98,417-424`) |
| `0x000001F8` | `BCMA_COMMAND`, the mailbox |
| `0x000001FC` | `BCMA_STATUS`; **this is where the VideoCore publishes the base of its host interface**, not a status register |
| `0x000E0000` | `BCMA_CMDPARAM`, where **Rockbox** writes the pixels. Apple never uses it |
| `0x10000400` | VideoCore doorbell, magic `0xA5A5` in the high halfword |
| `0x10000C00` | VideoCore boot: bit 31 written releases it, bit 0 read is its acknowledgment |
| `0x10001400` | touched by Rockbox's `bcm_powerdown()`; what it does is undetermined |
| `0xC0000000` | the chip's SDRAM. Not modeled |

The responses the model has to give, with what pins down each one, are in
[04-roadmap.md](04-roadmap.md). In short: bit 0 of `0x10000C00` and
`BCMA_COMMAND` reading **exactly 1** (`0x288080` does `cmp r0,#1`).

**Apple doesn't draw via the Rockbox path.** There isn't a single instruction
in seven megabytes that loads `0x000E0000` or `0xFFFF0000`
(`BCMCMD_LCD_UPDATE`), and the PP502x LCD controller (`0x70003000`) has a single
reference in the whole image, in the boot-time peripheral shutdown routine. It
draws through the VideoCore host interface, described right below.

### The VideoCore host interface

`BCMA_STATUS` is not a status register: it is where the VideoCore publishes a
pointer into chip memory, and an entire protocol lives there. Modeled in
`hw/arm/ipod_video/ipod_video_vc.c`.

Header, three sixteen-byte blocks from the base:

| Offset | Contents | Who reads/writes it |
|---|---|---|
| `+0x00`…`+0x0f` | eight `u16`s: offset of each channel's control block, relative to the base. **0 = channel absent** | `0x2880b0` copies them, `0x2871d4` indexes them, `0x2871e0` skips the zeros |
| `+0x10` / `+0x11` | **host** signal / acknowledgment byte | `0x28823c`, `0x288138` |
| `+0x20` / `+0x21` | **VideoCore** acknowledgment / signal byte | `0x288240`, `0x288118` |

Signaling is **by toggling**: there is an event on channel *n* when bit *n* of
`signal ^ ack` is set (`0x288244`, `0x288120`).

Channel control block, eighty bytes (`0x2882c0` reads them in one go):

| Offset | Field |
|---|---|
| `+0x04` (u16) | **channel type**: 1 ASCII, 2 graphics, 5 VCFS, 6 VCHR, 7 undetermined |
| `+0x06` / `+0x08` | start / end of the **TX** ring (host → VC) |
| `+0x0a` / `+0x0c` | start / end of the **RX** ring (VC → host) |
| `+0x10` | TX read pointer — **moved by the VideoCore** |
| `+0x20` | TX write pointer — moved by the host |
| `+0x30` | RX read pointer — moved by the host |
| `+0x40` | RX write pointer — **moved by the VideoCore** |

The ring offsets are relative to the interface base (`0x288478` adds
`[0x10831980]`). The usable capacity is `end − start − 16`: those sixteen
reserved bytes are what keep the writer from catching up to the reader
(`0x28874c`). Each pointer lives on its own sixteen-byte line and is
transferred with its own sixteen-byte operation (`0x288928`, `0x288800`,
`0x28850c`, `0x2885cc`) — that way both sides share the ring without a lock.

Message: sixteen-byte header and payload rounded up to a multiple of sixteen
(`0x28861c` composes, `0x2872fc` reads).

| Offset | Field |
|---|---|
| `+0x00` | magic **`0xF1A55A1F`** (`0x2886a0`; `0x287360` rejects a response without it) |
| `+0x04` | per-channel sequence, bit 31 forced to 0 |
| `+0x08` | opcode in the request, **result code** in the response |
| `+0x0c` (u16) | actual payload length; `0xFFFF` = NUL-terminated stream. **The model doesn't implement it**, see below |
| `+0x0e` (u16) | always zero. Undetermined |

> **`0xFFFF` is not implemented.** `ipod_video_vc.c:537-543` takes the length
> literally and computes `padded = (len + 15) & ~15`. With `len = 0xFFFF` that
> gives `0x10000`, much larger than the ring (`VC_RING_LEN` = `0x800`), so the
> space check is never satisfied and **the message stays stuck in the ring
> forever**. Today it isn't reached because the retailOS sends real lengths,
> but if anyone relies on this row, they should know that it describes the
> protocol, not the model.

The host → VideoCore doorbell is **the halfword `0x0031` to `BCM_CONTROL`**
(`0x287ba4`: `mov r0,#49` and `strh`), not `0x10000400`, which only appears at
boot. It is rung when the ring goes from empty to non-empty, when occupancy
crosses 3/4, or when the reader has caught up to the last published item
(`0x288880`).

**No interrupt is needed for the request/response path.** All of the retailOS's
requests are synchronous within a task (`0x28861c` and then `0x2883d4`), and
`0x2883d4` only sleeps if the ring is empty: `0x2883f8` returns as soon as it
has what it asked for and `0x288418` refreshes the pointer from chip memory
before `0x288424` decides to wait. If the response is already in place when the
guest looks, `0x2870dc` is never reached.

The **opposite** direction —the VideoCore speaking first— would need one, and
it is not modeled: the dispatcher `0x288100` only runs from the VCIH task
(`0x1239b8`), which waits for an event in `[0x1081eca8]` that **no instruction
in the image writes**. It has to come from the BCM's interrupt line, which is
still **unidentified**.

### The two channels that draw

**Type 1, ASCII commands.** `0xe9358` sends the command with the NUL included
and opcode 1; the response is `key=value` text terminated by NUL. The
vocabulary is the firmware's own: `vmcs.bin` registers twenty commands with
`gencmd_register`, their names in a contiguous block at offset `0xBE24`, and
their response templates at `0xBFF8` (`value=%d`) and from `0xC004` onward
(`error=%d error_msg="…"`).

**The model accepts 25, not 20**: the twenty from `0xBE24` plus five
`power_management` subcommands taken from `vmcs.bin:0xF98C` (`pm_set_policy`,
`pm_get_status`, `pm_show_stats`, `pm_start_logging`, `pm_stop_logging`),
because the retailOS sends them on their own as if they were top-level
commands (`ipod_video_vc.c:247-259`). That is what explains why
`pm_set_policy min` appears in the list below.

The retailOS sends five at boot:

```
set_vll_dir /Resources/VideoCore/Library
display_control 0 freeze=0
display_control 0 model=1 power=-2
power_management set_policy manual 27
pm_set_policy min
```

`0x2872c0` returns success only if **at least one byte** of payload came back
(`0x2872f0`), so the empty string is fine but an empty payload is not.

**Type 2, binary graphics RPC.** All calls have the same shape: send 16 or 32
bytes with the opcode in the header, read 32 back, and keep the first word of
the payload.

| Opcode | Routine | What it is |
|---|---|---|
| 8 | `0x286ca8` | **create surface**. The payload carries format (+4, byte), width (+8), height (+12), stride (+16) and the chip address the caller wants (+24). **Zero means "you pick it"**, and the response returns the handle in word 0 and the address in word 1 |
| 3 | `0x286b6c` | **present**. Handle at +12, width at +24 and height at +26 (u16) |
| 1, 2, 4, 9, 16 | various | pass tokens among themselves; the host doesn't branch on any of them. Undetermined |

And here is the twist that changes everything: **the host pushes the pixels**.
After the response to opcode 8, `0x164500` checks that the address isn't null
and `0x164524` uploads the entire surface through the **alternate port**;
afterward `0x165024` uploads only the dirty band. So Apple's framebuffer ends up
inside the chip's memory, at the address we chose, without the VideoCore
needing to ask for anything.

The format code is Broadcom's `VC_IMAGE_T`, and it is not a guess: `0x286a58`
computes the stride from it and its four cases —15 at four bytes per pixel, 1
at two, 5 at three, 17 and 18 at two— match one-to-one with `RGB565 = 1`,
`RGB888 = 5`, `RGBA32 = 15`, `RGBA565 = 17`, `RGBA16 = 18`. The retailOS labels
its bitmaps `0x0565` and `0x1888` and `0x164464` translates them to 1 and 15.
The one that shows up on this machine is 1.

## The click wheel

It shares a window with the I2C at `0x7000c000` and **shares its interrupt**
(`I2C_IRQ`, number 40). It is interrupt-driven, not polled:
`ipod_4g_button_read()` is only called from `ipod_4g_button_int()`.

| Address | Function |
|---|---|
| `0x7000c100` | configuration; `opto_i2c_init` writes `0xc00a1f00` |
| `0x7000c104` | status. Bit 26 = packet available. Acknowledged by writing `0x0c000000` over it |
| `0x7000c140` | `CLICKWHEEL_DATA`, the packet |

Packet format, from `button-clickwheel.c`:

```
valid packet:  (status & 0x800000ff) == 0x8000001a
  bit 8   SELECT     bit 9   RIGHT     bit 10  LEFT
  bit 11  PLAY       bit 12  MENU
  bit 30  wheel touched
  bits 16..22  position, 0..0x5F, clockwise increments
```

96 clicks per revolution (`WHEELCLICKS_PER_ROTATION`). Rockbox requires
`WHEEL_SENSITIVITY` (4) accumulated clicks before moving the selection by one
item, and **resets the accumulator when the direction changes**, so the first
press after reversing direction moves nothing. That's the firmware, not the
model.

**The retailOS uses the same format, byte for byte.** Its handler is
`0x281350`, and it's worth looking at because it confirms the packet from a
source completely independent of Rockbox:

```
281358:  ldr r0, [r1, #0x104]        ; r1 = 0x7000c000
28135c:  tst r0, #0x04000000         ; the same bit 26
281364:  ldr r0, [r1, #0x140]        ; CLICKWHEEL_DATA
281370:  and ip, r0, #0xbc0000ff     ; its own mask, leaves out bit 30
281374:  cmp ip, #0x8000001a         ; the same packet signature
281380:  lsl ip, r0, #18 ; lsr ip, ip, #26   ; buttons, bits 8..13
281384:  tst r0, #0x40000000         ; wheel touched
2813e0:  ldr r0,[r1,#0x104] ; orr r0,r0,#0x04000000 ; str  ; acknowledge
2813ec:  ldr r0,[r1,#0x100] ; orr r0,r0,#0x60000000 ; str
```

Two differences from Rockbox, neither fundamental: the validation mask is
`0xbc0000ff` instead of `0x800000ff` —it additionally requires bits 26..29 to
be zero— and it acknowledges by writing only bit 26 to `+0x104`, plus
`0x60000000` to `+0x100`. And one that does matter and is described above: the
retailOS doesn't reach this handler if the low bank doesn't carry `HI_IRQ`.

## How an iPod 5G boots

From `bootloader/ipod.c`, which documents it precisely:

> *The Apple ROM loader loads the entire OSOS image to DRAM_START, whatever it
> contains. If the bootloader is appended to another image then it will've
> modified the entry point in the OSOS header such that the ROM will call the
> bootloader rather than the main image.*

In other words:

1. The **PP ROM** (in the chip's NOR flash, not on the disk) starts at reset,
   reads the firmware partition, loads **the entire `osos` image** into
   `DRAM_START`, and jumps to `DRAM_START + entryOffset`.
2. That `entryOffset` comes from the firmware partition's directory.

**We don't need the ROM.** The emulator can do its job directly: load the
`osos` at `0x10000000` and set the PC wherever appropriate.

### Dual boot, byte by byte

`ipodpatcher` doesn't replace Apple's firmware: it leaves it at offset 0 of the
`osos` image and appends the Rockbox bootloader after it, at `entryOffset`.

```c
/* ipodpatcher.c, add_bootloader() */
if (ipod->ipod_directory[0].entryOffset > 0) {
    entryOffset = ipod->ipod_directory[0].entryOffset;
} else {
    entryOffset = (ipod->ipod_directory[0].len + ipod->sector_size-1)
                  & ~(ipod->sector_size-1);
}
memcpy(ipod->sectorbuf + entryOffset, ipod->bootloader, ipod->bootloader_len);
```

For our dump (`entryOffset = 0x735a00`):

| Piece | Range in `osos` | Size | Virtual address | Starts with |
|---|---|---|---|---|
| **Apple retailOS** | `0x000000`–`0x735a00` | 7,559,680 B | `0x10000000` | ARM vector table |
| **Rockbox bootloader** | `0x735a00`–`0x742514` | 51,988 B | `0x10735a00` | `msr cpsr_c, #0xd3` |

Verified: at `+0x20` of the `osos` is the string `"portalplayer\0"`, which is
exactly the marker that `bootloader/ipod.c` looks for at `DRAM_START+0x20` to
decide whether it can jump back to `DRAM_START` and boot Apple's firmware.

And the retailOS vector table at `+0`:

```
+00  ea00007a   b +0x1f0      reset
+04  ea000067   b +0x1a4      undefined
+08  e59ff0dc   ldr pc,[pc]   SWI
+0c  ea00006b   b +0x1b4      prefetch abort
+10  ea000070   b +0x1c8      data abort
+14  eafffffe   b .           reserved
+18  ea000058   b +0x168      IRQ
+1c  ea00004e   b +0x140      FIQ
```

### Boot plan for the emulator

```
load assets/images-5g/osos_full.bin at 0x10000000
    PC = 0x10735a00   -> Rockbox bootloader
    PC = 0x10000000   -> Apple retailOS
```

Both images are extracted separately in `assets/images-5g/` so they can be
loaded individually while debugging.

## Trap: the directory offsets carry a bias

The offsets stored in the directory **are not counted from the start of the
partition** but from the second sector: the first one holds the `]ih[` header
and everything it points to is numbered after it.

In the iPod Video partition the bias is `+0x200`; in the `Firmware-XX` that
comes out of a Classic 6G `.ipsw` it is `+0x1000`. `fwtool.py` deduces the bias
by locating the directory and then applies it to all the `devOffset`s.

How it was detected: `bootloader/ipod.c` says that `"portalplayer"` is at
`DRAM_START+0x20`, and with the uncorrected offsets it appeared at `+0x220`.


## The other path: Apple's retailOS

> ### ⚠ SUPERSEDED — the retailOS **does boot**
>
> Everything that follows, up to the section *"Resolved: who writes global
> 28"*, is the **historical record** of a diagnosis that began with the premise
> *"now we know exactly why it doesn't boot"*. That premise stopped being true:
> since commits `5b5af8966` and `662d0fb83` the retailOS boots, **draws its
> interface and can be navigated with the wheel and buttons**.
>
> It is kept because it documents hypotheses that turned out to be false, and
> that prevents someone from raising them again. But **do not read it as the
> current state**. For the state: [04-roadmap.md](04-roadmap.md). For the full
> chronology with the withdrawn hypotheses: [09-history-5g.md](09-history-5g.md).

Everything above is the Rockbox path. Apple's original firmware is a separate
path, and at the time it was believed that we knew exactly why it didn't boot.

### The symptom (historical)

With `-M iPod-Video,entry=0` the screen stays black (0 pixels lit) and only 266
blocks are translated, versus ~3700 for Rockbox. The exception sequence is:

```
Taking exception 2 [SVC]
Taking exception 1 [Undefined Instruction]
Invalid write at addr 0xFFFFFFC8 ... rejected
Taking exception 4 [Data Abort]        <- in an infinite loop
```

That is the **end** of the story, not the beginning. Tracing backward:

### The chain, from back to front

The retailOS's own SWI vector, in the image's vector table:

```
+08: e59ff0dc   ldr pc, [pc, #0xdc]   -> literal at 0xEC = 0x40000008
```

So its SWI handler lives in **IRAM**, which on our machine is empty. On the
SVC, the CPU jumps into blank IRAM, walks forward through the zeros until it
stumbles on an undefined instruction, and that exception's handler does a
`push` onto a stack that was never set up: `0xFFFFFFC8` is the stack pointer
wrapped below zero. Hence the loop.

But the SVC isn't a fault, it's **deliberate**: `0xe9b98` is the firmware's
fault reporter. Nearby are the strings `"Type: "` and `"Undefined
instruction"`, and the SVC itself is `ef123456`, a magic value. The firmware had
already detected a problem in software and was trying to report it.

What problem? The call comes from here:

```
144cc8: movs r3, r0        ; is the result zero?
144cd4: beq  0x10c360      ; yes -> report fault
```

and its caller is an assert with a line number:

```
d98: ldr r0, [r4, #0x24]   ; computed end of the image
d9c: ldr r1, [r4, #0x20]   ; available memory
da0: cmp r0, r1
da8: movhi r1, #231        ; line number
db0: blhi 0x144cc8         ; if it doesn't fit -> fault
```

Reading the structure at runtime with `xp /16xw 0x40006000`:

```
+0x10 = 0x10000000   DRAM_START
+0x14 = 0x00800000   8 MB
+0x18 = 0x0020e294   image size
+0x20 = 0x00000000   <- available memory: ZERO
+0x24 = 0x00a0e294   computed end, ~10 MB
```

`0xa0e294 > 0`, so it fires. **The firmware thinks the device has zero bytes of
RAM.**

### Where that zero comes from

`+0x20` is the sum of four table lookups, indexed by the four nibbles of a
packed value (`0xdf8`). The table is at `0x16d8` and holds power-of-two sizes:

| index | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| size | 0 | 512K | 1M | 2M | 4M | 8M | 16M | 32M | **64M** | 128M |

It is the SDRAM bank configuration: one nibble per bank. The packed value
starts out as a constant, `mov fp, #0x700` at `0x1f4`, but before it is used
nibble 2 is overwritten (`0xe6c` is a nibble setter) with a field from a
structure that turns out to be zero.

And that structure is never filled in, because what fills it in is this:

```
95024: ...
9503c: bl  0xb68
95040: cmp r0, #0
95044: beq 0x95064      ; if r0 == 0, exit without doing anything

b68:   mov  r0, #0x60000000   ; PROCESSOR_ID
b6c:   ldrb r0, [r0]
b70:   cmp  r0, #0xaa         ; am I the COP?
b74:   movne r0, #0           ; no -> 0
b78:   moveq r0, #1
```

### The root cause: the retailOS needs both cores — HYPOTHESIS WITHDRAWN

> **Withdrawn by commit `9d823ec76`**: the COP was modeled and **it fixed
> nothing**. It was real hardware and the model stayed, but it wasn't the
> cause. Furthermore, the paragraph below describes a model that no longer
> exists: today the machine creates **two** ARMs (`max_cpus = min_cpus = 2`)
> and both really sleep.

`0x95024` **only does its job if it runs on the COP**. On the CPU it exits
immediately without touching anything.

And it's not an isolated case. The same image runs on both cores and forks
right after boot:

```
240: mov  r0, #0x60000000
244: ldrb r2, [r0]
248: cmp  r2, #0x55        ; am I the CPU?
24c: beq  0x25c            ; yes -> CPU stack, CPU path
250: ldr  sp, [pc, #168]   ; no  -> COP stack
254: bl   0x1498           ;        COP init
```

Our machine has `mc->max_cpus = 1`, a single ARM, and `pp502x_proc_id_read()`
always returns `PP_PROC_ID_CPU`. The COP doesn't exist, so it never runs its
half of the initialization, and the CPU ends up reading zeros where it expected
the memory configuration.

**And this is exactly what distinguishes Rockbox from the retailOS.** Rockbox
also waits for the COP —`core_thread_init()` wakes it and goes to sleep waiting
for it— but since our `sleep_core` doesn't really sleep, the CPU carries on and
boots anyway, sacrificing only what Rockbox would have programmed on the second
core. The retailOS is not so forgiving: without the COP it can't even compute
the RAM size.

### The COP, modeled

Done. `hw/arm/ipod_video/ipod_video.c` now creates **two** ARMs:

- `PROCESSOR_ID` (`0x60000000`) answers according to **who is asking**, by
  looking at `current_cpu->cpu_index`: `0x55` to core 0, `0xaa` to core 1.
  It used to be a constant.
- `CPU_CTL` / `COP_CTL` (`0x60007000` / `+0x04`) really sleep and wake.
  Writing bit 31 does `cs->halted = 1` plus `cpu_interrupt(CPU_INTERRUPT_HALT)`;
  writing without it reverts that and issues a `qemu_cpu_kick`. Reading returns
  bit 31 from the core's real state. That supports the three operations the
  firmware uses: going to sleep (`str 0x80000000`), waking the other one (read,
  `bic` of bit 31, write) and asking whether it is asleep (`lsr #31`).
- The interrupt controller now has **two outputs**, and each bank applies its
  own mask: `cpu_enabled` to core 0 and `cop_enabled` to core 1. The
  `COP_INT_EN` registers already existed in the map; now they do something.
- Both cores come out of reset at the same vector, as on the real hardware.

**It works, and it can be checked.** With `info registers` on core 1 during the
retailOS boot: `R02 = 0xaa` (it read its identity correctly), `R14 = 0x258` (it
returned via the COP branch), stack in IRAM, and PC stopped at `0xa057c`, right
after:

```
a0570: ldr r1, [pc, #16]      ; 0x60007000
a0574: mov r0, #0x80000000
a0578: str r0, [r1, #4]       ; COP_CTL = SLEEP
```

So the COP boots, recognizes itself, takes its path and **puts itself to
sleep** as its first act, exactly as Rockbox does in `crt0-pp.S`. Being stopped
there is the correct behavior, not a fault.

**Rockbox has not regressed.** `navtest.sh` gives the same sequence as the
reference saved in `assets/nav-baseline`: menu, the bar moves down two items,
SELECT enters. The pixel differences fluctuate slightly (14180 → 14157) because
the title ticker is in a different phase.

### The chip has to say what it is

A second fix, necessary and found along the way. `PP_VER1` and `PP_VER2`
(`0x70000000` / `+0x04`) spell out the model in eight ASCII characters. The
order comes from Rockbox's `debug-pp.c`: the four bytes of `PP_VER2` from most
to least significant, then those of `PP_VER1`.

Apple reads **a single character** out of all that, the sixth:

```
598: ldr r0, [0x70000000]     ; PP_VER1
5a4: lsr r0, r0, #24          ; after lsl #8 -> bits 23:16
5a8: cmp r0, #0x32            ; '2' -> PP5022
```

and with the answer it picks a different base address:

| character 5 | base |
|---|---|
| `'2'` (our chip) | `0x40020000` |
| anything else | `0x40018000` |

> An earlier version of this table included a row `'6' → 0x40020000`. **I have
> not been able to verify it** in any code comment or in the cited
> disassembly; the code only documents `cmp r0, #0x32`. It is withdrawn until
> someone backs it up.

Returning zero was not a harmless unknown: it was a third answer that the
firmware doesn't account for, and it fell through to another chip's base.

**Correction (2026-08-09).** This document said the registers hold
`0x50503530` / `0x32324300`, i.e. **`PP5022C`**, "what the device on the desk
reports". That is **false**. The code says (`ipod_video.c:746-747`):

```c
#define PP5022C_VER1        0x32324120   /* '2' '2' 'A' ' ' */
#define PP5022C_VER2        0x50503530   /* 'P' 'P' '5' '0'  */
```

which spells **`"PP5022A "`**, not `PP5022C`. It is counterintuitive, which is
why it's worth writing down: the real device *is* a PP5022C and Rockbox shows
it that way on its debug screen —that's where the old value came from— but
Apple's firmware only knows the forms `"PP5020A "` and `"PP5022A "`, so the
registers have to say the `A` form. The change was made by commit `0763f94eb`
and this document was not updated at the time.

### It still doesn't boot, and why

With both things in place, the retailOS **still dies at the same assert**. The
structure at `0x40006000` doesn't change: `+0x20` is still zero.

What did change is what it looks at. With the correct identification, `0x1084`
returns `0x40020000 + 24 - 0x100 = 0x4001FF18`, and that address is **entirely
zeros**. `[r4+4]` is still 0, so the memory nibble still comes out 0 and the
assert still fires.

So the parameter block the firmware expects at the top of IRAM is written by
nobody. Going over what there is: the `osos` itself doesn't touch it before
reading it (it has only executed ~284 blocks), and the COP is asleep at that
moment by design.

**A hypothesis I put forward that has to be thrown out**: I said that block was
left in IRAM by Apple's boot ROM. **It is false**, and the check that knocks it
down is cheap.

`0x1084` doesn't compute the address of a parameter block: it is a **"give me
the address of global variable N"**. It returns `IRAM_END + N - 0x100`, meaning
the system keeps its globals in the **last 256 bytes of IRAM**, and the base
depends on the chip's IRAM size (96 KB on the PP5020, 128 KB on the PP5022 —
hence `0x40018000` versus `0x40020000`).

It has 13 callers, and **some of them write**:

```
1034: mov r0,#16 ; bl 0x1084 ; str r4,[r0]     ; global_16 = r4
b01e8: mov r0,#48 ; bl 0x1084 ; str r4,[r0]    ; global_48 = r4
bbdc8: mov r0,#44 ; bl 0x1084 ; str r1,[r0]    ; global_44 = 0
28575c:                         str r5,[r0]    ; global_44 = r5
```

and symmetric readers (`0xfa4`, `0xfbc`, `0xf2360`, `0x285748`). It is the
system's own global variable area, not something inherited from the ROM. It
comes out as zero simply because IRAM starts at zero and **nobody has
initialized it yet**.

### What actually remains unexplained

The assert reads `[&global_24 + 4]`, i.e. the **global at offset 28**, and uses
it as a pointer: `[pointer + 0xe0]` gives the memory configuration nibble.

Of the 13 callers of `0x1084`, **none uses offset 28**. So whoever writes that
global either has the base cached and doesn't go through the helper, or the
value arrives by some other route. That is what remains to be located.

### And the COP isn't the explanation either

The second check: **the CPU doesn't wake the COP before it needs that value**.
Its path is `0x25c` → `bl 0x15f8` → `bl 0x1348` → `bl 0x1244` → `bl 0x15fc` →
`bl 0x27eaec` → `bl 0x13bc`, and inside there:

- `0x15f8` is literally `bx lr`, an empty stub.
- `0x15fc` masks interrupts on `0x60004000` (writes `~0` to the disable
  registers at `+0x1c`, `+0x28`, `+0x38`, `+0x11c`, `+0x128`, `+0x138`).

There is no write to `COP_CTL` on that path. So the reading that "the COP
fills in the global and that's why it fails" **doesn't hold up either**: if it
depended on the COP, the CPU would have to wake it first, and it doesn't.

What is true and still stands: `0x95024` is conditional on the COP
(`cmp r0,#0xaa`) and on the CPU it exits without doing anything. But it doesn't
follow that it is the one that sets global 28; that was an inference of mine,
not a verified fact.

### Honest status

Three hypotheses put forward about this same assert, three withdrawn:

1. *Blank flash* — false: the firmware doesn't read the flash before dying.
2. *The ROM leaves a parameter block* — false: it is the system's own global
   area, with writers inside the `osos`.
3. *The COP fills in the global* — not supported: the CPU never wakes it
   beforehand.

What is verified and solid, and doesn't change: the system thinks there are
zero bytes of RAM because global 28 is zero, and the assert on line 231 fires.
What's missing is finding who should write it.


## Apple's retailOS: exactly where it stops (2026-08-08) — SUPERSEDED

> **This entire section is a withdrawn hypothesis.** It was withdrawn in commit
> `d06ce3123`: `0xe9b98` **is not the firmware's first system call**, it is
> the **ARM C library's panic printer**, and the empty IRAM was a
> **consequence, not a cause** — the handlers are installed by the `osos`
> itself at `0x2a0`, some 150 blocks *after* the point of death. The premise
> held for three commits.
>
> The paragraph further below about "139 literals of `0x40000000`" is also
> false: it was a raw byte search that found floating-point tables
> (`0x40000000` is the high word of the double `2.0`). By decoding the
> instructions that actually load a literal, `findlit.py` reduces the 139 to
> **1** (commit `b9786a0bb`).
>
> It is kept as a record of the mistake. See [09-history-5g.md](09-history-5g.md).

With the chip string corrected (see the `PP_VER1` commit), the firmware passes
the RAM size assert and gets much further. It stops here, and the trace leaves
no ambiguity:



The SWI vector at `+0x08` is `e59ff0dc` = `ldr pc,[pc,#0xdc]`, which loads the
handler address from `+0xec`. That literal is **`0x40000008`**: IRAM. And IRAM
is entirely zeros, so the CPU NOP-sleds through it until it stumbles.

That is: **the firmware expects to find its exception handlers already
installed in IRAM before making its first system call.** Nobody puts them there
in our model.

What remains to be figured out is who should put them there:

1. The `osos` itself, at some point in its reset path that we aren't going
   through or that fails silently earlier.
2. The PP boot ROM, which we skip entirely. If it's this, **the flash dump
   becomes necessary** — and this time as a conclusion derived from the trace,
   not as a hunch.

The way to tell them apart is to search the binary for references to
`0x40000000`. **There are some**: 139 word-aligned literals, the first at
`0x7f7b4`. And `0x40000008` appears twice aligned — at `0xec` (the SWI vector)
and at `0x53f508`.

That tips the balance toward **hypothesis 1**: the `osos` does have code that
touches the IRAM base, so it is probably the one that should install the
handlers and doesn't get to do so. It remains to be checked whether any of
those references is really a copy of the vector table, and at what point in the
boot it should execute relative to the first system call at `0xe9b98`. The
second occurrence of `0x40000008` at `0x53f508` is the place to start looking.

### Method

The four obstacles of this phase were all resolved the same way:
**disassembling the binary at the address the trace points to**, instead of
reasoning about which peripheral might be missing. The three hypotheses that
were put forward about the hardware —blank flash, ROM parameter block, the
COP— all three failed. The `"PP5020A "` literals were in plain sight, twelve
bytes after the comparison that uses them.

## Resolved: who writes global 28, and the three earlier hypotheses (2026-08-09)

Everything above about the line 231 assert is closed here. The question that
remained open —*"of the 13 callers of `0x1084`, none uses offset 28; who writes
that global"*— has an answer and it is none of the three candidates that had
been put forward.

**It is written by Apple's boot ROM, and hypothesis 2 is therefore
reinstated** — but not as it had been formulated. Both things were true at once
and that's why it was discarded too hastily:

- `0x1084` **is** a "give me the address of global N", and there are callers
  that write. That was correct.
- And **in addition** entries 24 and 28 of that same area are the
  `{tag, pointer}` pair that iPodLinux calls SysInfo, at `0x4001ff18` and
  `0x4001ff1c`. The fact that the `osos` uses that area for its own globals
  doesn't prevent the ROM from having left two words in it beforehand.

The check that closes it: the constant `0x53797349` (`"IsyS"`) is loaded by
**a single instruction in the entire image**, `0x13fc`, and it is a
*comparison*. It is never written. A value the firmware only compares and never
produces has to come from outside the firmware.

It matches iPodLinux's `ipodloader2/ipodhw.c` exactly, which reads those two
addresses and notes that the last 256 bytes of IRAM are used by the Flash ROM.

### The three hypotheses, in their final place

| Hypothesis | Final verdict |
|---|---|
| *Blank flash is the problem* | **Still false.** The firmware doesn't read the flash before dying, and it doesn't read `0x2000405c` either: `findlit.py` doesn't find a single instruction that loads that literal. |
| *The ROM leaves a parameter block* | **Reinstated, with a nuance.** It's not "the `0x1084` area belongs to the ROM": it's that two specific words of that area are left by the ROM. |
| *The COP fills in the global* | **Still not supported**, and now also unnecessary. |
| *The `osos` has 139 references to IRAM* | **Still false** (they were floating-point constants), but the conclusion drawn from it —that the `osos` installs its own handlers— turned out to be **true**: it does so at `0x2a0` → `0x83848`. |
| *A flash dump is needed* | **Not to boot.** It would be the ideal source for learning the rest of the SysInfo block, which today is zeroed. |

### And one that hadn't been put forward: bit 26

The wall remaining between "passes the assert" and "boots the kernel" wasn't
any peripheral: it was the memory map. The SDRAM controller doesn't decode bit
26 of the address, so every DRAM address has a twin `0x04000000` higher up, and
Apple's firmware uses both interchangeably. The full derivation is in
`04-roadmap.md` and in the `DRAM_ALIAS_BIT` comment.

Along the way, two corrections to the remap unit described further above:

- The window that puts DRAM at address 0 for Apple's firmware is **MMAP1**, not
  MMAP0. Its MMAP0 (logical `0x00003bf0` / physical `0x00003a88`) points to
  physical 0. It worked by accident because the model enabled the remap on any
  write to MMAP0.
- The mask: bit *k* of the logical register covers bit *k+16* of the address.
  It comes from comparing the two that `crt0-pp.S` uses — `0x3e00` for a 32 MB
  window and `0x3c00` for 64 MB: removing bit 9 doubles the window, so bit 9 is
  address bit 25. It is still not applied, but it is now documented.

### Method, revised

The lesson from the previous phase —disassemble the address the trace points
to instead of reasoning about which peripheral might be missing— still holds,
with an important correction: **before that, let the firmware talk**. The panic
printer had been interpreted as "the first system call" for three commits. Ten
seconds of semihosting would have said `Type: _sys_exit` and four empty memory
banks.


## Model assumptions that are not data

Added on 2026-08-09 after auditing the code against this document. The tree
marks its assumptions with `ASSUMED` / `INFERRED` / `UNDETERMINED` in capitals
next to the value, but several of the oldest ones —the infrastructure ones— had
never made it to the wiki. They are here so nobody takes them for measurements.

| Assumption | What was chosen | Basis |
|---|---|---|
| **DRAM size** | 64 MB | What Rockbox reads from the silicon. **Two sources say 32** (the default of Apple's own firmware and the iPodLinux table). Open question, see the memory map |
| **IRAM size** | 128 KB (`0x20000`) | **Derived, not invented**: Apple's firmware places its globals at `IRAM_END - 0x100 + N` and uses `0x40020000` as `IRAM_END` for a PP5022 (`0x40018000` for the PP5020, i.e. 96 KB). The comment at `ipod_video.h:116-119` called it "deliberately generous", citing Rockbox's linker script; that predates this finding |
| **Where the SysInfo block goes** | `0x4001fe00` | **Our choice.** The {tag, pointer} pair at `0x4001ff18`/`+4` is documented by iPodLinux and confirmed by the firmware, but **where the pointer points nobody knows**: no ROM dump exists. The 256 bytes just below the mailbox were chosen because they are clear of everything the firmware uses early |
| **`BCM_VC_DONE = 1` as the response to a finished command** | reuse the 1 | The 1 is **derived** for the *boot handshake* (`0x288080` does `cmp r0,#1`). Reusing it as the acknowledgment of any finished command is a **choice**, marked `ASSUMED` in `ipod_video_bcm.c:176-183` |
| **I²C result code when nobody answers** | `1` | **Placeholder.** It is known that bits 2:0 are a code and that 0 is success; the values 1..6 are undetermined. The model returns an invented 1 on NAK (`ipod_video_i2c.c:78-84`) |
| **PCF50605 ADC conversion** | instantaneous | The real chip takes an amount of time that is not documented. The firmware spins on the "ready" bit, so answering immediately works, but it's not what the hardware does (`ipod_video_pcf50605.c:53-58`) |
| **Only 4 fields of the SysInfo block are written** | the rest zero | A **deliberate** decision: iPodLinux's `sysinfo_t` puts `sdram_size` at `+0x80` and this firmware reads it at `+0xe0`, inside a pad that iPodLinux never decoded. Filling it in from there would be *"inventing with the appearance of rigor"* |

The Broadcom and VideoCore assumptions (host interface base, chip SDRAM size,
opcodes 1/2/4/9/16, channel type 7) are documented, in
[04-roadmap.md](04-roadmap.md).

## Differences between the model and the hardware worth keeping in mind

These are not assumptions: they are places where the model **knowingly** does not
reproduce the hardware, and where relying on this document without reading the
code leads to mistakes.

- **`TIMERn_VAL` reads 0**, not the remaining count.
- **The interrupt controller does not model priority or FIQ**; FIQs
  always return 0. It does model `FORCED_SET`/`FORCED_CLR`.
- **The GPIO does not generate interrupts**, and `INPUT_VAL` returns a fixed
  `0xffffffff`: the inputs are active-low, so a zero would mean "all
  buttons pressed, hold included".
- **The DMA ignores the peripheral's request lines** (`WAIT_REQ` leaves the
  channel idle) and takes the access width only from the peripheral-side
  descriptor; the memory-side one is not looked at.
- **The wheel never "lifts off"**: `wheel_touched` gets set and is never cleared.
- **The remap `mask` field is not applied**: the whole DRAM is always mapped and
  the permission flags are ignored.
- **The IDE is not migratable** and its DMA state is not saved; the rest of
  the devices do have a `VMStateDescription`.
- **The BCM starts with the VideoCore already running**, the same fiction as
  `GPO32` bit 14, so that Rockbox does not attempt the bootstrap.
- **The `rect` of `LCD_UPDATERECT` is ignored** and the whole screen is repainted.
- `I2S_MEM_BASE`, `LCD_MEM_BASE`, `SER1_MEM_BASE`, `PP_SER0_IRQ`,
  `IDE_TASKFILE_OFF` and `IDE_CONTROL_OFF` are **defined and unused**: they are
  documentation inside the header, not model.
