# Technical history of the iPod Video 5G port

A chronology of every obstacle that had to be removed so that an emulated
iPod Video 5G could boot its two real firmwares, **including the hypotheses
that turned out to be false**. The mistakes take up as much space here as the
successes, and on purpose: they are what keeps someone from walking down the
wrong path again.

The other documents say **what** the hardware **is like**
([07](07-pp502x-hardware.md)) and **what state** the work **is in**
([04](04-roadmap.md)). This one says **how it was figured out**, and what it
cost.

> The sibling document, for the iPod classic 6G, is
> [12-history-6g.md](12-history-6g.md). Several lessons from here were paid
> for again there — the status bit that never arrives, "zero is not harmless
> unknown", and above all the `HI_IRQ`, which reappeared as the PMU hold
> switch.

## How to read this

Every claim carries the hash of the commit where it was made and, when the
result was fixed in the tree, file and line. There are two confidence levels
and I always distinguish them:

- **Verified in the tree**: read in the current code of
  `hw/arm/ipod_video/` and `include/hw/arm/ipod_video/`, or executed.
- **Commit message only**: counts of translated blocks, lit pixels and
  addresses from the disassembly of the `osos`. They are internally
  consistent and several were replicated in code comments, but **they have
  not been reproduced one by one**. They are marked *(commit only)*.

`assets/` is outside git, so the cited screenshots do not travel with the
repository; they are regenerated with `navtest.sh` and `appletest.sh`.

## The four sessions

51 commits on top of the upstream merge `328815631`, from August 6 to 9, 2026.

| Date | Commits | What was achieved |
|---|---|---|
| 06-Aug 14:04–14:30 | `994f52fde`, `2ddc18b2a` | `iPod-Classic` machine (S5L8702): timers and interrupt verified |
| 06-Aug 15:57–21:44 | `d9cf852ab` … `a38f51d07` | 5G dump, `iPod-Video` machine, the Rockbox bootloader drawing, and the first chain of black screens |
| 08-Aug 19:40–22:23 | `b3355f116` … `9baca880e` | Rockbox up to the menu and navigable; first diagnosis of the retailOS, wrong |
| 08-Aug 23:04 – 09-Aug 07:45 | `9d823ec76` … `c68ce94d9` | The retailOS: from "thinks there is no RAM" to an interface drawn and navigable |

---

## Phase 0 — The Classic, and why the project moved

The project starts on the S5L8702 (`994f52fde`): memory map derived from
Rockbox's `firmware/export/s5l87xx.h`, ARM926EJ-S, VIC0/VIC1, UART0, and 32
peripherals as `create_unimplemented_device()` so that `-d unimp` says which
one the guest is going to. Timers and a complete interrupt are verified with a
bare-metal test (`2ddc18b2a`).

The wall: **the Classic's firmware is encrypted with a key that lives in the
silicon**. `d9cf852ab` measures the difference instead of arguing about it —
entropy of 5.2–6.3 bits/byte on the 5G versus 7.99 on the Classic, and 21,016
readable ASCII strings on the 5G, including `portalplayer` and the loader's
image names (`booting!`, `diskmode`, `diskscan`, `softupdt`, `retailOS`).
That is what moves the target to the Video.

> **Lesson**: the per-byte entropy of a dump is the five-second test of
> whether you will be able to work with that firmware. Before planning
> anything, measure it.

**Later correction about the Classic** (`3f5cd7280`, `e17924eab`): the wiki
said that ATA, click wheel and LCD for the S5L8702 had to be written from
scratch. That is false: `freemyipod/qemu`, branch `s5l8702-dev`, already has a
complete model, forked from the same `devos50/qemu-ios` base. `e17924eab` also
corrects the map in `wiki/02`: it had been derived by preprocessing
`s5l87xx.h` and bases that only exist on S5L8700/8701 slipped in (SPI0, ADC,
RTC, FMC, USB host/function, `INT_BASE`, `MS_BASE`), and it called "Mikey" the
click wheel, when it is actually the jack's microphone/remote, on I2C0 at
`0x72`.

---

## Phase 1 — Getting the firmware out: two parsing bugs with the same cause

`d9cf852ab` adds `tools/ipod_video/dump-firmware.ps1` and fixes two things in
`fwtool.py`:

- The directory offset in the `]ih[` header is not relative to the buffer,
  and **what it is relative to depends on where the container comes from**:
  `0x4000` means `0x5000` in an `.ipsw` payload, but `0x4200` in a partition
  read from the disk.
- The directory carries no length, so the parser ran off the end and
  reported 81 entries before blowing up printing a four-character code that
  was not text.

`a513a1a50` finds the common cause: **all the directory offsets are biased by
0x200 bytes**, because they count from the image base (the second sector of
the partition), not from the start of the buffer.

The symptom that gave it away is the detail worth copying: Rockbox's
`bootloader/ipod.c` says the string `"portalplayer"` is at `DRAM_START+0x20`,
and it showed up at `+0x220`. With the bias corrected, the `osos` starts with
a proper ARM vector table, `rsrc` with a FAT boot sector, and the Rockbox half
with `msr cpsr_c, #0xd3`.

> **Lesson**: when you have a constant documented by a third party
> (`"portalplayer"` at `+0x20`), use it as an oracle for the parser. The 0x200
> bias explained at once the "odd" directory offset and the zeros where
> vectors should have been: **a single error, two symptoms that looked
> independent**.

### Source conflict, resolved in favor of the silicon

`d9cf852ab`. The device reports model `MA146E`, which databases list as a
30 GB 5G with 32 MB of RAM and a PP5021C. Rockbox reads **PP5022C and 64 MB**
from the silicon, and the `osos` carries `vers 0x0000b012` (`0xb0` =
"Video/5G" for `ipodpatcher`). The unit has an iFlash adapter, so the most
likely explanation is mixed parts. What the chip says rules.

That conflict is still open on one point and it is worth knowing: see
"[The DRAM size](#the-dram-size-open-question)" at the end.

---

## Phase 2 — The machine boots

`ddf21e4bc`. An architecture decision that saves a ROM dump: Apple's ROM
loader only copies the `osos` to `DRAM_START` and jumps to
`DRAM_START + entryOffset`, so **the machine does that work directly** and
exposes an `entry` property. `0x735a00` is the Rockbox bootloader, `0` Apple's
firmware; both live in the same image.

Four registers had to be real, each because the firmware visibly stopped on
it:

| Register | Why |
|---|---|
| `PROCESSOR_ID` `0x60000000` | Both cores run the same image and tell themselves apart by reading this byte (`0x55`/`0xaa`). A stub returning zero is neither of the two |
| `COP_CTL` `0x60007004` | `crt0-pp-bl.S` spins until bit 31 says the other core is asleep |
| `USEC_TIMER` `0x60005010` | Free-running microsecond counter: all Rockbox busy-waits are written against it; at zero, the first delay is an infinite loop |
| NOR flash window at 0, board revision at `+0x405c` | `hwcompat.h` reads it from there during boot and from `0x2000405c` afterwards, with the comment "ROM is remapped" — that is how it became known that **address 0 is flash, not RAM** |

The revision value, `0x000b0005`, is still at `ipod_video.h:213,220`.

`9d3b0ec76` models the BCM2722 just enough for the bootloader to draw. A
detail that is not obvious and conditions everything else: **the panel does
not hang off the SoC**, it hangs off a Broadcom chip that the PP502x reaches
through eight registers spaced 64 KB apart starting at `0x30000000`.

---

## Phase 3 — The black screen: a correct bisection with three false conclusions

The most instructive part of the history, because almost everything that was
thought was wrong and got written down.

**Symptom**: when the disk was connected (`9d6b705c7`), the screen stayed
black. Without a disk, the bootloader drew.

### Hypothesis 1 — the DRAM alias in low memory

`33e30cdb1` had aliased DRAM at low addresses because the firmware read
`0x100000` tens of thousands of times. `6ea6f90ed` suspects that this alias
corrupts the running image (`0x100000` → `0x10100000` falls inside the
`osos`). It is removed: **the screen stays black**. Ruled out — although the
alias was wrong anyway and went away.

### Hypothesis 2 — interrupt delivery

TIMER1 was already firing and there was no vector table at 0. The line is
disconnected: **still black**. Ruled out.

### Hypothesis 3 — `CACHE_CTL_VECT_REMAP`

`70d0bae2c` traces and discovers that the CPU **is not in a loop**: it goes
through `0x000fe800`, `0x000fec00`, `0x000ff000`… in steps of `0x400` until it
runs off the map at `0x100000`. It is a **NOP sled** — a word of zeros decodes
as `andeq r0, r0, r0`. The "Invalid read at 0x100000" that was being seen was
**the end of that walk, not a spin**.

The deduction: an exception is taken, there are no vectors at 0, and
`pp5020.h` says the vectors are remapped to IRAM by the cache controller at
`0x6000c000`. `2893ab659` implements it, and the commit title says it all:
*"implement the vector remap, which was not the answer"*. Execution turns out
**byte for byte identical**, 3157 translated blocks, same ending *(commit
only)*. The firmware never sets that bit on that path. It stays in the tree
because it is correct (`ipod_video.c:294-348`), but it was not the cause.

### The real cause

`90de12e29`: trace **where it enters** empty memory, instead of where it
leaves.

```
0x400000b4  0x400000b8    Rockbox bootloader, from IRAM
0x10000000                jumps to Apple's firmware
0x100001f0 .. 0x10000234  its reset path
0x0000023c                and into low memory
```

Two things at once: **(a)** the bootloader was booting **Apple's** firmware,
not Rockbox — case 1b of `bootloader/ipod.c`, which looks for
`"portalplayer"` at `DRAM_START+0x20`; and **(b)** Apple's firmware is linked
for address 0 and needs the remap unit at `0xf000f000`, which was still a
stub.

And here is the sentence you have to read if you pick this up again, from the
commit itself:

> *An earlier attempt at this aliased DRAM at 0x100000 and did not help, which
> is why the wrong conclusion was drawn from it — the alias belonged at 0.*

A badly set-up experiment ruled out the correct hypothesis for four commits.

> **Lessons**:
> 1. An upward walk in regular steps through empty memory is a **NOP sled**,
>    not a loop. The "invalid access" you see is the end; tracing backwards
>    from there is useless, you have to find **where it started**.
> 2. A negative experiment only rules out the hypothesis **if the experiment
>    was well built**. "I aliased DRAM in low memory and it didn't help" is
>    not the same as "I aliased DRAM at 0".
> 3. A generic tool instead of a one-off patch: `6702ab527` puts a 64 KB
>    `unimplemented_device` **underneath** the two register windows
>    (`0x60000000` and `0x70000000`), with what is modeled on top. The reason,
>    from the commit: *an unknown register that reads zero is survivable; an
>    unmapped one is a data abort*, and here the handler it jumps to also
>    faults and never returns. Modeling registers one at a time as they fail
>    is the wrong shape of the fix.

---

## Phase 4 — Rockbox up to the menu

`60541d2d8` groups three hangs **of the same kind**: the hardware sets a bit,
we returned zero, the firmware waits in a loop.

- **The cache arrays at `0xf0000000` are addressable memory, not
  registers.** `cache_invalidate_special()` walks the line-state array by
  hand. Flat RAM is enough.
- **`+0x28` of the device control block**: bit 7 says the USB block came out
  of reset. It is not in `pp5020.h`; it comes from `usb-fw-pp502x.c`.
- **`GPO32` bit 14 powers the Broadcom.** Apple's flash ROM loader turns it
  on before handing over control; `lcd-video.c` reads it back to decide
  whether it has to upload the BCM firmware from the ROM's `vmcs` section. At
  zero, Rockbox gave up, left `display_on` false, and `lcd_update_rect()`
  **discarded every drawing without saying anything**: everything ran,
  nothing appeared. It is still set by hand in `ipod_video.c`.

> **Lesson**: when a driver has a *"the hardware is already initialized, I do
> nothing"* path, skipping the ROM loader means inheriting the obligation to
> reproduce **everything** that loader left done. `GPO32` bit 14 was the
> first of those inheritances; the SysInfo block (phase 5) was the second.

### The method discovery: a screenshot is an instant, not a state

`ad065fbeb`. *"No partition found"* was not the end of the story, **it was
the second frame**. Taking screenshots at intervals showed that at ~8 s the
Rockbox 4.0 logo is on screen in full color. The error came from the
**application's** `disk_mount_all()`, not from the bootloader.

With `-trace "ide_*"` the two phases separate cleanly: the bootloader reads
with READ MULTIPLE (`0xc4`), the app issues READ DMA (`0xc8`) and the next
line is `ide_reset` — `ipodvideo` defines `HAVE_ATA_DMA` and QEMU's generic
mmio-ide has no DMA engine.

`e2dba1271` solves it and leaves two traps written down, both verified today
in the tree:

- **`ide_bus_init_output_irq()` ends with `bus->dma = &ide_dma_nop`**, so
  claiming the bus **before** calling it is silently undone
  (`ipod_video_ide.c:240-246`).
- **`IDE0_CFG` INTRQ is a latch, not a mirror of the interrupt line**,
  because during DMA the firmware never reads the ATA status: that bit is the
  only completion signal it receives (`ipod_video_ide.c:63,92,121-125`).

### The click wheel: the data is not enough

`a38f51d07` corrects the label of the `0x7000c000` block (from I2C to click
wheel), `b3355f116` models it with the keyboard, and `e2dba1271` discovers
the nuance: `ipod_4g_button_read()` **is only called from
`ipod_4g_button_int()`**, and does not even look at the data if bit 26 of
`0x7000c104` is zero. You have to **latch the bit and raise the interrupt**.

The packet is `0x8000001a` with the buttons as individual bits on top, 96
clicks per revolution, and Rockbox requires four clicks of accumulated travel
before moving the selection, so each key advances two and a press+release
moves exactly one item (`ipod_video.c:482-626`).

> **Lesson**: reading zero from a data register does not mean "nothing
> pressed", it means "**no packet**". It is a distinction the firmware does
> make.

---

## Phase 5 — The retailOS: five walls and a chain of retracted hypotheses

### 5.1 "It needs the second core" — proposed and retracted

`d4259dea7` traces backwards from the SVC and concludes: the assert compares
the end of the image against the available RAM, and the available RAM comes
out zero. The zero field is filled from a structure that is only written by
code guarded by `cmp r0, #0xaa` (*am I the COP?*). Conclusion: the COP needs
to be modeled.

`9d823ec76` models the COP — and **fixes nothing**. The commit does not dress
it up: *"The retailOS still does not boot, and I am not going to dress that
up."* The COP stays because it is real hardware and the model is correct (it
reads `0xaa`, goes to sleep as its first act, exactly like `crt0-pp.S`), but
it was not the cause.

`288737573` formally retracts **three** hypotheses at once, after two cheap
checks:

1. *"`0x1084` is a pointer to the ROM parameter block"* → **false**:
   `0x1084` is "give me the address of global N", it returns
   `IRAM_END + N - 0x100`. It has 13 callers and several of them write.
2. *"the COP fills the global"* → **false**: the CPU path
   (`0x25c → 0x15f8 → 0x1348 → 0x1244 → 0x15fc → 0x27eaec → 0x13bc`) does not
   write `COP_CTL` at any point. And the commit confesses: *"that was my
   inference from 0x95024 being COP-gated, not something I had verified."*
3. *"the blank flash is the blocker"* → already retracted in `d4259dea7`,
   because the firmware dies **before** looking at the flash.

> **Lesson**: an inference by proximity ("this code is guarded by the COP, so
> the COP writes it") **is not a verification**. The cheap check — *does
> anyone write `COP_CTL` on the path?* — took minutes and would have saved
> two commits.

### 5.2 The chip string: correct diagnosis, wrong mechanism

`0763f94eb` finds a real wall: `PP_VER1`/`PP_VER2` spelled `PP5022C`, taken
from how **Rockbox** renders the device on its debug screen. Apple's firmware
compares against the literals `"PP5020A "` and `"PP5022A "`; `PP5022C`
matches neither, so it fell into the "unrecognized" base and **all** its
globals read zero, including the RAM size.

**But the mechanism was misattributed**, and `57ce76fab` corrects it. The
string comparator at `0x27ea8c` exists, and its caller `0xf0c` would indeed
turn the result into a base — but **`0xf0c` is dead code in this build**: it
loads the literal from `0xf34` (`0x4001c8fc`), finds it nonzero and exits via
the `popne` at `0xf1c` without ever comparing. What really decides is
`0x1084` via `0x598`, which reads **a single character** (bits 23:16 of
`PP_VER1`, `cmp r0, #0x32`). All the reasoning is preserved in
`ipod_video.c:716-745`.

Current values, verified in the tree (`ipod_video.c:746-747`):

```c
#define PP5022C_VER1        0x32324120   /* '2' '2' 'A' ' ' */
#define PP5022C_VER2        0x50503530   /* 'P' 'P' '5' '0'  */
```

that is, **`"PP5022A "`, not `"PP5022C"`**. It is counterintuitive and that
is why it is documented here: the real device *is* a PP5022C, Rockbox calls
it that, and even so the registers have to spell the `A` form because that is
what Apple's firmware knows how to read.

> **Lesson**: a fix working **does not validate the explanation** of why it
> works. Here the correct explanation and the incorrect one produced exactly
> the same patch, so the patch did not distinguish between them.

### 5.3 The premise that held up three commits and was false

`0e7407722`, `4cbe50bcf` and `b9786a0bb` investigate under the premise *"the
firmware dies on its first system call, because it expects its exception
handlers already installed in IRAM"*.

`d06ce3123` retracts it entirely. **`0xe9b98` is not a syscall: it is the ARM
C library's panic printer.** The giveaway is in the instruction above it:

```
e9b94:  mov r0, #4          @ SYS_WRITE0
e9b98:  svc 0x123456
```

and in the string table right above: `"Type: "`, `"undefined instruction"`,
`"data abort"`, `"_sys_exit"`, `"CPU "`, `"Task"`.

With semihosting answered, **the firmware itself says what it is dying of**:
four SDRAM banks at `FFFFFFFF` (empty), and a return chain that lands exactly
in the branch that was believed never to return. The empty IRAM was a
**consequence, not a cause** — the handlers are installed by the `osos`
itself at `0x2a0`, some 150 blocks **after** the point of death.

Semihosting stays **enabled by default** in the machine, with the reasoning
written at `ipod_video.c:901-919`. It is the highest-yield change in the
whole project: it turns every future black-screen failure into a message with
a register dump.

> **Lesson, the most reusable one in the repository**: before tracing an
> unknown firmware, **check whether it is already talking to you**. An `svc`
> with an odd immediate (`0x123456` is ARM's magic) and `r0 = 4` is
> `SYS_WRITE0`, not an OS syscall. Enabling semihosting is one line.

Along the way, `b9786a0bb` corrects a method error from `4cbe50bcf`: the claim
of "139 literals of `0x40000000` in the image" was **false**, because
`0x40000000` is also the high word of the double `2.0`, and a raw byte search
finds floating-point tables and calls them pointers.
`tools/ipod_video/findlit.py` decodes every `ldr rD,[pc,#imm]` and reports
only literals that the code actually loads: **from 139 to 1**.

> **Lesson**: searching for bytes in an image gives false positives by the
> handful. If you are looking for a pointer, **decode the instructions that
> would load it**.

### 5.4 Bit 26: a wall nobody had proposed

`2b1ba27e3`. The firmware's scatter-load table (`0x84040..0x840a8`) walks
the image contiguously, and in the middle of the sweep the addresses jump by
**exactly `0x04000000`**:

```
src 0x006c9a64 + len 0x0004c734 = 0x00716198
next src                        = 0x0471619c
```

A one-bit discontinuity in a series of consecutive offsets is not the
table changing address: it is **the same address with a flag**. And the
contents settle it — file offset `0x7161a4` is `e1b0f00e`
(`movs pc, lr`), which after the copy lands at `0x40000008`, exactly where the
SWI vector literal at `0xec` points. The handlers fit to the byte as soon as
bit 26 is ignored.

The aliases are in `ipod_video.c:1000-1032`, with the constant in
`ipod_video.h:76` (`DRAM_ALIAS_BIT`). **What the bit means to the real hardware
remains INFERRED**, marked as such in the commit itself and in the header.

From the commit, the sentence that sums up the risk of trusting a single firmware:

> *Rockbox never forms a bit-26 address, which is why a machine that runs it to
> its menu could still not get the retailOS through a memcpy.*

> **Lesson**: two firmwares for the same device exercise **disjoint** sets
> of the hardware. One booting fully says almost nothing about the other.

### 5.5 The SysInfo block — the ROM revisited, and the missing nuance

`3b2637dc8`. iPodLinux's `ipodloader2/ipodhw.c` names
`SYSINFO_TAG_PP5022 0x4001ff18` and `SYSINFO_PTR_PP5022 0x4001ff1c`, and notes that
"the last 256 bytes are used by the Flash ROM for special things". Apple's
firmware matches to the byte: `0x13bc` asks for global 24, compares it against
`0x53797349` (`"IsyS"`) and takes a pointer from `+4`. **Nothing in the image writes
either of the two words** — the magic is loaded by exactly **one**
instruction in seven megabytes, and only to compare it.

That is the decision criterion worth copying:

> **A value that the firmware compares but never produces has to come from
> outside it.**

Two failures, and the second only became visible once the first was fixed:

- **The RAM size.** With a null pointer, the read of `SysInfo+0xe0` goes to
  `[0x000000e0]`, which after the remap is a word of the vector table
  itself (`0x00000100`); 256 bytes round to zero, the four banks come out
  empty, and the assert fires. The bitter detail: **the firmware already knew the
  answer** — `0x1f4` does `mov fp, #0x700`, whose nibble 2 indexes the same
  table to `0x02000000`. `0x13bc` throws it away in favor of what it reads through
  the null pointer.
- **The hardware id.** *"This is the one the earlier analysis had
  backwards."* Leaving the magic at zero **seems** harmless, because `0x13e8`
  dereferences the pointer before `0x1400` checks the magic. But
  `0x140c` only publishes the pointer at `0x4000608c` **if the magic matched**, and
  without that `0x281224` does nothing, `0x265164` reads `+0x84` of an empty bss
  object, the 13-entry jump table at `0x2653c8` answers "no such
  machine", and `0x2668e4` dereferences a null driver.

**Four fields and not one more are written** (`ipod_video.h:200-202`, written in
`ipod_video.c:1216-1229`), with explicit reasoning for why the rest is not
filled in: iPodLinux's `sysinfo_t` puts `sdram_size` at `+0x80`
whereas **this** firmware reads `+0xe0`, which falls inside a pad that iPodLinux
never decoded. From the commit:

> *Filling the struct from it would be inventing with the appearance of rigour.*

**The final nuance** (`d83fd8908`): hypothesis 2 of `288737573` —"the ROM leaves a
parameter block"— **comes back**, but not as it was stated. Both readings
were true **at the same time**, which is exactly why it was over-rejected:
`0x1084` is indeed "address of global N" **and** entries 24 and 28 of that
same area are indeed the {tag, pointer} pair that iPodLinux calls SysInfo.

> **Lesson**: rejecting a hypothesis because you refuted **one of its
> formulations** is a specific kind of error. Here it cost two calendar
> days.

### 5.6 The remap, correct by accident

`57ce76fab`. The old rule was "any write to MMAP0 puts DRAM at
address 0", and it was **correct by coincidence**. The two firmwares use
different windows (documented in `ipod_video.c:374-384`):

```
Rockbox  MMAP0  logical 0x00003e00  physical 0x10000f84   <- this one
Apple    MMAP0  logical 0x00003bf0  physical 0x00003a88
         MMAP1  logical 0x00003a00  physical 0x10000f84   <- this one
         MMAP2  logical 0x20003800  physical 0x00003f88
```

Apple's MMAP0 maps low memory to physical zero, which is **not** the remap; the
one that counts is MMAP1. It worked because the write to MMAP0 triggered the rule
early. Now the pair is decoded: logical base zero + physical base
`DRAM_MEM_BASE` (`ipod_video.c:419-425`).

The same commit fixes comments that had been left lying:
`ipod_video_intc.c` said the COP was not modeled, and the header of
`ipod_video.c` said that only one core is created and that the remap is not
modeled.

> **Lesson**: comments age and **the compiler does not check them**. In
> this project they have lied at least three times; see the maintenance note
> at the end.

### 5.7 The PMU, and a register file that was worse than an error

`62af0171e`. The retailOS stopped in `pmu_adc_read` (`0x28341c`), spinning
**without a timeout** on bit 7 of the PCF50605's ADCS2, because `0x7000c000` was a
flat register file: every transfer "worked" and returned the
last byte the firmware itself had written.

Here **the correction** of `a38f51d07` is corrected: the block was neither I2C nor
click wheel, **it was both**. I2C at `+0x00..+0xff`, click wheel from `+0x100`
upward, and they share an interrupt line. It is written in
`ipod_video.h:251-260`, with the verbatim warning: *"Which of the two owns the
window was got wrong once already, in both directions."*

Three things that only Apple's driver reveals, and that Rockbox would never have
shown:

- Bits 2:0 of STATUS are a **result code**, not padding (`0x282f44`
  masks them with 7, `0x28358c` aborts if it is not zero).
- `+0x08` is a **second address register**, for the combined write-then-read
  at `0x282e30`, whose received byte lands in DATA1, not DATA0.
- **The block has to work uninitialized**: Apple's DEV_EN/DEV_RS routine
  (`0x282e98`) is only reached when a transfer times out, so
  if STATUS never says BUSY the guest **never enables the block**.

**A side effect that turned out to be a fix**: Rockbox started booting into the
main menu instead of *Now Playing*. `rtc_check_alarm_started()` decides
that the alarm woke it if clock and alarm match within ten seconds; without an
RTC, both reads returned the same thing and Rockbox believed an alarm had gone
off **on every boot**.

> **Lesson**: a flat register file **is not a neutral stub**. It returns
> what the guest wrote, which is a *plausible* answer, and therefore worse
> than an error: there is no log, no failure, and the loop never exits.

### 5.8 Bit 31 — the uncached view

`0ee503c2e`. Data abort reading `0x93EE1B70`, which is normal DRAM
(`0x13EE1B70`) with bit 31 set. Ten buffer constructors share the
same idiom, and **the exception is what identifies the bit**:

```
cmp  r1, r0, lsr #28      ; high nibble 4, i.e. IRAM?
beq  done                 ; then leave it
orr  r0, r0, #0x80000000  ; otherwise, this is how the buffer is used
```

IRAM is the only memory on the chip that **is never cached**, and it is exactly the
one the firmware refuses to mark. Rockbox says the same thing in C with another
constant (`system-target.h:164`, `UNCACHED_ADDR`). Reasoning preserved in
`ipod_video.h:92-114`.

**How it was found**, and it is reusable: a breakpoint on the firmware's `memcpy`
via QEMU's gdb stub, asking who passes a source pointer with bit 31
set → `0x21701c`; then a **write watchpoint** on that descriptor word
→ it lands in the constructor.

---

## Phase 6 — Video: the UI lives on the other side of the chip

`433be156f` implements the bootstrap handshake (`BCM_ALT_CONTROL` bits 7 and
6). A note that explains why this did not come up earlier: **Rockbox never gets there on
this machine**, because `bcm_init()` only runs if `GPO32` bit 14 says the
chip is unpowered, and we pretend that it is powered. The retailOS brings up
the chip on every boot. That exposed two model bugs:

- **The address registers are 16-bit.** Apple loads them as two
  halfwords, low first (`0x287c28`); Rockbox always writes the whole
  word via `BCM_WR_ADDR32`. Assigning the value on each write left only the
  high half — an upload to `0x000fc000` would have gone to `0x000f0000` **without saying
  anything**. Now it is a shift register loaded from the top
  (`ipod_video_bcm.c:323-330`).
- **The data port is as wide as the access.** A halfword write
  that stored a word clobbered the next two bytes.

`fb7c1039a` models the PP502x DMA — **two identical blocks**. Rockbox's `pp5020.h`
documents only one (master `0x6000a000`, channels `0x6000b000`,
`DMA_IRQ` 26) and uses only its channel 0 for the I2S FIFO. The other (master
`0x60008000`, channels `0x60009000`) **does not appear in any public source**:
it is the one Apple uses for video. The interrupt numbers are not
guesses — `0x1d98d0` registers the handlers under the firmware's indices 27 and 28
and the table at `0x1fcaa0` maps them to lines 26 and 27; that index
27 lands exactly on Rockbox's documented `DMA_IRQ` is the cross-check,
and a running machine confirms it: `CPU_INT_EN_STAT` reads
`0xcc002017` *(commit only)*.

`689c4892d` makes the Broadcom answer as if its firmware were running. Four
things that Rockbox alone would never have shown:

- The data port has to accept **single bytes** (the DMA descriptor
  leaves the width field at zero = 8 bits).
- **A command only counts if the write port was pointing at the mailbox**: a
  200 KB upload sweeps the pointer past the chip's address `0x1F8`,
  and on the real chip the VideoCore is halted while its own
  image is loaded.
- Both control words are written as two halfwords, so the side
  effect is decided on the **assembled word**.
- The primary and alternate ports are **separate engines with their own
  pointers**; Rockbox only uses the primary, the retailOS uses both at once.

It also fixes a bug of our own: a completed command now overwrites
`BCMA_COMMAND` (which is what both drivers watch) instead of zeroing
`BCMA_STATUS`, which was precisely the register the retailOS takes its interface
pointer from.

And a case of **"publishing nothing is not an option"**: if no structure is published,
`0x288058` returns an error, its only caller ignores it, and `0xcff8c` unlocks
a mutex that the failed path never created → data abort. The structure has to be
published **with all bytes zero**, which is how the driver
spells "no channels".

### `5b5af8966` — the one that draws

The finding: **Apple's UI is not painted by the PP502x**. The retailOS hands
entire surfaces to the BCM2722 and the BCM's processor composites them. With no channels
published, every display call was abandoned before being formatted
(`0xe9380` returns -1 on every ASCII command, `0x286cf8` on every RPC), **silently**.

Two channels are implemented: type 1 (ASCII `gencmd` vocabulary, with names and
templates taken from the device's real `vmcs.bin`) and type 2 (binary display
RPC; opcode 8 creates a surface, opcode 3 presents it). No interrupt is
invented: every request is synchronous within a task, and `0x2883d4` only
sleeps on an empty ring.

Result: language screen at 320×240 RGB565, **75,267 of 76,800 pixels
lit (98.00 %)**, identical at 10, 20, 40, 60, 90 and 120 s. Reproduced in
the Aug 9 audit with `appletest.sh`.

### The last wall: `HI_IRQ`

`662d0fb83`, and it is the cleanest example in the whole project.

Rockbox's `pp5020.h` lists `HI_IRQ` as source 30 of the low bank, next to
`TIMER1_IRQ 0` and `DMA_IRQ 26`, **and then never reads it**: `system-pp502x.c:76`
onward queries `CPU_HI_INT_STAT` directly. A model that leaves
line 30 undriven **looks perfectly correct from Rockbox**.

The retailOS does the opposite: its dispatcher at `0x277128` reads both status
words and **refuses to look at the high bank if the low one does not have bit 30**.
So every click wheel interrupt was taken and thrown away by the guest's own
dispatcher, and Apple's UI stayed on the language screen forever
**while Rockbox scrolled with the same keys**. Documented with
the trace in `ipod_video_intc.c:46-80`.

> **Lesson**: a third-party header file documents the *hardware*, but
> the **use** that project makes of the header tells you which parts are
> actually exercised. **What a driver names and does not use is exactly
> where your model has an invisible hole.**

---

## Catalog of retracted hypotheses

Quick index, so they are not proposed again.

| # | Claim | Status | Where |
|---|---|---|---|
| 1 | The DRAM alias at `0x100000` causes the black screen | Retracted by experiment | `6ea6f90ed` |
| 2 | Interrupt delivery causes the black screen | Retracted by experiment | `6ea6f90ed` |
| 3 | Missing `CACHE_CTL_VECT_REMAP` → NOP sled | Implemented; identical execution; **it wasn't that** | `2893ab659` |
| 4 | "Aliasing low DRAM does not help" | **Badly set-up experiment**: the alias was at 0, not at `0x100000` | `90de12e29` |
| 5 | `0xe9b98` is the firmware's first syscall | False: it is the ARM libc panic printer. It held up 3 commits | `d06ce3123` |
| 6 | The retailOS expects its handlers already in IRAM | Consequence, not cause: it installs them itself at `0x2a0` | `d06ce3123` |
| 7 | 139 references to `0x40000000` in the image | False: raw byte search over floating-point tables. There is **1** | `b9786a0bb` |
| 8 | The blank flash is the blocker; a dump is needed | Retracted twice; the third form (SysInfo) is reproduced without a dump | `d4259dea7`, `288737573`, `3b2637dc8` |
| 9 | The COP fills in the RAM-size global | False: inference by proximity, never verified | `288737573` |
| 10 | `0x1084` is a pointer to a ROM parameter block | **Over-rejected**: both readings were true at the same time | `288737573` → `d83fd8908` |
| 11 | `PP_VER` must say `PP5022C` (what Rockbox shows) | False: the firmware wants the form `"PP5022A "` | `0763f94eb` |
| 12 | `0xf0c` + the string comparator choose the globals base | False: `0xf0c` is dead code in this build; `0x1084`→`0x598` decides | `57ce76fab` |
| 13 | Any write to MMAP0 does the remap | Correct by coincidence; Apple uses MMAP1 | `57ce76fab` |
| 14 | The `0x7000c000` block is I2C | Corrected to "click wheel"… and **over-corrected**: it is both | `a38f51d07` → `62af0171e` |
| 15 | Leaving the SysInfo magic at zero is harmless | *"The earlier analysis had it backwards"* | `3b2637dc8` |
| 16 | `vmcs.bin` is at offset `0xb27508` of the partition | The offset pointed **`0xBF08` bytes inside** the blob | `8c873b597` → `c51e0f6fc` |
| 17 | The Classic work has to be written from scratch | False: `freemyipod/qemu` branch `s5l8702-dev` already exists | `3f5cd7280`, `e17924eab` |
| 18 | The S5L8702 map in `wiki/02` | S5L8700/8701 bases slipped in; "Mikey" is not the click wheel | `e17924eab` |

---

## Debugging techniques, by how much they paid off

1. **Enable semihosting before tracing** (`d06ce3123`). It turned a
   black screen into `Type: _sys_exit` with a register dump. Cost: one
   line.
2. **Captures at intervals, not a single one** (`ad065fbeb`). One frame is not a
   state.
3. **Trace where the flow *enters* the dead zone, not where it leaves**
   (`90de12e29`).
4. **`findlit.py`**: decode `ldr rD,[pc,#imm]` instead of searching for bytes. 139
   false positives → 1 true one (`b9786a0bb`).
5. **gdb stub: breakpoint on the firmware's memcpy + write watchpoint**
   on the suspicious field (`0ee503c2e`).
6. **Dump guest state from `memory_region_access_valid`**: it gives the faulting
   PC and the caller's return address in one line (`084445503`).
7. **`pmemsave` + string search in DRAM** to find out which tasks managed
   to start: the names copied to the scheduler's heap distinguish "the
   task ran" from "the string is in the image" (`8c873b597`).
8. **`-trace "ide_*"`** to separate boot phases (`ad065fbeb`).
9. **Two high-cost traps**, in [05-build-notes.md](05-build-notes.md):
   `-d unimp` in a tight loop slows things down enough to look like a
   hang, and **DMA transfers do not emit `ide_sector_read`**, so
   grepping for that proves nothing.

---

## Patterns that repeated, and deserve to be a rule

1. **The status bit that never arrives.** It is the most frequent failure in the whole
   project: `PLL_STATUS` bit 31, the cache `READY`, USB out of reset (`+0x28`
   bit 7), `BCM_ALT_CONTROL` bits 6/7, ADCS2 bit 7. The symptom is always identical:
   **a loop with no way out, no log and no failure**. The "Registers with
   behavior" section of [07](07-pp502x-hardware.md) exists precisely because
   that is where almost all the blockers ended up.
2. **Zero is not "harmless unknown".** `PROCESSOR_ID` at zero is neither
   of the two cores; `CLICKWHEEL_DATA` at zero is not "no buttons" but "no
   packet"; `PP_VER` at zero is a third answer with no case; GPIO at zero
   means "everything pressed", because it reads active-low.
3. **An `unimplemented_device` floor under every register window**, with the
   modeled parts on top. An unknown register that reads zero is survivable; an
   unmapped one is a data abort whose handler also fails (`6702ab527`).
4. **Skipping the ROM loader means taking on a debt.** Everything that loader
   left in memory has to be reproduced by hand: `GPO32` bit 14, the
   SysInfo block, the {tag, pointer} pair at `0x4001ff18`.
5. **Criterion for knowing what came from outside**: a value that the firmware
   **compares but never produces**. `"IsyS"` is loaded by a single instruction in
   7 MB, and only to compare it.
6. **Mark assumptions in the code itself, not only in the commit.** The tree
   uses `ASSUMED`/`INFERRED`/`UNDETERMINED` in capitals next to the value. And the
   negative criterion, which is the hard part: not filling in `sysinfo_t` from
   iPodLinux because *"it would be inventing with the appearance of rigor"*.
7. **Automated regression from early on.** `navtest.sh` against
   `assets/nav-baseline` and `appletest.sh` with pixel thresholds. Every
   later commit reports the difference, and the accepted ones are explained.

---

## Questions that remain open

None of these is resolved. They are here so that nobody takes them as closed.

### The DRAM size (open question)

`DRAM_MEM_SIZE` is **64 MB**, and it coexists with **two sources that say
32**: the firmware's internal default (`fp = 0x700`) and iPodLinux's table for
`hw_ver 0xb`. Both figures pass the assert and the machine is self-consistent
with either of them. It was left at 64 because that is what Rockbox reads
from the silicon of the real device. The reasoning is in
`ipod_video.h:31-43`.

### The rest

- **What bit 26 means to the real memory controller.** The model works by
  ignoring it; nobody has confirmed what the hardware does
  (`INFERRED`, `2b1ba27e3`).
- **The remap's `mask` field** is documented but **not applied**
  (`ipod_video.c:388-391`). A guest that asks for a different window will not
  get it.
- **The value 390 of Apple's ADC** and **the `0x00` ID of the PCF50605** are
  marked `ASSUMED` in the code; there is no source that confirms them.
- **Where the VideoCore host interface lives** inside the chip's memory is a
  choice of the model (`BCM_IFACE_BASE`, `ASSUMED`).
- **Whether the real BCM2722 has 2 or 4 MB of SDRAM**: our choice, marked
  `UNDETERMINED` in `ipod_video_bcm.h:33-40`.
- **Why the wheel does not move the highlight on the retailOS language
  screen.** Measured (0 px of difference), not diagnosed: it is not known
  whether it is a limitation of the model or behavior of the firmware itself.

---

## Maintenance note

Three times in 51 commits a code comment has been left lying after the code
changed underneath it (`57ce76fab` fixed two at once), and the wiki has
lagged behind the tree at least as many times. If you touch any of this:

- The header comments of `ipod_video.c` enumerate **what is not modeled**.
  That list goes stale with every commit that models something.
- The memory map in [07](07-pp502x-hardware.md) and the "modeled so far" list
  in [04](04-roadmap.md) are the two places that fall behind the most.
- Diagnostic sections that are superseded **are not deleted**: they are
  marked with a *superseded* banner and a link to what replaces them. That is
  the value of this document.
