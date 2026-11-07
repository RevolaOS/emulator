# Technical history of the iPod classic 6G port

A chronology of every obstacle that had to be removed so that an emulated
iPod classic 6G could boot Apple's original retailOS and reach a screen that
can be navigated, **including the hypotheses that turned out to be false**.
As in [09-history-5g.md](09-history-5g.md), the mistakes take up as much room
here as the successes, and on purpose.

The other documents say **what** the hardware **is like**
([02](02-s5l8702-hardware.md), [08](08-s5l8702-external-sources.md)), **what
the firmware does** ([11](11-classic6g-retailos.md)) and **how it was gotten
out of the device** ([10](10-classic6g-emcore.md)). This one says **how it was
figured out**, and what it cost.

The chapter that has most earned its place is in the middle, not at the end:
**[three times a ruled-out lead turned out to be the right one](#three-times-a-ruled-out-lead-turned-out-to-be-the-right-one)**.

## How to read this

Every claim carries the hash of the commit where it was made and, when the
result was fixed in the tree, the file. Three confidence levels, always
distinguished:

- **Verified in the tree**: read in the current code of
  `hw/arm/ipod_classic/` and `include/hw/arm/ipod_classic/`, or executed.
- **Reproduced in audit**: someone who did not write it measured it again
  from scratch. This is the level of the screen figures (75 463 pixels, 8 000
  per turn) and of the disassembly of the key addresses.
- **Commit message only**: counts of translated blocks and addresses from the
  disassembly that have not been rechecked one by one. They are marked
  *(commit only)*.

`assets/` is outside git — the firmware is not ours to redistribute —, so
there are no screenshots here: there are **measured figures and the command
that regenerates them**.

## The working days

27 commits touch the Classic on top of the upstream merge `328815631`, from
August 6 to 10, 2026, interleaved with those of the iPod Video 5G. The count
is obtained like this:

```bash
git log --oneline 328815631..HEAD -- hw/arm/ipod_classic include/hw/arm/ipod_classic \
    tools/ipod_classic wiki/0{1,2,3,8}-*.md wiki/1{0,1}-*.md | wc -l
```

| Date | Commits | What was achieved |
|---|---|---|
| 06-Aug 14:04–14:30 | `994f52fde`, `2ddc18b2a` | `iPod-Classic` machine: CPU, RAM, VICs, timers and an interrupt path verified with a bare-metal test. And the wall: the `osos` is encrypted |
| 08-Aug 19:47 | `3f5cd7280`, `e17924eab` | Two corrections that save work: a QEMU model of the S5L8702 already exists, and the map in [02](02-s5l8702-hardware.md) let in bases from another chip |
| 09-Aug 15:31 | `b71d16f3d` | **The foundational blocker falls**: the `osos` in the clear, decrypted by the device itself |
| 09-Aug 16:34–17:05 | `e7f8bfaf3`, `1eb53f6ff` | From reset to the RTXC kernel's idle loop, and from there to the disk |
| 09-Aug 18:20–19:36 | `8edf7fcdd`, `b675f32cc`, `88343aa0a`, `b9c2b38a7`, `c54ac2011` | Three spins with no way out, the no-disk crash cycle, and ATA with DMA reading for real |
| 09-Aug 22:24–23:15 | `02c6f3327`, `8395ac19d`, `b5026710f`, `70c29550a`, `61c92adf4`, `4bf80dacd` | The panel, GPIO pin 86, `gdbpoke.py`, and the audit that retracts a discarded lead |
| 10-Aug 01:01–01:49 | `3e67e5bbc`, `85450446c`, `03421c85d` | **There is an image**: the language selection screen |
| 10-Aug 03:26–03:58 | `8ecd4e673`, `c33634b69`, `59cf47aff` | **It navigates**: the wheel moves the highlight and the center button enters |

---

## Phase 0 — A machine that executes, and a firmware that cannot be read

`994f52fde` sets up the minimum: ARM926EJ-S, SDRAM at `0x08000000` (64 MB),
IRAM0 and IRAM1 at `0x22000000` and `0x22020000`, VIC0/VIC1 (QEMU's stock
PL192s) at `0x38E00000`/`0x38E01000`, UART0 via `exynos4210_uart`, and **32
peripherals registered as `create_unimplemented_device()`** so that `-d unimp`
says which one the guest goes to next. The map comes from preprocessing
Rockbox's `firmware/export/s5l87xx.h` for `CONFIG_CPU == S5L8702`.

`2ddc18b2a` writes the entire timer block and verifies an interrupt end to end
with a bare-metal test (`tools/ipod_classic/tests/`): timer B at 10 kHz like
Rockbox, vector table at `0x0`, IRQ 8 unmasked in VIC0. The
`-d unimp,guest_errors` log comes out empty.

And that is where the easy part ends. The `osos` in the `.ipsw` is encrypted
with the SoC's GID key. The measurement that settles it is from `d9cf852ab`,
the same one that decided to move the project to the iPod Video 5G: **entropy
of 7.99 bits/byte and zero readable ASCII strings**, versus 5.2–6.3 bits/byte
and 21 016 strings in the 5G firmware.

> **Lesson, inherited from the 5G**: the per-byte entropy of a dump is the
> five-second test of whether you will be able to work with that firmware.
> Before planning anything, measure it.

### Two early corrections, both of negative cost

`3f5cd7280`: **a QEMU model of the S5L8702 already exists**, in
`freemyipod/qemu` branch `s5l8702-dev`, descended from the same
`devos50/qemu-ios`. The wiki said ATA, wheel and LCD had to be written from
scratch. (In the end **they were written from scratch anyway**, from Apple's
disassembly and Rockbox's drivers; whether that fork was ever consulted is not
recorded in any commit. The notice remains in
[03-reuse-qemu-ios.md](03-reuse-qemu-ios.md).)

`e17924eab`: the map in [02](02-s5l8702-hardware.md) had been derived by
preprocessing `s5l87xx.h` and **bases that only exist on S5L8700/8701 slipped
in** (`SPI0 0x3CD00000`, `ADC`, `RTC`, `FMC`, `USB host/function`, `INT_BASE`,
`MS_BASE`), and it called **"Mikey"** the *click wheel*, when it is actually
the microphone and remote of the headphone jack, on I²C0 at `0x72`. The
corrections live in [08](08-s5l8702-external-sources.md).

---

## Phase 1 — The foundational blocker: let the device itself decrypt

`b71d16f3d`, and it is the commit on which everything else hangs.

**The idea that unblocks it is not cryptographic, it is logistical**: the GID
key is fused into the silicon and cannot be extracted, but there is no need to
extract it — it is enough to **ask the device to decrypt**. A physical iPod
classic 6G running emCORE exposes its AES engine over USB. The entire `osos`
comes out in the clear in **1.2 s**.

Three obstacles along the way, all in [10](10-classic6g-emcore.md):

- **Neither of the two official clients works.** The HEAD one only accepts
  `ff/00/00` interfaces (this device is `ff/ff/ff`) and uses EP0, a protocol
  later than r891; the r859 one calls pyusb with the `interface` parameter,
  which disappeared in pyusb 1.0.0b1. Hence
  `tools/ipod_classic/emcore_s5l8702.py`, which speaks the r859 protocol
  directly and only implements read commands plus just enough for the AES.
- **The detail needed to chunk large images**: `hwkeyaes()` resets the IV to
  zero at the start of **every call**, so the first block of each chunk comes
  out wrong. In CBC that is repaired exactly with an XOR against the last
  ciphertext block of the previous chunk. Checked against the device and with
  an offline test (`tools/ipod_classic/test_emcore_s5l8702.py`).
- **The bootrom cannot be read.** A negative finding, and it saves time for
  whoever comes next: after boot, `0x20000000` returns one constant per 16 KiB
  bank. It is locked and there is no dump this way.

Verification that the plaintext is what we say it is: coherent ARM vectors,
`"Copyright 2001-2008 Apple Inc."`, the literal interface (*Cover Flow*, *Now
Playing*), `RTXC v3.2b`, and Apple's build paths inside the `aupd`.
**42 014 ASCII strings versus 0.**

> **Lesson**: when the secret lives in the silicon, the useful question is not
> "how do I get the key out?" but "who else already has access to it?". The
> answer was the device itself, sitting on the table.

---

## Phase 2 — From reset to the idle loop: four things, and one of them was an `abort()`

`e7f8bfaf3`. Each change removes exactly one thing that stopped the `osos`
before it reached the RTXC kernel.

**A clock model of its own.** The machine had borrowed
`hw/arm/ipod_touch_clock.c`, which is the S5L8900's map. The two coincide up to
`0x4c` **by chance**, but the S5L8702 has `SWRCON` at `0x50`, `RSTSR` at
`0x54` and `PLLMOD2` at `0x60`, and the borrowed model answered those three
with `hw_error()` — which is not a log, it is an `abort()` of the whole
process. The `osos` writes all three.

**The bootrom parameter block.** `0x0827a904` is the complete accessor:
"return `0x2203ff00 + N`", the last 256 bytes of IRAM1; `0x08323aec` is its
only caller, magic `'IsyS'` at `+0x18`, pointer at `+0x1c`, `memcpy` of
`0x120` bytes. Without that block the firmware **did not crash: it rebooted**.
`0x083286d4` reads the hardware id and `0x0827a188` requires
`(id >> 16) == 19`; with anything else the firmware writes `"softupdt"` at
`0x2203ff00` and pulls `SWRCON`. That was the write of `0xaa5` that was being
seen and not understood.

And the `19` is not a guess: **it is what the `HwVr` entry of the SysCfg block
in this same device's NOR says**, at offset `0x5c` of
`assets/classic6g/bootflash_nor_1MB.bin`. Three fields are written and not one
more (hardware id, DRAM base and size); the other eight that the firmware
reads are left at zero, because a plausible invention cannot be told apart
from data.

**Semihosting on by default**: 131 sites with `svc 0x123456` in the image,
115 preceded by `mov r0,#4` (`SYS_WRITE0`), plus RTXC's `RTXCbug` with its
prompt at `0x0827bd79`. The same one-line change that turned around the 5G
port.

**An I/O floor over the 768 MB of `0x38000000..0x3FFFFFFF`**, beneath the
named blocks. The firmware maps that whole window as an uncached identity
section in the page table it builds at `0x22004534`, so a register nobody has
listed **is not a harmless zero: it is a data abort**, taken by a handler that
also faults.

**Traced result**: the image relocates itself, turns on the MMU, runs `main`,
brings up the RTXC kernel, unmasks interrupts and settles into the
`mcr p15,0,rX,c7,c0,4` loop at `0x22003b1c`, with **6 528 timer interrupts in
20 s**. It stops there because a task is blocked on an I²C transfer to the
PMU at `0xe6` that nobody answers.

---

## Phase 3 — I²C and the PMU: two registers that look like one

`1eb53f6ff`. `0x08325a20` writes `IICCON`, `IICDS` and `IICSTAT = 0xf0` and
waits for RTXC event 61 in a loop with no way out; only its interrupt handler
`0x0832566c` posts that event.

What makes this block not the S3C24xx it resembles is stated by Rockbox in
`s5l87xx.h`, and Apple's driver agrees **on every point**, which is what makes
it safe:

- `IICCON[13:8]` enable interrupts; `IICSTA2[13:8]` **read the status and
  clear it on write**.
- `IICCON[4]` does not clear interrupts, and in master mode the next byte
  starts when it is written as 1.

That is: **flow control and interrupt status live in different registers**,
and a model that puts them in the same one hangs the guest. Apple's handler
acknowledges with `IICSTAT2 |= 0x100` and resumes with the same
`IICCON = IICCON` idiom that Rockbox writes in `i2c_wr_internal()`.

**A byte takes 25 virtual µs, not zero.** Both drivers write `IICDS`, then
`IICCON` to start the byte, and **only then** clear the previous interrupt in
`IICSTAT2`. Completing the byte synchronously makes that clear erase the
interrupt the write had just raised.

The PMU is a **PCF50635 at `0xe6`**, with a register pointer that survives a
STOP — and that is not an assumption: both drivers send the register number in
one transaction and read in the next.

**An inference, and it is critical enough to have a property name**: the
retailOS **never enables its own I²C interrupt**. `0x08325908` is the only
code that writes `IICCON = 0x184`, and it is only reached when `0x220003b8`
reports the bus stuck for 100 ms, that is, as bus recovery. All the other
writes preserve what was already there. The driver **inherits the enable from
whoever ran before**, and the only thing that runs before is the bootrom,
which has to talk to this same PMU to find out why the machine woke up. So the
machine hands over the block with `IICCON = 0x184`. Marked `INFERRED`: there
is no bootrom dump, and [10](10-classic6g-emcore.md) says this SoC is not
going to give it up.

**Result**: the retailOS gets past the PMU, reads the panel *strap* from
`PDAT(6)`, configures GPIO, brings up the click wheel at `0x3C200000`,
programs the external interrupt controller and starts the ATA. Now it spins on
`ATA + 0x78` waiting for a disk that is not there.

---

## Phase 4 — Three spins with no way out, and a crash cycle that was invisible

`8edf7fcdd`. Three blocks, and **none was a wrong value: all three were loops
with no way out**.

**`0x3C100000`, the block no source names.** Rockbox's only `0x3C100000` is
`LCD_BASE` under `#if CONFIG_CPU==S5L8700` — another chip —, and freemyipod
has nothing there. The entire specification is Apple's code, and it is small
enough to quote in full: searching for the literal and then **all** the
PC-relative `LDR`s that resolve to one of those words yields **exactly three
sites**, so the list is complete and not "whatever turned up". Seed at `+8`, a
zero at `+0`, and `while (!([+0] & 7)) ; return [+4]`. Wrapped in a critical
section that pulls twenty words and discards nineteen: that is seeding a
generator and throwing away its first outputs, and nothing else fits. Identity
`INFERRED`, three-register contract **verified**.

And the consequence that has to be said out loud: **there is no enable bit**.
The only write to `+0` is a zero, and it happens **before** the twenty reads.
A model that makes "data ready" conditional on the guest having enabled the
block hangs on the first extraction.

**The LCD command port at `0x38300000`.** Rockbox and the retailOS spin on the
same three status bits, which is what makes them reliable: bit 1 idle, bit 4
busy, bit 0 read data valid; `s5l87xx.h` notes the reset value as `0x0106`.
`lcd-s5l8702.c:207` and the retailOS's `0x080b6f98` are the same loop
instruction for instruction. With the window unimplemented, the
`while (!(LCD_STATUS & 0x2))` loops never return.

**The ATA *task file* at `0x38700000`**, PIO only, so that the storage probe
finishes.

### And the finding: the firmware had been saying it all along

Without a disk the machine **was not sitting idle: it was crashing and
rebooting**, nineteen times every twenty seconds — 19 *Data Aborts*, 19
semihosting calls, 19 `SWRCON` writes. It was invisible because the
diagnostic goes out on **stderr** and the traces were throwing it away:

```
CPU_exception_handler
warning: S5L8702: guest asked for a software reset (SWRCON = 0xaa5), next image "--------"
```

The cause is in `DFSR 0x5 DFAR 0xEA00002A`. **`0xEA00002A` is not an address:
it is an ARM branch instruction.** And it is `0xEA000002 + 0x28`, where
`0xEA000002` is exactly the word that `main` writes to `[0x22000000]`. What
aborts is a pair of C++ jumps — vtable load and method `+0x28` — that is,
**a virtual call on a null object**.

The interesting part, and it is specific to this machine: since the firmware
itself maps VA 0 onto IRAM0, **the null dereference does not fault**. It
calmly reads a word from the vector table and blows up on the *second*, two
hundred bytes later and somewhere else.

> **Lesson, and it is number 1 for the whole project**: if the firmware prints
> something, listen to it. Semihosting was already on and even so a while was
> spent looking at `-d unimp` histograms instead of reading the two lines that
> explained everything.

> **Reusable diagnostic lesson**: a `DFAR` that looks like an ARM instruction
> (`0xEAxxxxxx`) is the signature of a null pointer on a machine where VA 0 is
> mapped.

---

## Phase 5 — ATA with DMA, and the order that shapes the model

`b675f32cc`. The storage task was blocked on a transfer that could not finish:
it programmed `ATA_XFR_NUM` and waited for the interrupt of an engine that
stored its descriptors and moved nothing.

The protocol is **specified twice and both agree**: Rockbox writes it out in
full in `storage_ata-6g.c:851-880`, and Apple's driver was traced doing the
same register by register. That is what makes the bit meanings safe and not
merely plausible.

**The order is what shapes the model, and it is the opposite of the intuitive
one.** The buffer address is programmed **after** the ATA command, so
`start_dma` cannot know where the data goes: it can only hold on to the
*callback* the IDE core gives it, and it is the write of the start bit that
triggers. (The 5G's PP502x needs the same split for the mirror-image reason:
there the address goes first and the start bit still goes last.)

Two traps that cost an afternoon each, both already paid for on the 5G and
repeated here:

- **`s->bus.dma` has to be claimed after `ide_bus_init_output_irq`**, not
  before: the last thing that function does is set
  `bus->dma = &ide_dma_nop`. Claiming it before makes every DMA command be
  accepted and silently ignored, which looks a great deal like a disk that
  does not answer.
- **Checking DMA by the absence of complaints is not good enough.** It is
  checked by reading guest memory: after the first transfer,
  `0x0be5d3e0 + 0x1fe` contains `55 aa`, the MBR signature of sector 0 of the
  image, at the byte where it belongs.

`ATATRACE=1` dumps every register access. It is how the guest's sequence was
read in the first place, and it is cheap to keep.

And the measurement that reordered the whole investigation: **the firmware
partition parser never runs**, with a real partition on the disk or without
it. Neither it nor the interface code that draws. Something upstream of both
is waiting.

> **Nuance that came later** (`b9c2b38a7`): that measurement was made with
> `grep -c '^0x08057160'` on a `-d in_asm` log, and **`0x08057160` is not a
> block entry**, it is an `add r0,sp,#4` in the middle of the function. It
> would have given zero even if the code ran on every boot. The real entry is
> `0x0805712c` (`push {r1..fp,lr}`) and there the conclusion holds — but the
> conclusion was correct by another route, not by the measurement. See the
> `-d in_asm` trap in [11](11-classic6g-retailos.md), §7.

---

## Phase 6 — The panel, a pin, and a debugger

`02c6f3327`. Two devices and a tool, all three driven by measurement.

**The panel.** `ipod_classic_lcd.c` accepted commands and pixel words and
threw them away, so nothing this machine did could appear on a
screen. Now it carries a 320×240 RGB565 memory, a window via `0x2A`/`0x2B`,
a write pointer after `0x2C`, and a `qemu_console`. **No figure comes from
a datasheet**: the 8-bit command set is proven by the `LCD_CON = 0x80000c21`
the guest writes and by the only branch of `0x080b6f8c` that can produce it; the
geometry, by the `0x2A`/`0x2B` parameters and the `(320<<16)|240` at `+0x74`.

And it is validated with a **control case**, because an untested panel is not
a measurement: `tools/ipod_classic/lcdtest.S` emits that sequence and pushes 76 800
words. `screendump` + `pixcount.py` return **76 800 of 76 800 pixels and
1920 colors**, which is, to the pixel, what the test writes.

**GPIO pin 86.** `0x080aeca4` is literally `gpio_get_pin(86)`, and the
storage task turns it directly into "mount the volume" or "no
medium". With `0x3CF00000` unimplemented it read zero, so **every boot
with a disk told the volume manager there was no medium**. Now the pin
follows whether there is a `-drive`.

**`gdbpoke.py`**, and this changes the pace of work. `-d in_asm` only answers
"was a block starting here translated?"; the question that was needed was "what
is in `r0` when the guest reaches this address?". This WSL does not have
`gdb-multiarch`, so there is a 200-line GDB remote protocol client
that talks to QEMU's `-gdb tcp::PORT`.

> **A trap that cost an hour.** QEMU does **not** have "ignore the
> breakpoint I just left" logic: `accel/tcg/cpu-exec.c:346` always fires
> when `pc == bp->pc`. A bare `c` from a stop stops again at the same
> place **forever**, and the first version of the tool produced eight
> identical stops from a function with 374 callers without any alarm
> going off. The way out is three lines further up in the same file:
> *"Singlestep overrides breakpoints"*.
>
> **And the control case that exposed it**: put the breakpoint on a function
> with many callers and check whether `lr` changes between stops. If it does not, the
> tool is broken, not the firmware.

### The fundamental correction: there are no fonts inside the image

With the debugger, the diskless crash loop is traced to its root, and **it is not
what the wiki said**. The lookup of the `"FONT"` resource **succeeds**
(`r0 = 0x086aef48`), so does the one for `"Str "`, and the record it returns, read
from the guest, is `0dad0001 0000003a 01d58101 0dad0002` — name id, body 58,
style. The provider chain works.

The null appears when **instantiating the font**: `0x082393ec` looks for it in two lists
that are only filled by `0x08239220`, and that function **enumerates the
`"Resources/Fonts"` directory of a mounted volume** (`0x08239134` is a real directory
iterator: it skips entries starting with `.` and calls
`0x08107184` for each name). Without a volume the lists are empty, no font
matches, and the caller invokes a virtual method on `{0,0}`.

And it cannot be bypassed, which settles it: with `gdbpoke.py --skip` that
call is skipped without touching Apple's image and **the boot crashes again**, now with
`DFAR 0xEA00003E` — another virtual method, slot `+0x3C`, on another null of the
same origin.

Measured after all this: the retailOS **still gives 0 pixels**, with a disk and
without one. The black screen goes from being a shrug to being a
number.

---

## Phase 7 — The audit that puts a suspect back on the list

`70c29550a`. Everything is re-measured from scratch and, besides confirming the big things,
it produces four corrections. The one that matters is the first, and it has its own chapter
further down:

**`0x38900000` had been ruled out** with the test "no register
receives anything that looks like a DRAM address". The test was **true**, and it did not
mean what it seemed to. In a diskless boot that block receives **793
distinct addresses**, and two of them are unmistakable:

| Register | Value | What it is |
|---|---|---|
| `+0x400`…`+0xFFC` | `value == offset − 0x400` | **768 words = 3 × 256: an identity gamma table**. Only a video path writes that |
| `+0x214` | `0x013f00ef` | **the panel geometry to the pixel**, `(319<<16)|239` |

And fourteen of the eighteen `0x38900000` literals in the image live at
`0x08127xxx`, the same function that writes `LCD[0x74]` and `LCD[0x70]`. Ruling-out
withdrawn.

In the same audit, and eight instructions away from code that was already
documented: `0x08127f14` loads `[0x089115e4]`, compares it with zero and calls its
`+0x0c` method with `(0,0,0,320,240)` **before** starting the frame engine.
That pointer is `0x08911fe0` with the machine running — it is not null — and its
`+0x0c` is `0x080c1164`, the function this tree had already identified as
Rockbox's `displaylcd_setup()`. **So the retailOS does request a full-screen
draw**; that call only sets the window, and the pixels were never going
to come through the CPU.

---

## Phase 8 — The image: two things were needed at once

`3e67e5bbc`. Three things had to be true at the same time, and the first two
were already known.

### 1. `0x38900000` is the compositor

Nothing outside Apple's code names this block, so the entire register map
in `ipod_classic_disp.h` comes from disassembling **the twenty-seven places
that load the `0x38900000` literal**, checked line by line against a
boot. Two methods, and both are needed:

1. **The twenty-seven literals** give almost the whole map, and the list is complete
   by construction, not "whatever showed up".
2. **`DISPTRACE=1` with the guest PC** finds what step 1 cannot:
   `+0x200..+0x214` are written from `0x081273f8..0x08127428`, which reaches the
   block via `r6` and not via a literal of its own.

Honest readback matters by itself, and there is a before-and-after measurement: **six
functions do read-modify-write on `+0x008`**, and while the
block read zero each one erased what the previous one had set. Before:
`+0x008` received `0x01110001`, then `0x00008000`, then `0x00000100`, then
`0x40000000`, each landing on a zero. After: `0x01118001`,
accumulated, which is what those four calls should add up to.

The pixels come from **the five layer descriptors** at `+0x058 + k*0x18`,
each with stride, format, source address, size and position. A
real boot programs two: a 320×240 32-bit surface at `0x09767a24`
and a 108×240 16-bit panel at x=212 — and `212 + 108 = 320` to the pixel.

**And the image was there all along**: `pmemsave 0x09767a24 0x4b000` and
viewing it as 320×240 at 32 bits gives the whole language screen, already composited.
Nothing had to be invented; it just had to be copied to the panel.

Two details that bite:

- **`W` and `H` are signed.** While the right-hand panel slides off the
  screen the firmware writes `+0x07c = 0xffc900f0`, i.e. width −55. Reading them
  as unsigned draws 65 481 columns of garbage.
- **The byte order is the only `INFERRED` part of the path to pixels.** The
  32-bit layer is read as *little-endian* ARGB8888 and the 16-bit one as RGB565, because
  Rockbox's `config/ipod6g.h:85` says `LCD_PIXELFORMAT RGB565` for this
  device, because only that way do the two layers match, and because then the selection
  bar comes out `#0484da` — Apple's blue — and not `#da8404`, an orange
  that appears nowhere in this interface. It is an argument about Apple's
  colors, not a datasheet, and it is marked as such.

### 2. DMAC0 and DMAC1 are PL080s, and QEMU already shipped the model

**This is what unsticks the boot with a disk, and it corrects phase 5.** The ATA
was never the problem: its two DMA reads finish and its semaphore wakes
whoever was waiting. What does not come back is a **later** `memcpy`, via DMAC0 channel
0, which sleeps on the channel semaphore with `KS_wait` **without a timeout**. The only
thing that can raise it is DMAC0's terminal-count interrupt, `0x220027b4`,
registered at `0x080357f4` on **IRQ 16**. And `0x38200000` was
`create_unimplemented_device`.

It was also doing silent damage: `0x22004b68` does a read-modify-write
on `C0Control`, and with the block returning zero the final write came out as
`0x0c000000` instead of carrying the `I` bit, the widths and the count. **A model
that only generated the interrupt without returning the reads correctly would still
fail.**

Two independent sources agree: Rockbox names the two PL080s
(`dma-s5l8702.h:33-36`) and puts them on IRQ 16 and 17 (`s5l87xx.h:1893`,
`#define IRQ_DMAC(d) (16+(d))`), and the offsets the retailOS uses
match byte for byte with `hw/dma/pl080.h`.

A wiring detail that is not optional: **the master port has to be system
memory**, because the guest programs the channels with the cached alias
(`0x0be55320`) while the CPU runs on the uncached one
(`0x8be55320`).

### 3. And with that, the whole chain

`-d in_asm` with a disk, counting only verified function prologues. **The
nine addresses gave zero in every previous trace of the project**:

| Address | What it is | Before | Now |
|---|---|---|---|
| `0x080aee04` | MBR parser | 0 | **1** |
| `0x0808f8f4` | locate the firmware partition | 0 | **1** |
| `0x0805712c` | **the `]ih[` parser** | 0 | **1** |
| `0x08040cf8` | mount `rsrc` as volume 4 | 0 | **1** |
| `0x08122a04` | raise the storage flag | 0 | **1** |
| `0x081228d0` | the **exit** of `while (![obj+0x28]) wait();` | 0 | **1** |
| `0x08239134` | iterate `Resources/Fonts` | 0 | **1** |
| `0x08239658` | build the string object | 0 | **1** |
| `0x0815b1b8` | the interface code that draws text | 0 | **1** |

**Result**: `screendump` + `pixcount.py` go from 0 to **75 463 non-black
pixels out of 76 800, in 404 colors**, and the image is the iPod's language
selection screen — **looked at, not just counted**: "Language" bar with battery and
lock, nine different scripts (Japanese, Greek, Czech…), a blue selection
bar over *English*, and the *Welcome / Bienvenue / Willkommen* panel.

The independent proof that the fonts come from the disk is that **there is
text in nine scripts**: those fonts only exist in `\Resources\Fonts\` of the
`rsrc` volume.

### The missing control experiment

`03421c85d`. That the compositor was needed is obvious — it is the only thing that
draws. That the PL080s were **also** needed was an inference over a long
chain. It was checked by removing them:

> With the compositor in place and both PL080s removed, a 45 s boot with a disk:
> **0 pixels, 0 colors, and not a single `SOURCE ADDRESS` line.** The
> compositor is there and nobody ever gives it a source address.

(The patch was a temporary `getenv`; it was reverted and is not in the tree.) With
that, phase 8 stops being a nice story and becomes a cause.

---

## Phase 9 — The wheel: two traps, and only one was the wheel's

`8ecd4e673`, `c33634b69`, `59cf47aff`. The screen could be seen and not used.
`0x3C200000` was `create_unimplemented_device`.

The whole model (block, IRQ 23 and keyboard handler) is in
`ipod_classic_wheel.c`, and its header lists every line of the register map with
the Apple instruction that establishes it. What deserves to be in the history are
the three decisions.

**First trap: `WHEELMASK` has to be read back honestly.** The firmware
saves it, sets it to zero around each polled transaction and restores it.
While the block was `create_unimplemented_device` every read gave zero, so
**what it restored was zero**, and the interrupt stayed masked
forever. The boot log was saying so all along: **twelve writes of `0` to
`+0x10` against a single one of `1`**. Counting accesses without looking at the values would have
hidden it.

**The design decision that is not obvious: packets are spaced out with a
timer, not queued all at once.** The rotation decoder `0x0807a670`
needs **two consecutive packets with the finger down** before computing any
delta — the first one only arms —, and the ISR overwrites `0x089116d8+0x20` on every
packet. If two arrive before the wheel task runs once, the arming packet
disappears and the movement is read as a first touch and discarded.
**A FIFO alone is not enough**: the guest needs to see each packet as its
own interrupt.

**Second trap, and it was not the wheel: the PMU hold switch.** With the
wheel in place, rotation worked and **the buttons did not**. Measured, with the key
held for three seconds and reading the firmware's own status block:
`0x089116d8+0x1C = 1` (the packet arrived and the ISR decoded it) and `+0x14 = 1`
(the task ran the edge detector and called the dispatcher). And the screen,
still.

The cause is inside the key dispatcher, at `0x0803f3e0` — **instruction
number 21** of `0x0803f390`, after the four comparisons that
remap the key code —, which ends up reading the PMU's register `0x87` (`GPIOSTAT`)
over I²C and **returns immediately if it says "locked"**. Our PMU
returned it as zero, so **every button was silently dropped**. And rotation **does not
go through that dispatcher**, which is why the bug looked like the wheel's.

Rockbox says what that bit is and arrives at the same polarity from the other side
(`pmu-6g.c:244`, `holdswitch = !(GPIOSTAT & GPIO2)`): **bit set = hold
off**. The fix is one line in `ipod_classic_pmu_reset()`, marked
as a CONVENIENCE VALUE, because a machine with no physical switch to move
has to boot with it off or it cannot be used.

> **Lesson, and it is the 5G's `HI_IRQ` again**: the model raising an
> interrupt does not mean the firmware receives it, and **checking the whole
> path means checking it for each kind of event**, not once. Rotation and
> buttons come in through the same block, share an ISR, and from the wheel
> task onward they split.

### What is seen, measured and looked at

`sh tools/ipod_classic/navtest.sh`, three runs with identical figures:

```
00-idle             75463 px lit
01-idle-again       75463 px             0 px differing   <- the noise floor
02-wheel-fwd        75432 px          8000 px differing   (8000 in the list, 0 in the panel)
03-wheel-fwd2       75463 px          8000 px differing
04-wheel-back       75432 px          8000 px differing
05-select           76800 px         50540 px differing
06-select-later     76800 px             0 px differing
07-menu             76800 px             0 px differing
```

The **8 000 pixels** are exactly what it costs to move the bar: two
160×25 boxes turning on and off. The noise floor of this screen is **0**, and
`01-idle-again` checks it **on every run before trusting anything** — if it stops
being 0, the test fails, because its whole verdict rests on that zero.

And looked at, which is the other half: `e` moves the bar from **English** to **日本語**, `q`
moves it back, and Enter **changes screen** to the musical note icon with
**「ミュージックが…」**, that is, **in the language that was highlighted**. That
coincidence is what turns "many pixels changed" into "the interface did
what it was asked".

The model's counters match the keys one by one: **14 packets** =
1 polled response at boot + 3 turns × (arm + move + finger off)
+ Enter (press and release) + `w` (press and release); and **13 edges** = 14 − 1,
because the boot packet is queued with `WHEELMASK` at zero.

### And a correction to the brief the work came from

**The guest does not poll the wheel 120 times per second.** It does **a single**
polled transaction at boot (with five retries) and from then on it is
purely interrupt-driven, IRQ 23. The thousands of `+0x14` reads that were
counted were the wait spin of that single transaction against a register that
returned zero. The conclusion at the time ("someone is already listening") was
**correct by coincidence**; the mechanism was not — and a model that only returned
data in `WHEELRX` without raising the line would not have produced a single event.

---

## Three times a ruled-out lead turned out to be the right one

This is worth more than the register list, and that is why it is in the middle of the document and
not in an appendix.

### The three cases

**1. `0x38900000`, ruled out because "no register receives anything that looks like an
address".** Withdrawn in `70c29550a`, resolved in `3e67e5bbc`: it was the
video compositor, the piece that draws. The test was true and the absence was
**a consequence of the hang being investigated**: the source address
is only written when the resource volume is mounted, and on top of that it comes from a
different place (`0x081276dc`) than the one that writes the layer geometry. The
system was measured **in the state in which it was broken** and the result was read as a
property of the hardware.

**2. The `rsrc` image from the `.ipsw`, taken to be empty for days.** It turned out to be a
**78 MB FAT16 volume** with `Resources/Fonts` inside, that is, with the
fonts without which the interface cannot compose anything. A glance at
`dev_offset` without the `0x1000` bias lands in the padding and reads as an
empty image. Corrected in `03421c85d`, documented in
[01-firmware-ipsw.md](01-firmware-ipsw.md).

**3. The ATA, blamed for the hang with a disk, was innocent.** The
diagnosis "the hole is between *the DMA finished* and *wakes whoever
requested it*" pointed at the right gap and at the wrong peripheral: the ATA completes its
whole cycle, the interrupt arrives and the semaphore wakes. What did not come back was
a **later** `memcpy` via DMAC0. Corrected in `3e67e5bbc`, §14 of
[11](11-classic6g-retailos.md).

### What they have in common

**(a) All three rest on a negative test, and a negative test only
rules something out if you know that what you are looking for would have had to show up in the state
in which you measured.** In all three cases the system was broken just upstream of
where the evidence is produced: with no mounted volume there is no source address
to write, and the `memcpy` that hangs happens *after* the ATA has
finished and gone quiet.

**(b) All three close a question with a single observation and no control
case.** The project already had the rule written down — it is lesson 2 of
[09-history-5g.md](09-history-5g.md), *"a negative experiment only rules out
the hypothesis if the experiment was well built"* — and even so.

**(c) In all three, the good evidence was in the same measurement as the
dismissal.** The 793 addresses written to `0x38900000` included a 768-word identity
gamma table and the panel geometry down to the pixel, two signatures
that only a video path has. In the `.ipsw` directory table, the
`osos` entry next to it extracted perfectly with the offset applied. And in
the ATA trace, the driver closes cleanly (`ATA_CFG = 0x640`) and **never speaks
again**, which is exactly the signature of a peripheral that has finished its
work, not of one that is stuck.

**(d) The cost grows with how late they are revisited.** The ATA dismissal cost
an entire session of `gdbpoke.py` walking down the wrong call chain;
the `rsrc` one, days of assuming the fonts had to be in some
odd place; the `0x38900000` one, the only piece that draws.

And a fourth trait that is no coincidence: **in two of the three, the true
suspect was already written in the list of candidates**. The list "to be
ruled out, in order of cost" in §6 of [11](11-classic6g-retailos.md) had
`0x38900000` as number 1 and DMAC0 as number 4 — **the last** —, and DMAC0
turned out to be the cheapest of the four to fix, because QEMU already ships
`hw/dma/pl080.c`.

### What should have been done differently

Five rules, in order of payoff:

1. **A dismissal is written with its expiry condition inside.** Not
   "`0x38900000` is not the source of the frame", but "`0x38900000` receives no
   addresses **in a diskless boot**, which is the only one we know how to bring
   this far". The sentence with the condition inside re-evaluates itself as soon as
   the state changes; the sentence without it stays in the wiki forever.

2. **Before believing a zero, check that the experiment produces a one in a
   case whose answer you already know.** This rule exists in this project and
   has a name: it is how it was discovered that `grep -c '^0xDIRECCION'` on
   `-d in_asm` only works for block entries — it was tried with `0x22003b1c`,
   which executes thousands of times, and it came out **zero**. The rule was born there and **was not
   applied** to any of the three dismissals.

3. **Positive evidence weighs more than negative evidence, and it has to be scored
   separately.** An identity gamma table and the panel geometry down to the pixel are
   *signatures*; "I do not see an address" is an *absence*. A signature that has no
   other explanation beats an absence that has several. Operational corollary, and
   it is the rule that has paid off most in this project: **count values, not
   accesses**.

4. **When the obvious suspect is also the last block you touched,
   checking the next one on the list costs a minute.** The ATA had just been
   modeled, it was being traced with `ATATRACE=1` and it was the only one talking: it was
   the available suspect, not the likely one.

5. **Dismissals live in a table, not in prose.** With a date, with the concrete
   test and with what it would take to reopen them. All three were buried
   in paragraphs, which is where a dismissal stops being revisited. The following table
   exists for that reason.

---

## Catalog of withdrawn hypotheses

Quick index, so that they are not proposed again.

| # | Claim | Status | Where |
|---|---|---|---|
| 1 | The Classic firmware is intractable without extracting the GID key from the silicon | **False**: there is no need to extract it, it is enough for the device itself to decrypt over USB | `b71d16f3d`, [10](10-classic6g-emcore.md) |
| 2 | ATA, click wheel and LCD for the S5L8702 have to be written from scratch | False: `freemyipod/qemu@s5l8702-dev` already existed. (They were written from scratch anyway, from the disassembly) | `3f5cd7280`, `e17924eab` |
| 3 | The base map in [02](02-s5l8702-hardware.md) | Legacy S5L8700/8701 bases slipped in (`SPI0`, `ADC`, `RTC`, `FMC`, `USB HC/FC`, `INT_BASE`, `MS_BASE`) | `e17924eab`, [08](08-s5l8702-external-sources.md) |
| 4 | "Mikey" is the click wheel controller | False: it is the jack microphone and remote, I²C0 `0x72` | `e17924eab` |
| 5 | `rsrc` and `hash` are both filled with `0xFF` | **False for `rsrc`**: it is a 78 MB FAT16 with the fonts. True for `hash` — and that is what made it believable | `03421c85d`, [01](01-firmware-ipsw.md) |
| 6 | The framebuffer goes via DMA, because one pixel word arrives per CPU write | False twice: it was **zero** pixels and eight bytes of command parameter, and no DMA channel ever points at `LCD_WDATA` | `02c6f3327`, `4bf80dacd` |
| 7 | `0x38900000` ruled out: "no register receives anything that looks like an address" | **True, and it did not mean that.** It is the video compositor | `70c29550a` → `3e67e5bbc` |
| 8 | The hang with a disk is in the ATA | **False**: the ATA closes its whole cycle; it is a later `memcpy` via DMAC0 with no IRQ 16 | `3e67e5bbc` |
| 9 | "The `]ih[` parser never runs", measured at `0x08057160` | The conclusion held; **the measurement was invalid**: it is not a translation block entry | `b9c2b38a7` |
| 10 | GPIO pin 86 is "the whole reason" a boot with a disk does not draw | Necessary, **not sufficient**: the announcement was later thrown away at `0x080b2ce8` | `70c29550a` |
| 11 | With a disk the LCD block is touched twice | It is **five** writes | `70c29550a` |
| 12 | The three disk images give identical 682-line ATA traces | 680 lines and one difference, and the difference is run-to-run noise in the buffer address | `70c29550a` |
| 13 | The guest polls the wheel ~120 times/s, "so someone is already listening" | Conclusion correct by chance, **false mechanism**: one polled transaction and then IRQ 23 | `8ecd4e673` |
| 14 | No layer ever overlaps, so Z order is not measured | **False**, and it was precisely the fact that supported it: layer 1 is a sweep with `x + w = 320` at every step | `03421c85d` |
| 15 | The buttons do not arrive because the wheel model fails | False: it was the PMU hold switch, which turning does not get past | `8ecd4e673` |
| 16 | `0x0803f3e0` is "the second instruction" of the key dispatcher | It is the **twenty-first**, and there is a second gate behind it at `0x0803f3ec` | `59cf47aff` |
| 17 | The eight wheel register names are Rockbox's | **Six of the eight are inventions of this port**: Rockbox only names `WHEELINT`, `WHEELRX` and `WHEELTX` | `59cf47aff` |
| 18 | The header of `ipod_classic_disp.c`: "a compositor here would be inventing pixels" | It contradicted `ipod_classic_disp_compose()`, thirty lines further down | `03421c85d` |
| 19 | The Classic's `navtest.sh` passes if the turns move the highlight | It **also** passed with the noise floor broken, which is the premise of all its thresholds. Now it fails | `59cf47aff` |

---

## Debugging techniques, by how much they paid off

1. **Read stderr.** The firmware prints its panics via semihosting, and it was
   saying `CPU_exception_handler` and `SWRCON` nineteen times every twenty
   seconds while `-d unimp` histograms were being looked at. Cost of turning it on:
   one line.
2. **`gdbpoke.py` / `gdbrun.sh`.** Answers "what is in `r0` when the guest
   gets here?", which is the question `-d in_asm` cannot answer. The whole
   call chain in §9 of [11](11-classic6g-retailos.md) and the fonts
   correction come from there. With `--skip` it skips a call **without
   touching Apple's image**, which is how it was shown that dodging the fault
   leads nowhere.
3. **`grep -c '^0xDIRECCION'` on a `-d in_asm` log**, for "did
   this run?" in one second. **It only works for block entries**: without capstone,
   `-d in_asm` prints one address per translation block. Disassemble the
   address and check that there is a prologue before believing the zero.
4. **Per-block traces with the guest PC and totals at the end**: `DISPTRACE`,
   `LCDTRACE`, `ATATRACE`, `WHEELTRACE`, `GPIOTRACE`. The rule that makes them
   useful is printing **values**, not just accesses — that is how the gamma table
   was found —, and the one that makes them reliable is that **the totals count everything**: a
   counter that compares against the last value seen instead of against those already seen does not
   count, and there was one.
5. **`pmemsave` + `ppm2png.py --raw 320x240:argb8888`.** Look at the guest
   framebuffer **before** having a path to the screen. It is what confirmed that the
   image was complete in DRAM and only needed copying.
6. **Control cases.** `lcdtest.S` for the panel (76 800/76 800 and 1920
   colors, and the ramp **looked at**); `lr` changing between stops for the
   debugger; the noise floor of 0 checked on every run of `navtest.sh`.
   An instrument that is not tested produces measurements that are not valid, and here it produced
   two.
7. **Read the guest's own state block through the monitor.** `0x089116d8`
   is where the wheel ISR leaves what it decoded: `+0x0C` and `+0x10`
   moving show that **the interface drained the event**, which is more than
   a delivered interrupt proves.
8. **`findlit.py`**: decode the PC-relative `LDR`s that load a literal,
   instead of searching for bytes. It is what makes "there are exactly three places that
   touch `0x3C100000`" a complete list and not a sample.
9. **Pixel difference between frames, with thresholds.** Moving the highlight
   one item is 8 000 pixels; a new screen, 50 540. A few pixels
   are the status bar clock and prove nothing — with the advantage that
   **this screen has no clock**, so its floor is exactly 0.

---

## Open questions

None of these is resolved. They are here so that nobody considers them closed.

**The missing milestone**

- **MENU does not go back** from the screen that follows choosing a language: 0
  pixels, measured with waits of 15 and 25 s. And the key **arrives intact**: with
  breakpoints along `0x0803f390` one can see the remapped code go through the
  dispatcher, reach `0x08170e94`, build the `'kDwn'` message and enter
  the EventManager queue. That screen **is not deaf** — it reacts to a turn
  with 50 307 pixels — and decides to do nothing with MENU or with a second Enter.
  That this is because the first-boot assistant has nowhere to go back to is
  `INFERRED`, not verified.
- **The right panel does not change when turning** (0 pixels on the right in the three
  turns), when the rule `delayedselected1 -> ShowSetting_Language` says it
  should. It points to the timed half of the interface (`TimerEventManager`,
  `0x081059c4`), and it is the most promising thread for both things.

**Things the model chooses and does not derive**

- **The byte order of the layers** (`INFERRED`, two lines of
  `ipod_classic_disp.c` if better evidence appears).
- **The wheel timing**: 40 ms is a tuning constant, not a fact about the
  device. The real timing is `UNDETERMINED`.
- **`IICCON = 0x184` inherited from the bootrom** (`INFERRED`, and there will be no
  bootrom dump over USB).
- **The PMU hold switch disabled at boot**: VALOR DE
  CONVENIENCIA, marked as such.
- **That `0x3C100000` is an RNG**: `INFERRED`. Its three-register contract,
  verified.

**What could not be checked**

- **The composition of several layers has never been seen in an image.** Layer 0
  alone already contains the whole screen; the layer 1 sweep lasts less than a
  second and no `screendump` has caught it. The 75 463 pixels prove
  slot 0 very well and **do not prove** Z order or clipping.
- **Whether `LCD +0x70` latches a frame or only enables scanning.** The model
  composites in `gfx_update`, which is a modeling decision stated out loud in
  `ipod_classic_lcd_gfx_update()`.
- **What `WHEELCON`, `WHEELPEND[1:2]`, the `0x3A980` of
  `WHEELDIV` and the `0x8000062A` command mean bit by bit**; and **why the real hardware accepts both
  `WHEELTX` encodings** (Rockbox's and Apple's). None of them is
  needed.
- **What GPIO pin 86 is on the board.** What it does in Apple's
  firmware, verified.

**What is simply not there**

- **The diskless boot** is still in its crash loop, and it is now known to be
  unfixable along that path: there is no font inside the image.
- **Audio** (I²S, CS42L55), **USB**, **NAND/FMC**, **CE-ATA over SDCI**, the
  **CalmADM2E DSP**, the **RTC** and the SoC's own **AES**: not modeled.
- **4- and 8-bit layers** are not decoded (the palette is missing) and none has
  been seen in use; **the scaler and the color matrix** are stored and not
  honored, because every measured boot writes identity.
- **The bootrom**: there is no dump and there will not be one over USB.

---

## Maintenance note

In this port a comment in the code itself has ended up lying at least
three times —`ipod_classic_disp.c` came to say the opposite of what the
function thirty lines below it did—, and the wiki has lagged behind the tree
as many times again. If you touch any of this:

- The headers of `ipod_classic.c`, `ipod_classic_disp.c` and
  `ipod_classic_lcd.c` list **what is not modeled**. That list goes stale
  with every commit that models something.
- The map in [02](02-s5l8702-hardware.md) and the milestone list in
  [04](04-roadmap.md) are the two places that fall behind the most.
- Superseded diagnostic sections **are not deleted**: they are marked with a
  *superseded* banner and a link to what replaces them. That is the value of
  these documents, and it is why §6 and §9 of
  [11](11-classic6g-retailos.md) are still there describing a state that no longer
  exists.
