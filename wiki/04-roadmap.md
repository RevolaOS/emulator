# Roadmap

> **Status as of August 10, 2026: both machines boot Apple's original
> firmware and both can be navigated.** This document is the longest and the
> closest to a diary, and therefore the one that falls behind the most. The
> sections before "Strategy" are the journey of the iPod Video 5G and remain
> valid. The Classic part, from "Strategy" to the end, was written
> when its firmware was unreadable: **it is corrected at the end**, in
> ["Current status"](#current-status-august-10-2026), and the full chronicle
> is in [12-history-6g.md](12-history-6g.md).

## Two targets, two machines

The project started out aiming at the **iPod classic 6G** (S5L8702) and ran into
the fact that its firmware is encrypted with a key that lives in the silicon. When a
physical **iPod Video 5G** turned up, whose firmware is in the clear, the goal of booting
Apple's original system moved there. Later, the Classic blocker fell
by a path that was not the expected one: **the key did not have to be extracted; instead
the device had to be asked to decrypt** ([10](10-classic6g-emcore.md)).

| Machine | SoC | Status |
|---|---|---|
| `iPod-Classic` | S5L8702 | **boots Apple's retailOS**: RTXC kernel, ATA with DMA, `rsrc` volume mounted, fonts, and the language screen — 75 463 of 76 800 pixels — which the wheel moves and the center button passes through |
| `iPod-Video` | PP5022C | **Rockbox is fully navigable, and so is Apple's retailOS: it boots, draws its interface and responds to the wheel and the buttons** |

## iPod Video (PP502x) — status

With `-M iPod-Video,entry=0x735a00 -kernel osos_full.bin`, the Rockbox
bootloader extracted from the real device goes through this sequence:

1. Reads `PROCESSOR_ID` (`0x60000000`) and recognizes itself as the CPU
2. Waits for the COP to declare that it is sleeping (`COP_CTL`, bit 31)
3. Copies itself to IRAM (`0x40000000`)
4. Configures GPIO — six pin-direction writes
5. Queries the hardware revision at `0x405c`
6. Performs **I²C transactions** against the PMU and the codec (`0x7000c100`/`0x104`/`0x120`)
7. Reaches the **IDE controller** (`+0x1fc`), looking for `rockbox.ipod` on the FAT32

Modeled so far: DRAM, IRAM, NOR flash window with the hardware
revision, **both ARM cores** (CPU and COP, which genuinely sleep and wake
up), `PROCESSOR_ID`, `CPU_CTL`/`COP_CTL`, `USEC_TIMER`, 16550 UART,
interrupt controller, IDE with ATA DMA, click wheel, GPIO, cache,
remap (with the bit 26 and bit 31 aliases), **the two PP502x DMA
blocks**, **the I²C and the PCF50605**, the BCM2722 with **the VideoCore
host interface**, clock/PLL, device control and **ARM semihosting**.

### The bootloader now paints on the emulated screen

Current status: the Rockbox 4.0 bootloader boots fully and writes to the LCD:

```
No partition
found (0).
Insert USB cable
and fix it.
4.0
```

Getting there meant removing four blockers, all of the same kind —
a status bit that the real hardware sets and that we returned as zero, with
the firmware waiting for it in a loop with no way out:

| Symptom | Cause | Fix |
|---|---|---|
| loop reading `0x6000603c` | `PLL_STATUS` bit 31 = PLL locked. `system-pp502x.c`: `while (!(PLL_STATUS & 0x80000000));` | clock block at `0x60006000`, with bit 31 always set |
| `Invalid write at 0xF0004000` | `cache_invalidate_special()` writes the cache's entire status array by hand, which was not mapped | RAM at `0xf0000000`–`0xf000efff` |
| 20 M reads of `0x70000028` | bit 7 = the USB block came out of reset. `usb-fw-pp502x.c`: `while ((inl(0x70000028) & 0x80) == 0);`. That register **does not appear in `pp5020.h`** | `DEV_CTL` block at `0x70000000`, with bit 7 always set |
| everything ran but the screen stayed black | `GPO32_VAL` bit 14 powers the Broadcom. If it is 0, `lcd-video.c` thinks it must initialize the BCM by uploading its `vmcs` firmware from the NOR (which we do not have), leaves `display_on = false`, and **`lcd_update_rect()` silently discards all drawing** | `GPO32_VAL` starts with `0x4000`, reproducing what Apple's loader leaves behind |

The lesson: on this SoC almost every failure shows up the same way — the emulator
looks alive, there is no exception at all, and the firmware is waiting for a bit.
`-d unimp` with `sort | uniq -c | sort -rn` finds the loop in seconds.

Careful with `-d unimp`: when the guest enters a tight loop on an unmodeled
register, the log reaches tens of millions of lines and slows emulation down
so much that it looks hung. Always measure without `-d` before concluding that
something is not progressing.

### Rockbox really boots

Capturing the screen at several moments shows that `No partition found` is not
the end, it is the **second** frame:

| t | Screen |
|---|---|
| ~8 s | the **Rockbox boot logo** in color: "ROCKBOX — open source jukebox firmware", "Ver. 4.0" (538 colors, 22 000 pixels) |
| ~20 s onward | `No partition found (0). Insert USB cable and fix it.` |

In other words, the full sequence works: the bootloader mounts the FAT32, loads
`rockbox.ipod`, jumps to it, and **the Rockbox application boots and draws its
logo**. The error comes from the *app*'s `disk_mount_all()`, not from the bootloader.

### The real blocker: the app uses DMA, the bootloader does not

`-trace "ide_*"` separates the two phases unambiguously:

```
# bootloader
cmd 0xec  IDENTIFY
cmd 0xef  SET FEATURES  (x2)
cmd 0xc6  SET MULTIPLE MODE
cmd 0xc4  READ MULTIPLE   <- x40, loading rockbox.ipod
# app
cmd 0xec  IDENTIFY
cmd 0xef  SET FEATURES  (x3)
cmd 0xc6  SET MULTIPLE MODE
cmd 0xec  IDENTIFY
cmd 0xc8  READ DMA        <- and this is where it breaks
ide_reset                 <- immediately afterwards
cmd 0xec / 0xef / 0xc6 / 0xec / 0xe7 FLUSH CACHE
```

And sector 0 is read only twice, both times at the start: the app's
`disk_mount_all()` does not even reach `disk_init()`.

`ipodvideo` defines `HAVE_ATA_DMA`, so the app requests **READ DMA (0xc8)**
whereas the bootloader stays in PIO. We were using QEMU's generic `mmio-ide`,
which **has no DMA engine**: the transfer never happens,
`ata_wait_intrq()` exhausts its 10 seconds, the driver resets the bus and gives up.

What we ruled out along the way (all correct, do not touch): the image's
partition table, the FAT32 (512 B/sector, 2 FATs, `FATsz32 = 4033`,
`dataclusters ≈ 516190`, well above the minimum of 65525 in
`fat_mount_internal`), and `BPB_HiddSec = 0`, which Rockbox does not look at.

### Resolved: the PP502x IDE DMA engine

On the PP502x the DMA is not PCI bus-master; it is registers belonging to the IDE block.
From `ata-target.h` and `ata-pp5020.c`:

| Register | Address | Function |
|---|---|---|
| `IDE0_CFG` | `0xC3000028` | bit 3 `INTRQ` (pending, cleared by writing 1), bit 15 active during DMA |
| `IDE_DMA_CONTROL` | `0xC3000400` | bit 0 = **start**, bit 1 = arm, bit 3 = direction (1 = read to memory) |
| `IDE_DMA_LENGTH` | `0xC3000408` | bytes **minus 4** |
| `IDE_DMA_ADDR` | `0xC300040C` | destination physical address |

The address and length are programmed **before** the ATA command and the start
bit **after**, so `start_dma` can only store the callback the IDE core passes
to it; it is the write of bit 0 that triggers the transfer.
It is the same split that PCI bus-master IDE uses, for the same reason.

It lives in `hw/arm/ipod_video/ipod_video_ide.c`, a `pp502x-ide` device that
takes the whole `0xc3000000`–`0xc3000fff` window: the task file, the
configuration and timing registers, and the DMA. It replaces the generic
`mmio-ide` plus the stub that sat next to it.

A trap that cost one cycle: `ide_bus_init_output_irq()` **ends** with
`bus->dma = &ide_dma_nop`. If you claim the bus before calling it, the claim is lost
without any warning and all DMA commands are accepted and ignored. It has to be
assigned afterwards.

`INTRQ` is a **latch**, not a mirror of the interrupt line: during
DMA the firmware never reads the ATA status (doing so "causes failures and hangs"),
so that bit is its only end-of-transfer signal. It is set on the rising edge and
cleared by writing a one.

### The click wheel: two more things

With the disk mounted, Rockbox booted fully but did not respond to keys.
On the PP the wheel is **interrupt-driven**, not polled: `ipod_4g_button_read()`
is only called from `ipod_4g_button_int()`, and it does not even look at the data
unless a status bit is set:

```c
if ((inl(0x7000c104) & 0x04000000) != 0) {
    unsigned status = CLICKWHEEL_DATA;   /* 0x7000c140 */
    ...
}
/* and the handler acknowledges with: */
outl(inl(0x7000c104) | 0x0c000000, 0x7000c104);
```

So a press has to do two things: set bit 26 of `0x7000c104`
and raise the wheel interrupt, which on this SoC is **the high-bank I²C
line** (`I2C_IRQ = 32+8`). Without both, the firmware never reads the wheel.

Rotation goes in the same packet: bit 30 = wheel touched, position in bits
16..22, 96 clicks per revolution, clockwise increments. Rockbox requires
`WHEEL_SENSITIVITY` (4) accumulated clicks to move the selection by one item, so
each key event advances two and a complete press (press +
release) moves exactly one.

## Status: the menu can be navigated

`tools/ipod_video/navtest.sh` checks it on its own: it boots, leaves *Now Playing*
with MENU, turns the wheel and selects, capturing the screen at each step.

| Frame | Result |
|---|---|
| boot | *Now Playing* screen with theme, cover art, progress bar and battery |
| MENU | main menu: Files / Database / Now Playing / Settings / Recording / Playlists / Plugins / System |
| wheel clockwise ×2 | the selection bar moves down from *Now Playing* to *Settings* and then to *Recording* |
| SELECT | enters *Recording* (Time, Size, Volume −28 dB, Gain) |
| wheel counterclockwise ×3 | the selection moves back up |

A real detail, not a model bug: **the first press after changing
direction moves nothing**. Rockbox resets its accumulator when the direction
changes, so it swallows those four clicks. You can see it in the test figures: the
frame after the change of direction differs by ~2 300 pixels (only the title
ticker, which scrolls on its own) and the next one by ~14 000 already (the moved bar).

Watch out for that when measuring: the *Now Playing* screen animates by itself, so a
small pixel difference **proves nothing**. Only the moved bar counts.

### What remains unmodeled

> **Corrected on 2026-08-09**: this list opened with *"The COP: since our
> `sleep_core` does not sleep, the CPU continues and boots anyway"*. That stopped being true in
> commit `9d823ec76`: there are **two** ARM cores and both genuinely sleep and
> wake up. The CPU-COP mailbox at `0x60001000` does remain unmodeled.

- **Audio.** The I²S and the codec are not there; playback makes no sound.
- **The CPU-COP mailbox** at `0x60001000`, which falls on the floor of `-d unimp`.
- **DMA writes.** Only reads have been exercised. The write path
  is implemented but untested.
- **The wheel does not generate "untouch"**: once touched it stays touched.

## Apple's retailOS: from black screen to booted kernel

With `-M iPod-Video,entry=0` Apple's original firmware went from dying at
instruction 289 to executing 5175 blocks, booting its kernel, bringing up its two
cores and entering its scheduler's idle loop. **The screen is still
black**, and now we know exactly why.

### First: it was not silent, it was panicking

The address where all traces ended, `0xe9b98`, was not "the firmware's first
system call". It is the ARM C library's panic printer. The instruction above
gives it away:

```
e9b94:  mov r0, #4          @ SYS_WRITE0
e9b98:  svc 0x123456
```

and the string table just above contains `"Type: "`, `"undefined
instruction"`, `"data abort"`, `"_sys_exit"`, `"CPU "`, `"Task"`. With
semihosting enabled, the firmware itself says what it is dying of. The machine
enables it by default; `-M iPod-Video,semihosting=off` disables it.

**Retracted**: the hypothesis that the retailOS died in a system call, and
with it the one that the empty IRAM was the cause: the handlers are installed by
the `osos` itself at `0x2a0`, some 150 blocks *after* the point of death.

### The three walls, in order

| # | Wall | Diagnosis | Fix |
|---|---|---|---|
| 1 | `svc 0x123456` without a handler | it is the panic printer, not a syscall | respond to semihosting |
| 2 | data abort at `0x84390` reading `0x0471619c` | **bit 26 of the address is not decoded by the SDRAM controller**: every DRAM address has a twin 0x04000000 higher | DRAM aliases at `0x04000000` and `0x14000000` |
| 3 | data abort at `0x2668e4` on a null object | the **SysInfo block** that the boot ROM leaves at the top of IRAM is missing | write the tag, the pointer and three fields |

#### Bit 26

The firmware's own scatter-load table says so, at
`0x84040..0x840a8`. Sources and destinations come in a contiguous run of the file
and in the middle of that run the address jumps by exactly `0x04000000`:

```
src 0x006c9a64 + len 0x0004c734 = 0x00716198 ; next src = 0x0471619c
dst 0x1493690c + len 0x0004c734 = 0x14983040 ; next dst = 0x14983040
```

A one-bit discontinuity within a series of consecutive offsets is not a
change of address: it is the same address with a flag. The contents confirm it:
file offset `0x7161a4` is `e1b0f00e`, `movs pc, lr`; the copy at
`0x83848` leaves it at `0x40000008`, which is exactly where the
SWI vector literal at `0xec` points. It fits to the byte.

What is **inferred** is what the bit means to the hardware. The remap unit
points the same way: MMAP1 is programmed with logical `0x00003a00` /
physical `0x10000f84`, and comparing that mask with the two that Rockbox's
`crt0-pp.S` uses (`0x3e00` for a 32 MB window, `0x3c00` for
64 MB) shows that bit *k* of the mask covers bit *k+16* of the address
— that is, bits 26 and 27 reach the physical bus intact. Something further down
has to be ignoring bit 26, and the only candidate is the memory
controller.

#### The SysInfo block

iPodLinux's `ipodloader2/ipodhw.c` names the addresses and comments that *"the
last 256 bytes seem to be used by the Flash ROM for special things"*:

```c
#define SYSINFO_TAG_PP5022 0x4001ff18
#define SYSINFO_PTR_PP5022 0x4001ff1c
```

Apple's firmware matches to the byte: `0x1084` lays out globals at
`IRAM_END - 0x100 + N`, and `0x13bc` asks for `N = 24`, compares the word there
against `0x53797349` (`"IsyS"`) and takes a pointer from `+4`. Nothing in the image
writes either of the two words — the magic constant is loaded by **a single
instruction** in seven megabytes and only to compare it — so they were left by the
ROM we skip, just like bit 14 of `GPO32_VAL`.

Without the block two things break, and the second only became visible once the
first was fixed:

- **The RAM size.** `0x13e8` reads `SysInfo+0xe0` as a byte count and
  `0xe6c` rounds it against the table at `0x16d8` to the size of bank 2. With the
  null pointer that read goes to `[0x000000e0]`, which after the remap is a
  word of the vector table itself, `0x00000100`; 256 bytes round to
  zero, all four banks come out empty and the assert at `0xda0` fires. The
  striking part: the firmware already had the right answer — `0x1f4` does
  `mov fp, #0x700`, whose nibble 2 indexes the same table to `0x02000000`, the
  32 MB of a 5G — and throws it away.
- **The hardware identifier.** This is what the earlier analysis had
  backwards. Leaving the tag at zero seems harmless, because `0x13e8` dereferences the
  pointer *before* `0x1400` checks the magic. But `0x140c` only
  publishes the pointer at `0x4000608c` if the magic matched, and that word does
  matter:

  ```
  0x2b8   bl 0x1658    memcpy(0x14983060, 0x40006000, 0x90)
  0x2bc   bl 0x281224  reads [0x149830ec]; if null it returns at once, otherwise
                       copies min(len,248) bytes to 0x108710dc and installs it as
                       the hardware information singleton
  0x2d0   bl 0x2668c4 -> 0x111c58 -> 0x2653a4 -> 0x265164, which reads +0x84
  ```

  With the tag at zero `0x281224` does nothing, `0x265164` reads `+0x84` of an
  object in bss that nobody filled in and gets zero, the jump table at
  `0x2653a4` answers "I don't know that machine", `0x111c58` returns a null
  driver and `0x2668e4` dereferences it.

**Four fields and not one more** are written: the magic at `0x4001ff18`, the
length at `+0x04`, the hardware identifier at `+0x84` (which is
`IPOD_VIDEO_HW_REVISION`, the value this machine already declared, taken from the
real device via Rockbox's HW info screen — its high half, `0x000b`,
is entry 11 of Apple's table, live) and the SDRAM size at `+0xe0`.
The rest stays at zero: the serial number, the FireWire GUID, the board
name and the image revisions are unknown to us, and iPodLinux's `sysinfo_t`
**is not usable as a source** — it puts `sdram_size` at `+0x80` whereas this firmware
reads `+0xe0`, which falls inside a `pad` that iPodLinux never decoded.

### Where it got stuck: the PMU (resolved, 2026-08-09)

The system is **alive**, not hung:

- both cores execute (the CPU in the I²C driver, the COP spinning on a
  lock at `0x40000fac`),
- ~700 interrupts per second are taken,
- the whole reset sequence is traversed (`0x2668c4`, `0x276fe8`, `0xbbdc4`,
  `0x330`) and the OS `main` at `0xaf468` is reached,
- the scheduler enters its idle loop at `0x285730`.

And the CPU spends the rest of its time in the I²C block at `0x7000c000`,
repeating this transaction forever:

```
CTRL=0x80  ADDR=0x10  DATA0=0x30    write register pointer 0x30
CTRL=0x80  ADDR=0x11                 address for reading
CTRL=0xa2  ADDR=0x11                 read 2 bytes  (0xa2 = SEND | read | len 2)
```

Address `0x10` >> 1 = **`0x08`, the PCF50605**, the power management chip
(`i2c_readbytes(0x08, ...)` in Rockbox's `pcf50605.c`). Register **`0x30` =
`ADCS1`**, and reading two bytes also takes `ADCS2`: these are the **A/D converter
result registers**, i.e. the battery measurement.

The block's map, from `i2c-pp.c`:

| Register | Offset | Bits |
|---|---|---|
| `I2C_CTRL` | `+0x00` | bit 7 `SEND`, bit 5 = read, bits 2:1 = length−1 |
| `I2C_ADDR` | `+0x04` | shifted 7-bit address, bit 0 = read |
| `I2C_DATA0..3` | `+0x0c`,`+0x10`,`+0x14`,`+0x18` | |
| `I2C_STATUS` | `+0x1c` | bit 6 `BUSY`; Apple's driver also reads bits 2:0 as a result code (`0x282f40`) |

Today the block is a register file: `STATUS` always reads zero, so
every transaction "completes" instantly and successfully, and the firmware gets
back the last thing it wrote itself. Rockbox survives that because its
driver reads whatever is there and moves on; Apple's keeps spinning.

**The next step, scoped:** model the PP502x I²C controller and hang
off it a PCF50605 at address `0x08` that at least answers `ADCS1`/`ADCS2`.
The PMU register map is in
`assets/rockbox/firmware/export/pcf5060x.h`. What we do **not** have is the ADC
scale or the values the retailOS considers acceptable, so that
number will have to be derived from how the firmware interprets it, not invented.

It remains to be checked, and it is not ruled out, that the battery reading is the
only blocker: there may be more PMU consumers behind it.

## The retailOS boots all the way (2026-08-09)

With the I²C and the PMU modeled —plus two walls that showed up behind them— the
retailOS stops hanging: it finishes booting, enters the scheduler's idle loop
and prints not a single panic. **The screen is still black**, and now we know
exactly what is missing and why.

Screenshots at 10, 30, 60 and 90 s: **0 pixels lit in all four**. No
embellishment: the retailOS does not draw.

### The three walls of this session

| # | Wall | How it was found | Fix |
|---|---|---|---|
| 1 | infinite loop reading `ADCS2` of the PCF50605 | already narrowed down | I²C controller `pp502x-i2c` + `pcf50605` slave |
| 2 | data abort reading `0x93EE1B70` | the firmware itself prints it via semihosting, in a loop | DRAM also responds with bit 31 set |
| 3 | loop on bit 6 of `BCM_ALT_CONTROL` | `info registers` in the monitor: `R01=0x30070000` | the two bits of the Broadcom's boot *handshake* |

#### 1. The I²C and the PMU

The hang was `pmu_adc_read` (`0x28341c`) spinning **without a timeout** on bit
7 of `ADCS2`, the PCF50605's "conversion ready". Until now `0x7000c000` was
a flat register file, so every transaction "worked" and returned
the last byte the firmware itself had written.

Two new devices, `hw/arm/ipod_video/ipod_video_i2c.c` and
`ipod_video_pcf50605.c`. The details of the block are in
[07-pp502x-hardware.md](07-pp502x-hardware.md). What is worth keeping in mind:

- **Apple's firmware reads `STATUS[2:0]` as an error code** and aborts the
  entire transaction if it is not zero. Rockbox does not look at it. A flat
  register file satisfied this by accident.
- **The start bit has to self-clear.** This is not an assumption: every
  subsequent step of both drivers is a read-modify-write of `CTRL`, so a bit
  that stayed set would fire a second, unrequested transfer on the next one,
  and against a slave with an auto-incrementing register pointer that reads
  the wrong bytes forever.
- **The block has to work without initialization.** Apple's `DEV_EN`/`DEV_RS`
  routine (`0x282e98`) is only reached when a transfer times out; if `STATUS`
  never says `BUSY`, the guest never enables the block.

**The ADC values, and where each one comes from** — this is what the "don't
make things up" rule demanded:

| Mux input | Value | Justification |
|---|---|---|
| 2, `ADCIN1_RES` (Rockbox battery) | 682 | Rockbox converts it to millivolts with its own `BATTERY_SCALE_FACTOR` of 6000 per 1024 counts, so it is 4000 mV. Its iPod Video table puts 0 % at 3600 mV and 100 % at 4180. Rockbox now shows **77 %**, which is exactly what interpolating 3996 mV in that table gives |
| 3, `ADCIN1_SUBTR` (Apple battery) | 390 | **ASSUMED**, but within a range the firmware defines: `0x264ee8` looks up the raw count in a table of fifteen thresholds (283…454) and returns the index, and `0x264cb0` does not turn on a rail below the literal `0x12a` = 298 at `0x264d20`. 390 falls between the tenth and eleventh thresholds, far from both ends. **How many volts it is, is not known, and nothing depends on knowing it** |
| 4 and 7, accessory identifiers | full scale | `0x266034` takes the four high bits as a bucket and converts bucket 15 into code 100, which is the value the `"RsistrAccsryMgr"` task (`0xfec04`) is initialized with. An identification line with nothing plugged in is open-circuit and reads full scale, which is that same bucket. The two things agree |
| the rest | full scale | with `LOG_UNIMP` saying so |

**ASSUMED and marked as such in the code**: the `ID` register (`0x00`) returns
`0x00`. No source —Rockbox, iPodLinux, the OpenMoko header taken from the
datasheet— says what a real PCF50605 answers. All the firmware
needs is for it **not to be `0x24`**, which is what `0x283704` uses to decide to
switch to another chip's register map. Zero is chosen precisely because it adds
no information we don't have.

**Rockbox has not regressed, but it has changed**, and it is a fix, not damage:
it now boots into its main menu instead of going straight into *Now
Playing*. `rtc_check_alarm_started()` in `rtc_pcf50605.c` decides that the alarm
woke the device if the clock and the alarm registers match within
ten seconds. Without an RTC both reads returned the same thing, so Rockbox
believed an alarm had gone off and started playback **on every
boot**. With a real clock they do not match and it does the normal thing. The
screenshots in `assets/nav-baseline` are updated; navigation itself does not
change (the bar still moves ~12 000 pixels per wheel step and SELECT still
enters).

#### 2. Bit 31: the uncached view of DRAM

Past the PMU, the firmware took a data abort reading `0x93EE1B70` —DRAM that
it had just allocated itself, `0x13EE1B70`, with bit 31 stuck on— and printed it
via semihosting in a loop.

This wall **explains itself**; there is no need to infer it. Ten buffer
constructors share an idiom: `0x22131c` and nine from the allocation module
(`0x28c8fc`, `0x28c9dc`, `0x28cab4`, `0x28cb80`, `0x28cca8`, `0x28cd74`,
`0x28ce0c`, `0x28ced0`, `0x28d0ec`). From `0x28cebc`:

```
str  r0, [r4, #4]         ; the aligned pointer
ldrb r1, [r4, #12]        ; "the caller says it is already uncached"
cmp  r1, #0
bne  fin
mov  r1, #4
cmp  r1, r0, lsr #28      ; high nibble 4, i.e. IRAM?
beq  fin                  ; then leave it alone
orr  r0, r0, #0x80000000  ; otherwise, this is how the buffer is used
str  r0, [r4, #4]
...
bl   0x9aa18              ; over [ptr - slack, ptr + size + slack]
```

**The exception is what identifies the bit.** IRAM is the only memory on
this chip that is never cached, and it is exactly the one the firmware does not
mark. Rockbox says the same thing in C, with a different constant because its
cached view lives elsewhere: `system-target.h:164`,
`UNCACHED_ADDR(a) = a | UNCACHED_BASE_ADDR`.

How it was located, and the method is worth it: **a breakpoint on the firmware's
`memcpy` (`0xf0`) through QEMU's gdbstub**, asking who passes a
source pointer with bit 31. Answer: `0x21701c`, a 512-byte copy
taken from a 32 KB disk block buffer whose descriptor, at
`0x13ee9b90`, stores the raw allocation at `+0` and that same address with bit
31 at `+4`. Then a **write watchpoint** on that word, which lands
right in the constructor above.

Since there is no cache here, cached and uncached are the same memory: an alias,
just like the bit 26 twin that was already next to it.

#### 3. The Broadcom: the boot *handshake*

With that, the retailOS reaches the video chip and sits at `0x2876dc` waiting
for bit 6 of `BCM_ALT_CONTROL`. It is the second half of the Broadcom boot
that Rockbox also does (`lcd-video.c:559-560`):

```c
while (BCM_ALT_CONTROL & 0x80);      /* wait while busy */
while (!(BCM_ALT_CONTROL & 0x40));   /* wait until ready */
```

Rockbox never gets there on this machine, because `bcm_init()` only runs if
`GPO32` bit 14 says the chip is unpowered, and we pretend that it
is. **The retailOS boots it itself on every boot**, without looking at that
bit. The two new bits are answered like the two that were already there: never
busy, always ready.

And that uncovered two errors in the Broadcom model that Rockbox could never have
revealed:

- **The address registers are 16-bit.** Apple loads them as two
  halfwords, low first (`0x287c28`); Rockbox always writes the
  whole word via `BCM_WR_ADDR32`. Assigning the value on every write left
  only the high half, so a firmware upload to `0x000fc000` would have gone
  to `0x000f0000` silently.
- **The data port is as wide as the access.** Apple sends the last
  1..255 bytes of a block two at a time (`0x287ca8`); a halfword
  write that stored a full word clobbered the next two bytes.

### Where it stops, precisely

**The Broadcom firmware upload, which goes via DMA.**

`0x287be8` splits the transfer into a bulk and a tail, and hands the bulk to
`0x286fb4`, which flushes the cache over the source, chops it into 64 KB
pieces, calls `0x1599fc` for each one and **waits on a virtual method at
`+0x18` of the driver object**. Nothing modeled answers, so the task blocks
forever and the scheduler sits idle — which is why the machine looks
healthy and prints nothing.

The registers it touches already show up in `-d unimp` as `pp502x.soc-regs`
`0x6000b000`, `0x6000b020`, `0x6000b040` and `0x6000b060`: **DMA0..DMA3** from
`pp5020.h`. So the next thing is the PP502x DMA engine, and the arguments
of the call that blocks are known:

```
bcm_load(name = 0x001affe8, data = 0x13ea136c, len = 0x312a0)
   -> 201 376 bytes to the Broadcom SRAM, address 0
```

**And this has to be known before starting**: even if the DMA is modeled, the
screen will not turn on by itself. What gets uploaded is the chip's own
firmware —the `vmcs` blob, whose file name is literally in the image, at
`0x2878f4`: `"VMCS    BIN"`— and afterwards both guests **wait for that firmware
to answer**:

| Wait | Apple | Rockbox |
|---|---|---|
| bit 0 of the chip's `0x10000C00` word | `0x287868` | `lcd-video.c:589` |
| `BCMA_COMMAND` (`0x1f8`) nonzero | `0x2878b0` | `lcd-video.c:594` |

Those answers are produced by code we are not running. Either we emulate just
enough of the Broadcom to fake them (and then we have to figure out which command
protocol the retailOS expects, which need not be Rockbox's), or we
look for the path by which Rockbox skips the chip's boot and check whether the
retailOS has an equivalent.

## The Broadcom firmware uploads, and the chip answers (2026-08-09)

The above is done: **the retailOS completes the entire bring-up of the video
chip**. It uploads the 201 376 bytes of the `vmcs` blob via DMA, boots the
VideoCore, receives the two answers it was waiting for, discovers the chip's
interface and moves on. Ninety seconds of execution with `-d guest_errors`:
**the log comes out empty**. Not a panic, not a rejected access, not a new
unmodeled register. And the disk really works — 460 `READ DMA EXT` of 64
sectors, about 14 MB read, plus 78 writes.

**The screen is still black.** Screenshots at 10, 20, 40, 60 and 90 s:
**0 pixels lit in all five, 0.00 %**. That is no longer a boot failure; it is
that the retailOS does not draw where our console looks, and the reason is
further below.

`navtest.sh` against `assets/nav-baseline`: **no regression**. The bar still
moves down (12 312 and 12 114 px of difference per wheel step), SELECT still
enters (31 597 px), and compared with the saved reference the screenshots differ
by a couple of hundred pixels (the status bar clock) except the last one,
which comes out identical. *(The exact clock figure is not an invariant; see the
note at the end of the section "The retailOS draws, and can be navigated".)*

### The four walls of this session

| # | Wall | How it was found | Fix |
|---|---|---|---|
| 1 | the task slept forever after requesting the DMA | already narrowed down | `pp502x-dma`, **both** blocks |
| 2 | `Invalid write at addr 0x0, size 1, region 'ipod-video.bcm2722'` | QEMU's own log, as soon as the DMA started moving data | the BCM port accepts single-byte accesses |
| 3 | loops on bit 0 of `0x10000C00` and on `BCMA_COMMAND` | 7.9 million reads of `0x10000c00` in `-d guest_errors` | the chip answers as if its firmware were running |
| 4 | data abort at `0xa0bd0` reading `0xEA00007A` | the firmware prints it via semihosting, and the PC came out by patching `memory_region_access_valid` to dump the guest state | publish the VideoCore interface instead of a null pointer |

#### 1. The DMA engine, and that there are two blocks

The warning we carried —"the registers it touches already show up in `-d unimp`
as `0x6000b000/20/40/60`"— **was a red herring**. Those belong to the block that
`pp5020.h` documents, and they appeared only because a shutdown routine during
boot sweeps *all* channels of *both* blocks clearing bit 31.

The Broadcom upload goes through a **second DMA block that `pp5020.h` does not
mention**: master at `0x60008000`, channels at `0x60009000`. `grep -rn
"60008000\|60009000" assets/rockbox/` gives zero results; it is documented
only by Apple's firmware, which loads that base at `0x1da198` and hangs
two channels off it.

The two blocks are identical register for register. The complete map, with the
provenance of each bit, is in
[07-pp502x-hardware.md](07-pp502x-hardware.md).

**The interrupt numbers are not a guess, and this matters** because without
the correct line the model moves the data and the task stays asleep anyway.
`0x1d98d0` registers both handlers:

```
mov r1, #27 ; r2 = 0x2680c4 (b 0x1d9c40, block B handler) ; register
mov r1, #28 ; r2 = 0x2680c0 (b 0x1d9be0, block A handler) ; register
```

27 and 28 are indices into **the firmware's own** interrupt table, not
hardware lines. The table is built at `0x1fcaa0`, entry *n* at
`object+0x208+4n`:

```
0x1fcc38   entry 26 = -1 (does not exist), entry 27 = 26
0x1fcc44   entry 28 = 27
```

That is, **block B → IRQ 26** and **block A → IRQ 27**. That index 27 lands
exactly on the `DMA_IRQ = 26` that Rockbox documents for the block it does
know is the check that makes the pair reliable. And the running machine
agrees: `CPU_INT_EN_STAT` (`0x60004020`) reads `0xcc002017`, with lines 26 and
27 enabled and 28 not.

One thing is **inferred** and it has to be said: that the peripheral address is a
port and not a range, i.e. that only `RAM_ADDR` advances. No source says so;
it is the only reading compatible with the two real uses, Rockbox's I²S FIFO
and Apple's Broadcom data port.

And one that is **not** modeled: the peripheral request lines. A channel with
`WAIT_REQ` set is waiting for a peripheral to request data, and there is
none here to request it, so it is left idle. That is exactly what keeps
Rockbox's audio path behaving the same as when these registers were a
`create_unimplemented_device`.

#### 2. The Broadcom port accepts single bytes

As soon as the DMA started moving data, all four transfers failed on the
first access. The descriptor the retailOS programs for the upload leaves the
access-width field at zero, and `pp5020.h:699` spells zero as 8 bits.
This was checked by reading the register at runtime: `0x6000901c = 0x02000000`,
field 30:28 empty.

So the retailOS uploads its video firmware **byte by byte**. The BCM model
had `valid.min_access_size = 2` and rejected them. It now accepts 1, 2 and 4, and
stores exactly what it is given: the content that ends up in the chip's SRAM is
the same however it is written, because the port advances its pointer by the
access size.

#### 3. The chip's answers, and where each one comes from

None of them is a number planted to pass the check. All three are
pinned down by the code that queries them:

| Answer | What pins it down |
|---|---|
| bit 0 of `0x10000C00` = 1 | both drivers test **that bit and no other** (`tst r0,#1` on an `ldrh` at `0x287880`, `& 1` at `lcd-video.c:589`) and then write zero over it and never read it again. The rest of the word is free |
| `BCMA_COMMAND` = **1**, exactly | three checks box it in: `bcm_load` spins until it is nonzero (`0x2878b4`), its caller treats it as a signed integer and fails if it is negative (`0x1affc8`), and the routine that looks for the interface does `cmp r0,#1 / bne <give up>` at `0x288080`. Not a single degree of freedom remains |
| `BCMA_STATUS` = interface base | `0x288088` reads it, `0x288090` rejects a null one and `0x28809c` an unaligned one |

The VideoCore boot is an **edge, not a level**: writing bit 31 releases
it and nothing stops it, because the driver clears the whole register two writes
later (`lcd-video.c:591`) while the core is demonstrably running,
and at `0x28805c` it sends the bootstrap sequence over the port again,
expecting to find it still running. That second pass is what rules out
the idea that the bootstrap resets the chip.

What bits 31 and 30 of `0xC0000000` mean separately, and what the low
halfword of the doorbell `0xA5A50002` selects, remain **undetermined**. The
model fires on bit 31 and on the `0xA5A5` magic respectively, which is
all the drivers allow us to deduce.

Three more things that only Apple's firmware reveals:

- **A command counts only if the write port pointed at the mailbox.** A
  200 KB upload walks its pointer over `0x1F8` without that being a
  command — and on the real chip it could not be one either, because what passes
  over it is the VideoCore image itself while the VideoCore is stopped.
- **The two control words are written as two halfwords**, so the
  effect has to be decided on the assembled word and not on a single access.
  Acting on the first one would have seen `0x0000` where `0xC0000000` was going.
- **The primary and alternate ports are separate engines**, with their own
  pointers. Rockbox only uses the primary; the retailOS uses both at once.

And a model error that Rockbox could never have revealed: on finishing a
command, zero was written to `BCMA_STATUS`, a register that **no guest reads**
— and precisely the one the retailOS uses to get the interface pointer.
Now `BCMA_COMMAND` is overwritten, which is what the real chip does and what
both drivers watch (`lcd-video.c:204-206` and `:500-506`). As a side effect,
Rockbox stops repainting via the *timeout* path and switches to its normal path.

#### 4. The null pointer that the retailOS does not tolerate

Publishing "there is no interface" with a null pointer seemed the honest thing to do: it is the
encoding that the driver itself defines (`0x288090: cmp r1,#0 ; beq <error>`).
**It does not work**, and the failure is instructive.

`0x288058` does return an error. But **its only caller, `0xcff6c`, does not look at the
return value**: it calls the four channel hooks and ends with
`0xcff8c: b 0x28827c`, which unlocks a mutex that is only created on the
success path (`0x2880dc`). With the null pointer that mutex does not exist, `0x28827c` does
`ldr r0,[r0]` on address 0 —which after the remap is DRAM, so it does not
even fail there— picks up `0xEA00007A`, the first word of the retailOS
image itself, and dies dereferencing it at `0xa0bd0`.

What is published now is the whole structure **with every byte set to zero**,
which the driver reads as "the interface is there and has no channels": `0x2871d8` loads
a channel's offset and `0x2871e0` skips it if it is zero. That is the driver's own
spelling for an absent channel, and it is the honest thing because we do not run the
VideoCore and there would be nobody behind a channel.

**Marked as ASSUMED in the code**: where that structure is placed
(`0x00180000` of the chip memory). The real address is chosen by the `vmcs`
firmware, which we do not run; nothing depends on the number, only on it being inside the
chip memory, 4-aligned, and far from both the uploaded image (0 to
`0x312a0`) and Rockbox's framebuffer (`0xE0000`).

### How wall 4 was located, which is a reusable method

Semihosting gave the abort registers but not the PC. The trick: **temporarily
patch `memory_region_access_valid()` in `system/memory.c`** so that,
in addition to the "Invalid read at addr", it calls `cpu_dump_state(current_cpu, stderr, 0)`.
That prints the complete guest state at the instant of the rejected access:

```
R00=ea00007a R13=108c6f24 R14=002893f0 R15=000a0bd0
```

`R15` is the PC of the fault and `R14` the return address, which gives the caller
in one go. Free cross-check: the firmware's abort handler
printed `R14 : 000A0BD8`, which is exactly `PC + 8` — the ARM's `R14_abt`.
The patch is reverted; it can be reapplied in ten seconds when needed.

### Where it stops now, precisely

The retailOS is **alive and working**: BCM state at `0x108cbec8` = 3
("VideoCore threads running"), the interface mutex created at
`[0x10831978]`, the published base at `[0x10831980] = 0x00180000`, and the
channel offset table mirrored at `0x108d3bd4` with all eight at zero, which is what we
gave it.

**The next wall is the VideoCore host interface, and it is of a different kind.**
It is not a missing bit: it is that on this device **the UI is painted by the video
chip**, not by the PP502x. The evidence, all negative and all verified:

- `findlit.py` does not find **a single** instruction that loads `0x000E0000`
  (`BCMA_CMDPARAM`, where Rockbox writes the pixels) in seven megabytes.
- Nor any that loads `0xFFFF0000` (`BCMCMD_LCD_UPDATE`) from the BCM
  driver. The only write to `BCMA_COMMAND` in the entire image is the
  bootstrap's, with value 0.
- The PP502x's own LCD controller (`0x70003000`) has **a single
  reference** in the image, in the boot's peripheral shutdown routine
  (`0x1328`). There is no driver.

What there is instead: four ring channels over the chip memory, and the
VideoCore **pulling** from host memory. `0xf9ca0` (the `VCHR` task) serves
requests and dispatches by ASCII opcode: `'H'` (`0xf9ff0`) flat
host→BCM copy, `'Y'` (`0xfa110`) 2D blit with 16.16 fixed-point scaling, `']'`
(`0xfa1b4`) line-by-line scan. **In all three cases the source address in
host memory comes in the VideoCore's message**, the PP does not choose it.

So to see pixels one would have to build a "fake VideoCore" that speaks the
ring protocol *and* requests the data. The first part can be derived from the consumer;
the second requires knowing which addresses to request.

### The most narrowed-down lead we leave

The previous analysis considered the writer of `[0x1083198C]`, the
callback that translates a line index to a host address, not located. **It is located.**
`findlit.py` on `1083198c` gives exactly one instruction, `0x1c29c4`, and it is
a C++ static initializer with the ARM ABI's `tst r0,#1` guard:

```
1c299c: ldr r0,[0x108315c8] ; ldr r0,[r0,#4] ; tst r0,#1 ; bne fin   <- guard
1c29c0: ldr r0, =0x001c2a04     ; the callback
1c29c4: ldr r1, =0x1083198c     ; the slot
1c29cc: str r0, [r1]            ; *** this is where it is registered ***
```

and the callback `0x1c2a04` is not a computation, it is **a lookup in a table of
per-line addresses** hanging off `[0x108315c8 + 8]`:

```
1c2a10: ldr r4,[r0,#8]      ; the container; if null returns 0
1c2a28: bl 0x29e034         ; iterator: first entry
1c2a3c: bl 0x29e034         ; last entry
1c2a54: bl 0x29e034         ; entry for the requested line, then clamped
1c2a80: ldr r0,[r0]         ; *** the host address of that line ***
```

What fills `[0x108315c8 + 8]` is `0x1c2a90`, and it is only called from two places,
`0x1c7c30` and `0x1c7cd4`, both inside the same routine — and that routine sends
ASCII commands whose strings are twelve bytes further down: `"transitions"`,
`"push"`, `"alphawipe"`. That is, **the line callback is only for
screen transitions**, and it is registered just before and cleared (`mov r1,#0 ; b
0x1c2a90`) just after.

With that, the next step is narrowed down to a concrete, answerable question:
**setting a breakpoint at `0x1c7c30` with the gdbstub and reading the container
it passes in `r8`** gives, in one go, where the UI framebuffer lives in the
host DRAM and with what line stride. With that one can decide whether it is worth
building the ring or whether it is enough to point the QEMU console at that memory.
What will not be learned that way is the format of the `'H'`/`'Y'` messages, which has to be
extracted from the jump table at `0xf9e2c` (30 entries, five identified).

**Warning about that breakpoint**: today it is not reached. `0x1c299c` is a
lazy static initializer and `[0x1083198C]` is still zero at runtime,
just like `[0x108315c8]`. The transition routine only runs when there is a screen,
and there is none. So first the UI needs to start, and for that
the channel is needed.

### Two things seen by dumping the DRAM, which change the picture

With `pmemsave 0x10000000 0x4000000` at 55 s and searching for strings:

**The four VideoCore tasks exist and run.** `VCIH`, `VCFS`, `VCHR` and
`VCPD` appear not only as literals of the image but also **copied to the heap**
(`0x13e5d9cc`, `0x13e5ef2c`, `0x13e5fd78`, `0x13e60e58`…), which is where the
scheduler stores the name of each task. The retailOS made it to the end of
`0x2892d4`.

**And the one that did not run is the display one.** `display_control` and `transitions`
appear **only at image addresses** (`0x10163c40`, `0x101c7cd8`); there is
not a single formatted copy on the heap. That is, `0x164e9c` never got to
compose its `display_control %d model=%d power=%d`. Without a channel there is nobody
to send it to, and without that the UI does not start. It is indirect evidence —a stack
buffer could have been overwritten— but it points in the same direction as everything else.

**The `vmcs.bin` we upload is the real one.** This was not known. The buffer that
the DMA reads (`0x13ea136c`, `0x312a0` bytes) contains, at its offset `0xbfe8`, the
symbol table of the VideoCore firmware:

```
tasks, version, commands, power_down, set_vll_dir, led_control, disk_notify,
usb_inserted, audio_enable, inuse_notify, power_control, audio_control,
motor_control, camera_control, display_control, end_application,
power_management, load_application, vmcs_display_clear, vmcs_display_enable
```

plus **response** templates —`value=%d`, `drive=%i inuse=%i`,
`error=%d error_msg="bad argument"`— that only make sense on the
chip side. And it comes from `assets/ipod-video-disk.img`, file offset `0xb27508`, which
is the firmware partition; the same content is in
`assets/ipod-video-fw.bin` at the same offset. It is **not** in `osos_full.bin`.

In other words: the emulated machine already has the real VideoCore firmware, reads it from
disk, and uploads it whole and correct to the modeled chip's SRAM. The only thing it does not
do is execute it. That turns "emulating the VideoCore" from an abstract idea into an
option with material: the binary is there, with symbols, and the vocabulary of
commands the host sends it is in plain sight.

## The retailOS draws, and can be navigated (2026-08-09)

**Apple's interface is on screen and responds to the wheel.** Without executing
a single VideoCore instruction: what has been modeled is its *protocol*.

| Frame | What is seen |
|---|---|
| 10 s onward | the first-boot language screen: `Language`, `English`, `日本語`, `Čeština`, `Dansk`, `Deutsch`, `Español`, `Français`, `Ελληνικά`, `Italiano` |
| SELECT | the **iPod main menu** in the chosen language: Music / Photos / Videos / Extras / Settings / Shuffle Songs |
| wheel | the blue bar moves down from *Music* to *Videos*, 15 360 px of difference |
| SELECT | enters *Videos*: Video Playlists / Movies / Music Videos / Video Podcasts / Video Settings, 18 731 px |
| MENU | goes back to the main menu with *Videos* still selected, another 18 731 px |

Screenshots in `assets/apple/`, reproducible with
`tools/ipod_video/appletest.sh`. The six from boot (10, 20, 40, 60, 90 and
120 s) give **75 267 of 76 800 pixels lit, 98.00 %, and 307 colors**,
identical to each other — the screen gets painted and stays.

`navtest.sh` against `assets/nav-baseline`: **no regression**. Six frames
differ by a couple of hundred pixels (the status bar clock) and the
seventh by **0**, byte-for-byte identical.

> **Corrected on 2026-08-09, by running it.** This used to say that the seventh
> frame differs by 1 590 px "because of the recording screen counters".
> Measured today: the seventh is *Now Playing*, has no clock, and comes out **byte-for-byte
> identical** to the baseline. The very message of commit `5b5af8966` already
> said "0 in the seventh".
>
> And about the other six: **the exact figure is not an invariant**. In this
> document it appears as 120 and as 225–227, in commit `5b5af8966` as 201,
> and a run on 2026-08-09 gave 266. All of them are the clock, and they change with the
> digits that change between one screenshot and the next. What is stable —and the only thing that
> needs checking— is **the shape**: six frames with a couple of hundred
> pixels of difference and the seventh at zero.

### The finding that overturns the previous diagnosis

The previous session concluded that the UI is painted by the VideoCore *pulling* from
host memory, and that therefore a fake VideoCore was needed that would also
know which addresses to request. **The first half is true and the second is not.**

That path —the `'H'`, `'Y'`, `']'` opcodes of the VCHR task, and the line
callback of `[0x1083198C]`— exists, but it is the one for the **slideshow and
transitions**: `[0x1083198C]` is only filled by `0x1c2a90`, and `0x1c2a90` is only
called by `0x1c7c30` and `0x1c7cd4`, both inside the transitions routine.

The normal UI goes the opposite way, and **it is the host that pushes**:

1. `0x1649ac` asks the VideoCore to create **two** surfaces for it (double buffering on
   the chip, a single pixel buffer on the host).
2. Each one is an **opcode 8** message over the type 2 channel carrying
   format, width, height and stride, and a chip address of zero, which in this
   protocol means *"you choose it"* (`0x286d50` stores the one returned in the
   response only if the caller passed zero).
3. Since we are the VideoCore, **we choose the address**.
4. `0x164524` uploads the whole surface through the **alternate port** and `0x165024`
   then uploads only the dirty band.
5. **Opcode 3** presents: handle at `+12`, width at `+24` and height at `+26`.

That is: there is no need to guess where Apple's framebuffer is, or to request anything.
As soon as we answer opcode 8, the retailOS puts its interface inside the
modeled chip's memory, and the QEMU console only has to point there.

Independent confirmation, and very useful for future sessions: the retailOS was already
composing its entire UI in host DRAM **before** all of this. The
static descriptor at `0x10883634` places it at `0x10885db0`, 320×240, RGB565, stride 640, and
a dump of that memory at 75 s already showed the complete language screen.
What was missing was not the drawing: it was the delivery.

### The four walls of this session

| # | Wall | How it was found | Fix |
|---|---|---|---|
| 1 | the eight channel offsets at zero, i.e. "there are no channels" | it was already narrowed down | publish real channels, types 1 and 2 |
| 2 | with no channel, each ASCII command is discarded **unformatted** | `0xe9380` returns −1 before the `vsnprintf` at `0xe9394` | the above |
| 3 | no interrupt wakes the VideoCore dispatcher | `[0x1081eca8]` is zero for the whole run and no instruction writes it | **not needed**: see below |
| 4 | the wheel did not move anything, with Rockbox working on the same machine | Apple's interrupt dispatcher, `0x277128` | drive `HI_IRQ`, line 30 of the low bank |

#### 1 and 2. The channels

The whole protocol —sixteen-byte header with magic `0xF1A55A1F`, two
rings per channel, four pointers each on its own sixteen-byte
line, and the doorbell, which is the halfword `0x0031` to `BCM_CONTROL`— is
documented field by field, with the instruction that sets each one, in
[07-pp502x-hardware.md](07-pp502x-hardware.md). The model is in
`hw/arm/ipod_video/ipod_video_vc.c`.

What is published is two channels: **type 1** (text commands) and **type 2**
(binary graphics RPC). Types 5, 6 and 7 are left **absent on purpose**:
they are the direction in which the VideoCore speaks first, and that is exactly what cannot
be modeled (wall 3). Leaving them at zero is the driver's own spelling
for a channel that does not exist, and `0xcff6c` does not look at a single return value.

The text responses come from `vmcs.bin` itself: the twenty command names
it registers with `gencmd_register` are in a contiguous block at offset
`0xBE24`, and the response templates at `0xBFF8` (`value=%d`) and from `0xC004`
onward (`error=%d error_msg="…"`). A registered command receives **the empty
string**, which is the honest thing for a *setter* and the only thing that cannot be a
wrong number; any other receives the firmware's own "not registered"
template. The only hard requirement is that the payload is **not** of length
zero: `0x2872f0` returns success only if at least one byte came back.

The five commands that the boot sends, as the model sees them:

```
set_vll_dir /Resources/VideoCore/Library
display_control 0 freeze=0
display_control 0 model=1 power=-2
power_management set_policy manual 27
pm_set_policy min
```

**Correction to the previous note**: the roadmap said that `0x164e9c` "never got
to compose its `display_control`". That was correct, but not for the reason thought.
The call is made; what happens is that `0xe9358` checks the channel at
`0xe9374` and aborts at `0xe9380`, **before** the `vsnprintf` at `0xe9394`, so
the formatted string never comes to exist on the heap. The indirect evidence from the
DRAM dump was good; the cause attributed to it was not.

#### 3. The interrupt that was not needed

The dispatcher that would wake a sleeping task, `0x288100`, only runs from
the VCIH task (`0x1239b8`), which waits for an event at `[0x1081eca8]`. `findlit.py`
gives **a single** instruction that loads that literal, and it is a read; a
write *watchpoint* during the entire boot only catches the *scatter-load*
`memset`. Without the BCM interrupt line —which is still **not
identified**— that task never wakes up.

And it turns out that for the drawing path **it does not matter**. All the
retailOS requests are synchronous within a single task: `0x28861c` to send and
`0x2883d4` right after to read. And `0x2883d4` only sleeps if the ring
is empty — `0x2883f8` returns as soon as it has what it asked for, and before waiting
`0x288418` refreshes the write pointer from the chip memory. So
**if the response is already in place when the guest looks, `0x2870dc` is never
reached**. The model acts on the guest's own `str` of the write pointer,
which happens earlier, so it always gets there first.

That turns "the BCM IRQ needs to be identified" from a structural blocker into
bounded debt: it is only needed for channels 5 and 6, i.e. for the VideoCore
file system and for transitions.

#### 4. `HI_IRQ`: the wall that only Apple could reveal

With the screen painted, the wheel did not move anything — and Rockbox could be navigated fully
on the same machine and with the same wheel model. The cause was not in the
wheel.

Rockbox's `pp5020.h` lists `HI_IRQ` as source **30** of the low bank, between
`DMA_IRQ` 26 and `GPIO0_IRQ` 32, and then **never reads it**: its handler goes
straight to `CPU_HI_INT_STAT`. With only Rockbox in front of you, leaving that line
undriven looks correct.

The retailOS dispatcher, `0x277128`, does the opposite: it reads both status
words and refuses to look at the high bank if the low one does not carry bit 30.

```
277134:  ldr r4, [0x60004000]     ; low bank
277138:  ldr r5, [0x60004100]     ; high bank
2771a4:  tst r4, #0x40000000      ; anything up there?
2771a8:  beq  <spurious>
2771dc:  tst r5, #0x100           ; the I2C line, i.e. the click wheel
2771e4:  bl 0x281350              ; the click wheel handler
```

So **every press was taken and thrown away by the guest itself**. The fix
is one line of logic in the controller: the high bank drives line 30 of the
low one.

As a side effect it was verified that the retailOS uses **the same wheel packet
format** as Rockbox (`0x281374: cmp ip, #0x8000001a`), which is an
independent confirmation of something that until now only one source stated. The
details are in [07-pp502x-hardware.md](07-pp502x-hardware.md).

### What is chosen rather than derived, marked as such

- **Where the interface lives** in the chip's memory (`0x00180000`), where the
  control blocks and rings go within it, and **where the surfaces go**
  (`0x00200000` onwards). The real address would be chosen by the `vmcs`
  firmware, which we do not run; nothing depends on the number.
- **That every registered command succeeds.** We are not the VideoCore and we
  cannot fail for its reasons.
- **Result code 0** in the response header, and the zero returned by
  opcodes 1, 2, 4, 9 and 16 of the graphics channel. None of the
  callers branch on them.
- The chip's memory goes from 2 to **4 MB** so the surfaces fit. The
  real BCM2722 has its own SDRAM for that; how much, undetermined.

### What is still missing

- **The BCM interrupt line.** Without it there is no channel 5 (VideoCore
  file system) and no channel 6 (VCHR), and therefore no transitions,
  no slideshow and no video playback.
- **Type 7 channels** and eighteen of the thirty VCHR opcodes.
- **The `+0x00` field** of the channel control block and the `u16 +0x0e` of the
  message header: always zero, nobody reads them.
- What the firmware really answers to each `display_control`. It doesn't matter
  today — `0x164eec` only looks at the return value— but it is an assumption.
- Audio, and the COP.

## Strategy (iPod classic 6G)

> **Superseded, and by a path other than this one.** The strategy below took the
> `osos` to be out of reach and proposed booting Rockbox first. What happened was
> something else: on August 9, 2026 **the device itself decrypted its firmware**
> with its AES engine over USB (`b71d16f3d`,
> [10-classic6g-emcore.md](10-classic6g-emcore.md)), so the retailOS went from
> being the speculative milestone H7 to being the first thing that booted — and
> **Rockbox on the S5L8702 was never tried**. The paragraph below is kept
> because its second half is still true and was exactly what happened:
> Rockbox's drivers were the executable specification against which
> each peripheral was validated.

Apple's `osos` is encrypted with the SoC's GID key
(see [01-firmware-ipsw.md](01-firmware-ipsw.md)), so it is not a reasonable
initial target. **The first target is booting Rockbox**, which is open
source, builds unsigned, has register-level drivers for the whole
S5L8702 and loads at `0x08000000` — exactly like the `osos`.

Side benefit: Rockbox's drivers are the executable specification
against which to validate each peripheral we model.

> And there was a third source the strategy did not foresee and that turned out
> to be indispensable: **the disassembly of the retailOS itself**. The blocks that
> no external source names —the compositor at `0x38900000`, the generator at
> `0x3C100000`, the LCD frame engine registers— could only be
> modeled by reading Apple's code. Rockbox says what the hardware is like; Apple
> says which parts of that hardware really have to work.

## Milestones

> **These milestones are for the iPod classic 6G (S5L8702)**, which was the
> original target.
>
> **Corrected on August 10, 2026.** This used to say that "H3 onwards are
> stalled… because the Classic's firmware is encrypted and there is nothing real
> to boot". Not anymore. **H3, H4 and H5 are done, and H7 —the milestone marked
> as speculative— too, albeit by another route**: there was no need to model the
> AES engine or dump the bootrom, because the device decrypted it. What remains
> undone is **H2b (booting Rockbox on this machine, which was never attempted)**
> and **H6 (audio)**. The success criteria below are kept exactly as they were
> written, with a note on what really happened in each one.

### H0 — Infrastructure ✅
- [x] Clone `devos50/qemu-ios`, create branch `ipod_classic_6g`
- [x] Build the baseline (`arm-softmmu`) in WSL
- [x] Firmware parsing tool (`tools/ipod_classic/fwtool.py`)
- [x] Extract `osos` / `aupd` from the `.ipsw`
- [x] S5L8702 memory and IRQ map documented

### H1 — Minimal machine that executes instructions ✅
- [x] `include/hw/arm/ipod_classic.h` with the S5L8702 base table
- [x] `hw/arm/ipod_classic.c`: ARM926EJ-S CPU, SDRAM `0x08000000` (64 MB),
      IRAM `0x22000000` (256 KB)
- [x] Entries in `hw/arm/meson.build`, `hw/arm/Kconfig` and
      `configs/devices/arm-softmmu/default.mak` (`CONFIG_IPOD_CLASSIC_6G`)
- [x] `-kernel`-style boot path: load a binary at `0x08000000` and
      put the PC there (skip the bootrom, which we don't have)
- [x] The 32 not-yet-modeled peripherals registered as
      `create_unimplemented_device()`, so that `-d unimp` says what the guest touches
- [x] Check with a trivial ARM `.bin` (`tools/ipod_classic/mktestbin.py`)

**Result:** `-M iPod-Classic` appears in `-M help` and the test binary
writes `S5L8702 alive` to the serial console. That validates in one go: initial
PC at `0x08000000`, SDRAM mapped, and UART0 at the correct address.

```
$ ./qemu-system-arm -M iPod-Classic -kernel hello.bin \
      -display none -monitor none -serial stdio -d unimp
S5L8702 alive
```

### H2 — Serial console and interrupts ✅
- [x] VIC0/VIC1 (PL192) at `0x38E00000` / `0x38E01000`
- [x] Clock/PLL at `0x3C500000` (model reused from the iPod Touch)
- [x] UART0 at `0x3CC00000` (exynos4210_uart, IRQ 24)
- [x] Timers at `0x3C700000` — complete block written from scratch
      (`hw/arm/ipod_classic/ipod_classic_timer.c`)
- [x] IRAM0 alias at address `0x0`, which is where the ARM926 takes its
      exceptions from
- [x] Interrupt path verified end to end

**Result:** the test `tools/ipod_classic/tests/timer_irq.c` configures timer
B at 10 kHz exactly like Rockbox, installs a vector table at `0x0`,
unmasks IRQ 8 in VIC0 and counts interrupts:

```
S5L8702 timer test
counter runs: yes 0x00000001 0x00000002
waiting for interrupts...
got 0x00000005 timer interrupts
TIMER IRQ OK
```

The `-d unimp,guest_errors` log comes out empty: no access to an unmodeled
peripheral, no out-of-place address.

#### The timer block

Reconstructed from `timer-s5l8702.c`, `kernel-s5l8702.c` and the comment
on `TSTAT` in `s5l87xx.h`:

- 8 channels. **The 16-bit A–D share IRQ 8**; **the 32-bit E–H share
  IRQ 7**.
- The interrupt status of A–D lives **inside `CON[18:16]`**, with the
  enables in `CON[14:12]` — which is why Rockbox writes `TBCON = TBCON` to
  acknowledge, and why it checks `TACON & (TACON >> 4) & 0x7000`.
- That of E–H lives in `TSTAT` (`0x118`), three bits per channel: E at 24, F at 16,
  G at 8, H at 0. It is cleared by writing ones.
- Clock selector in `CON[10:8]`: /2, /4, /16, /64, or direct for E–H.
  `CON[6]` chooses ECLK (12 MHz, the crystal) versus PCLK.
- **PCLK is not modeled**: the clock block is a register file with no
  logic, so we don't know which PLL the guest asked for. The two timers Rockbox
  uses choose ECLK, so for now PCLK is treated as ECLK and a warning is issued
  via `-d unimp` if any guest really selects it.
- Only interval mode is emulated, which is the only one Rockbox uses. The others
  are reported as unimplemented instead of silently doing something wrong.

#### IRAM0 at address 0

Rockbox's `crt0.S` says it unambiguously:

> *we do not want to execute from 0x0 as iram will be mapped there*
> *copy vector section to start of IRAM0 (it will be mapped to 0x0)*

So IRAM0 also appears at `0x0`, which is where the ARM926 takes its
exceptions from. It is modeled as an alias. **What we don't know yet** is whether the
bootrom occupies that address first and the alias only appears after a remap, as
on the S5L8720. Until there is a bootrom dump that clears it up, if a
bootrom is passed, it gets the address.

### H2b — Boot Rockbox ❌ never attempted
- [ ] Toolchain: `apt install gcc-arm-none-eabi` works for our tests, but
      Rockbox wants `arm-elf-eabi-gcc` from `tools/rockboxdev.sh`
- [ ] Build the Rockbox bootloader for ipod6g
- [ ] Launch it with `-kernel` and follow `-d unimp` as a task list

**Success criterion:** see Rockbox's debug output over serial.

> **Still open, and now cheap.** When the `osos` was decrypted, this milestone
> stopped being an intermediate step and was left undone. It would be worth it
> anyway: Rockbox and the retailOS exercise **disjoint sets** of the hardware — that
> is the lesson of the 5G's bit 26 — so booting it would find holes that
> Apple's firmware does not touch. For example, Rockbox **does** push the frame via
> DMAC0 to `LCD_WDATA` (`lcd-s5l8702.c:172`), a path this model
> has never exercised because Apple uses the compositor.

### H3 — Display ✅ (via another block)
- [x] LCD controller at `0x38300000` — it is an MPU/8080-style command port,
      not a scanning engine
- [x] 320×240 RGB565 scanout to `ui/console`
- [ ] Windows at `0x39200000` (IRQ 14) — **the retailOS barely touches them**

**Success criterion:** the Rockbox logo or menu appears in the SDL window.

> **Met with the retailOS, not with Rockbox, and the piece was not the one listed
> here.** The pixels do not come from the LCD block nor from `0x39200000`: they come
> from **`0x38900000`, a compositor that no external source names** and that this
> plan could not foresee. The LCD block only sets the window and presents.
> Measured: **75 463 of 76 800 pixels, 404 colors**, and the image looked at. See
> [11](11-classic6g-retailos.md) §13.

### H4 — Input ✅
- [x] Click wheel at `0x3C200000` (IRQ 23), mapped to keyboard
- [x] GPIO — one pin, 86, which is the one the interface expects

**Success criterion:** navigate the Rockbox menu.

> **Met with the retailOS.** The wheel moves the highlight (**8 000 pixels per
> turn over a noise floor of 0**) and the center button enters the next
> screen, in the language that was highlighted.
> `sh tools/ipod_classic/navtest.sh` checks it and decides.
>
> **And MENU now goes back**, since §18 of [11](11-classic6g-retailos.md):
> it was not the input, it was the compositor, which kept drawing a layer the
> guest had turned off and that covered the entire interface. Measured and **looked
> at**: from the main menu, Enter goes into Music and MENU returns **the same
> frame** it left from (0 px of difference in the good run). When the interface
> was uncovered the figures changed: entering with Enter is now
> **~19 340 px**, not the 50 540 from when the dead layer was on top, and a
> turn in the main menu moves **the list and the panel at the same time**.
>
> **What is missing:** the guest drops one SELECT in every three or four and it is
> not known why; the suspect is in §18.8. See also §19.
>
> And half the work was not the wheel: the retailOS key dispatcher
> checks the **PMU's hold switch** and was silently dropping all buttons.
> The turn does not go through there, so the failure looked like the wheel's.

### H5 — Storage ✅
- [x] ATA controller at `0x38700000` (IRQ 29), with **DMA**, backed by a
      disk image file
- [ ] Alternative/complement: CE-ATA over SDCI at `0x38b00000` (IRQ 44) — not
      needed, the retailOS uses parallel ATA

**Success criterion:** Rockbox mounts the disk's FAT32 and plays a file.

> **Met with the retailOS**, and the part that was hard was not the ATA: its two
> DMA reads completed from the start. What was missing was **DMAC0**,
> a PL080 that QEMU already shipped, without which a later `memcpy` slept
> forever on a semaphore with no *timeout*. The retailOS mounts the `rsrc` partition
> as volume 4 and reads its fonts from `\Resources\Fonts\`. See
> [11](11-classic6g-retailos.md) §14.

### H6 — Audio ❌
- [ ] I²S at `0x3CA00000` + CS42L55 codec over I²C

> Still not done, on either machine. **Nothing makes a sound.**

### H7 (speculative) — Apple's OS ✅ (and not as planned)
- [ ] ~~Model the AES engine at `0x38c00000` with the S5L8702 GID key~~
- [ ] ~~S5L8702 bootrom (a dump is needed)~~
- [x] **Decrypt and boot `osos`**

This milestone depends on material we don't have right now. It will be reconsidered
when we get there.

> **And it reconsidered itself.** Neither of the first two items was needed:
> - **The AES does not have to be modeled**, because the one decrypting is the real device.
>   The `osos` comes out in the clear over USB in 1.2 s with emCORE (`b71d16f3d`).
> - **The bootrom does not have to be dumped**, and moreover **it can't be**: after
>   boot it returns one constant per 16 KiB bank
>   ([10](10-classic6g-emcore.md) §4). Instead the machine reproduces by hand
>   the parameter block it left at the top of IRAM1 — three fields, and not
>   one more.
>
> It is the milestone that was marked as speculative and it was the first one to be
> met. The chronicle is in [12-history-6g.md](12-history-6g.md).

## Current status (August 10, 2026)

> **Corrected twice.** First it said *"H0 complete, starting H1"*, which
> had gone untouched for about 50 commits. Then it said that the Classic *"does not boot
> real firmware and will not as long as the `osos` remains encrypted"*, and that stopped
> being true on the afternoon of August 9. This final line of the document is the place
> that has fallen behind most often: if you touch something, start here.

**iPod Video 5G (`iPod-Video`, PP5022C)** — is far ahead of what
milestones H0–H7 describe, which are those of the Classic:

- **Rockbox** boots to the menu and can be navigated with the wheel.
- **Apple's retailOS** boots, draws its interface and can be navigated: language
  screen → main menu → *Videos* submenu → back.
- What is missing: **audio** (neither I²S nor codec), **USB**, and actually running the
  VideoCore firmware, of which only the protocol is modeled today. And **the wheel
  does not move the highlight on the language screen**, measured and not diagnosed.

**iPod classic 6G (`iPod-Classic`, S5L8702)** — **boots Apple's original
retailOS and can be navigated.** H0, H1, H2, H3, H4, H5 and H7 done; H2b (Rockbox) was never
attempted and H6 (audio) is still not done.

- Fourteen peripheral blocks with real behavior (plus the PMU behind
  the I²C), listed in [02-s5l8702-hardware.md](02-s5l8702-hardware.md). The
  other 24 are `create_unimplemented_device()`.
- The language selection screen: **75 463 of 76 800 pixels, 404 colors**,
  between 22 and 30 s of wall-clock time. `sh tools/ipod_classic/navtest.sh` boots
  it, moves it with the wheel and **decides**.
- **It really navigates**: the language screen leads to the main menu, a
  turn there moves **the list and the right panel at the same time**, Enter goes into
  Music and **MENU returns to the exact frame** it left from. The two items
  that were listed here as pending —"MENU does not go back" and "the panel does not change"—
  were **a single bug of ours and not of the `TimerEventManager`**: the compositor
  was drawing a layer the guest had turned off. See
  [11](11-classic6g-retailos.md) §18.
- **What is missing, in order**: (1) the guest **drops one SELECT in every three or
  four**, with no cause found (§18.8, §19.6); (2) `TICNTH`/`TICNTL` of the
  timer block are not decoded and read zero — harmless for
  the retailOS, deadly for anyone spinning on `FIVE_USEC_TIMER` (§19.3); (3)
  audio, USB, NAND and CE-ATA.
- **Without a disk there is no image**, and that is on the guest: there is no font
  inside the firmware image.

See also [08-s5l8702-external-sources.md](08-s5l8702-external-sources.md):
`freemyipod/qemu@s5l8702-dev` has an S5L8702 model with **more blocks**
than ours (AES, SHA, JPEG, NAND, SPI, watchdog, USB). Two differences
worth keeping in mind before comparing them: their machine **boots from a
bootrom**, which is what cannot be dumped here
([10](10-classic6g-emcore.md) §4), and their device list **does not include
`0x38900000`**, which is the block the pixels come from in this tree.

The complete chronicle of how we got here, with the hypotheses that
turned out to be false, is in [12-history-6g.md](12-history-6g.md).
