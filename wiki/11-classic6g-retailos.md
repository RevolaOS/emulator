# The iPod classic 6G retailOS booting in QEMU

What Apple's `osos` does when it is allowed to run on the
`iPod-Classic` machine, in what order it asks for things, and where it stops today.

Everything below is **measured** except where it says `INFERRED` (reasoned
from something verified) or `UNDETERMINED` (we don't know and we don't pretend
to know). The traces are reproducible with the commands at the end.

> This document is **what the firmware does and what is known about each block**, and
> it is ordered by block. If what you are looking for is **how it was figured out, in what order
> and what was mistakenly believed along the way**, that is in
> [12-history-6g.md](12-history-6g.md), which also carries the catalog of
> retracted hypotheses and the chapter on the three times a dismissal turned out
> to be the right lead.

---

## 0. Status

| Milestone | Status |
|---|---|
| 1. It loads and executes the first instruction | **done** |
| 2. It gets through early initialization without hanging | **done** |
| 3. It initializes peripherals and reaches its main loop | **done** — RTXC kernel started, scheduler alive, idle task at `0x22003b1c` |
| 4. It draws something on screen | **done, with a disk** — the language selection screen, 75 463 of 76 800 pixels. See §13 |
| 5. It can be **navigated** | **done** — wheel, SELECT and **MENU**: main menu, going into Music, and going back with MENU to the exact frame it left from (§18) |

> **August 10, 2026, even later — and now MENU goes back.** The two
> remaining symptoms —MENU did not go back and the right panel did not follow the
> highlight— were **the same bug, and it was ours**: the compositor was drawing
> layers the guest had turned off, and from the first screen change a
> dead layer covered the entire interface. The gate is **bits 4..6 of the
> compositor's `CONTROL` register**. With it in place: main menu in the
> chosen language, the wheel moves the highlight **and the right panel follows it**,
> SELECT goes into Music and **MENU goes back, with 0 pixels of difference from
> the frame it left from** (§18).

> **August 10, 2026, later — and now it can be used.** With the click
> wheel modeled (§17), `e` moves the selection bar from **English** to
> **日本語**, `q` brings it back, and Enter **changes screen**: the musical
> note icon appears with the text **「ミュージックが…」**, that is, in the language
> that was highlighted. Each turn changes **8 000 pixels** over a noise
> floor of **0**. And a second thing that was not the wheel had to be fixed: the
> PMU's **hold switch**, which was silently dropping all buttons
> (§17.4).

> **August 10, 2026 — there is an image.** With a disk built by
> `mkdisk6g4k.py` from the `.ipsw`, the retailOS paints **its language
> selection screen**: white list on the left with nine different
> scripts (including 日本語 and Ελληνικά), blue selection bar, battery, and
> the gradient with "Welcome" on the right. `screendump` + `pixcount.py`:
> **75 463 pixels lit out of 76 800, 404 colors**. And **the image has been looked at**,
> not just the number: it is Apple's interface, not garbage (§13).
>
> **Without a disk it still gives 0 pixels**, and that is on the guest, not the model: the
> interface dies looking for fonts before composing anything (§4).

Three things were needed at once, and all three were on the list of
suspects in §6:

1. **`0x38900000` modeled** — it is the video compositor (§13).
2. **DMAC0/DMAC1 as real PL080s** — this is what unblocks booting with a
   disk (§14). And it corrects §9: the ATA was never the culprit.
3. With those two, the `rsrc` volume mounts, the fonts appear and the interface
   composes. Measured with `-d in_asm`, and they are the same addresses that had been
   giving zero for months.

With `-drive if=ide` the boot is clean: zero exceptions, zero reboots.
Without a disk it enters a crash-and-reboot cycle (§4).

Reproduced in an independent audit (August 2026), with these figures:
with a disk, 35 s of `-d int` give 27 166 `Taking exception 5 [IRQ]` and **nothing
else**; `PC = 0x22003b20`, `SP = 0x22010b70`, SVC mode; `0x22003b1c` is
`mcr p15,0,r0,c7,c0,4` and `0x22003b30` is `b 0x22003b0c`, checked in the
disassembly. Without a disk, 25 s give 23 *Data Abort*s, 23 semihosting calls
and 23 `SWRCON`s. The ATA DMA really delivers: with an image that has
`55 aa` at `0x1FE`, `pmemsave 0x0be5d3e0` finds it there after
boot.

And the warning that went with milestone 3, which was true until August 10,
2026: **"reaching the idle loop" is not "booting"**. A quiet system is not
necessarily a working system. With a disk that **no longer applies** (§13,
§14): the interface task runs and composes. Without a disk it still applies as is.

> **Reading note, from a third audit (August 10, 2026).** Sections
> §3, §6 and §9 were written when booting with a disk stopped, and they describe
> that state. **Do not read them as today's state**: §13 and §14 correct them,
> and where they contradict each other what is further down wins, as it is what was measured later. The
> parts of §6 and §9 that talk about booting **without** a disk are still valid.

---

## 1. The image has two link regions

`osos_decrypted.bin` is not a flat binary. It is an ARM ADS/RVCT image with scatter
loading (*scatter-load*) with **two** bases:

```
file [0x00000, 0x0AE48)  ->  0x22000000   stub + boot core (IRAM0)
file [0x0AE48, end)      ->  0x08000000   the OS body (DRAM)
```

In practice: `address = 0x08000000 + (file_offset - 0xAE48)` for everything
from `0x0AE48` onwards. Disassembling the whole image with a single
base points at the wrong instruction in 99 % of the file.

To disassemble:

```bash
# the IRAM part
arm-none-eabi-objdump -D -b binary -m arm --adjust-vma=0x22000000 \
    --start-address=0x22003b0c --stop-address=0x22003b40 osos_decrypted.bin
# the body in DRAM
arm-none-eabi-objdump -D -b binary -m arm --adjust-vma=0x07FF51B8 \
    --start-address=0x08327760 --stop-address=0x08327780 osos_decrypted.bin
```

`0x07FF51B8` is not a magic number: it is `0x08000000 - 0xAE48`.

---

## 2. The unnamed block at `0x3C100000`

**No source we have documents it.** The only `0x3C100000` in Rockbox's
`s5l87xx.h` is `LCD_BASE`, and it is under
`#if CONFIG_CPU==S5L8700` — another chip. The freemyipod model doesn't have
anything there either. The only authority is Apple's code.

And it is small: searching for the literal word `0x3C100000` in the image and then
all PC-relative `LDR`s that resolve to one of those words yields
**exactly three sites**, so the list is complete, not "what
showed up":

| Site | What it does |
|---|---|
| `0x08031d9c` | `str r0,[r1,#8]` — seed: seven bytes of state + `TECNT` + the word at `0x3C700008` |
| `0x08327738` | `str #0,[r1]` — writes **zero** to `+0` |
| `0x08327760` | `while (!([+0] & 7)) ; return [+4]` |

> **Correction (audit).** The third addend is **not** "the previous
> seed", as the first version of this page said. The literal at
> `0x08031de0` is `0x3C700000`, i.e. `TIMER_BASE`, and `+0x08` is `TADATA0`.
> The seed is mixed with **two** independent timer reads, and
> nobody ever reads `+4` or `+8` of this block again: the seed
> register is write-only from the guest's point of view.

In context: `0x08327714` takes a critical section and writes that zero;
`0x08031d30` seeds `+8`, pulls twenty words through `0x08327760`
discarding nineteen, and releases the section at `0x08327750`. Seeding a
generator and throwing away its first outputs is an RNG idiom, and the seed is
mixed with a free-running counter (`0x08015748` is a *veneer* to `0x22001EC4`, which
is "return `TECNT`"). Nothing else fits: no new data comes in between the twenty
reads, so a checksum or a hash would return the same word
twenty times and the loop would make no sense.

- That it is an **RNG**: `INFERRED`.
- The three-register contract: **verified**.
- What it is called and what else it has: `UNDETERMINED`.

### The trap: there is no enable bit

No instruction writes a nonzero value to `+0`. The only
write is the zero at `0x08327738`, and it happens **before** the twenty
reads, not after. A model that makes "data ready" conditional on the
guest having enabled the block hangs on the first extraction, because
`0x08327760` spins with no *timeout* and no exit. The ready bits have to
rise on their own.

It is pattern 1 of [09-history-5g.md](09-history-5g.md) — *the status bit
that never arrives* — with an aggravating factor: here the bit seemed to have an enable
and it doesn't.

### How much this block really weighs (measured in the audit)

**With a disk attached, the retailOS does not touch `0x3C100000` even once**:
`0x08327760` does not appear in a `-d in_asm` log of that boot. The block
is only reached on the path **without** a disk, once per boot attempt, and
the seeding site `0x08031d9c` is executed in neither — so
the generator's state always starts from zero.

In other words: removing the false enable bit really unblocked an infinite
spin **in the state of the tree at that time**, but the block is not
on the path that today leads to the idle loop. Milestone 3 cannot be credited to it.

---

## 3. The LCD interface at `0x38300000`

It is an MPU/8080-style command port, **not** a scanning engine: there is no
framebuffer address and no video timing. The panel keeps its own
image. The block at `0x39200000` (`LCDBASE`, "CLCD CONTROLLER" in Rockbox) does
have framebuffer addresses, and the retailOS barely touches it.

The status bits (`LCD_STATUS`, `+0x1C`) are what matters, because each one
is a spin with no *timeout* on both sides. **Rockbox and the retailOS agree
on all three**, which is what makes them reliable:

| Bit | Meaning | Rockbox | retailOS |
|---|---|---|---|
| 0 | read data available in `LCD_DBUFF` | `lcd-s5l8702.c:241` | — |
| 1 | interface idle, control can be written | `lcd-s5l8702.c:207` | `0x080b6f98`, `0x080b6ffc` |
| 4 | write FIFO busy | `lcd-s5l8702.c:215` | `0x080a90b4` |

`s5l87xx.h` notes the reset value as `0x0106`: bits 1, 2 and 8 set and bit 4
clear — idle and not busy, consistent with the above. What bits 2 and 8 mean is
`UNDETERMINED`; no driver looks at them.

With the window unimplemented, the `while (!(LCD_STATUS & 0x2))` loops never
return. It is the wall that ate 48 million reads in a 45 s trace.

**The block is wider than Rockbox knows.** The retailOS writes
`0x3830007C = 0x804` and `0x38300088 = 0x1000000` in its very first LCD
configuration, before touching anything Rockbox names. Both are `UNDETERMINED`,
but the window has to cover them or they fall to the machine's I/O floor.

### Where the pixels go: **zero arrive**, and now it is counted

Correction to the two previous versions of this section. With the panel
modeled and `LCDTRACE=1` counting each access, a 25 s boot without a disk gives:

```
LCDTRACE totals: 63 writes to the command port +0x04, 168 to the data port
                 +0x40..0x5C, of which 0 were pixels; 21 frame-engine starts
```

The 168 writes to the data port are **21 starts × 8 parameters**: the
four bytes of `0x2A` (`00 00 01 3f`) and the four of `0x2B` (`00 00 00 ef`).
**Not a single pixel word.** The sentence "one pixel word arrives per
CPU write" was that `0x3f` byte, and the conclusion "so the framebuffer goes
via DMA" doesn't hold up by another route: in an entire boot without a disk,
**no DMAC0 channel ever points at `LCD_WDATA`** (`-d unimp`: the only
destination programmed is `0x3CA00010`, the I²S FIFO, and the LLI written is
`0x22010020`, also audio).

What the retailOS does do is start a **frame engine** inside the LCD block
itself, whose registers no external source knows:

| Register | What is written to it | Site |
|---|---|---|
| `+0x70` | `1` — start | `0x08127f94` |
| `+0x74` | `(320 << 16) \| 240` | `0x08127f8c` |
| `+0x78` | `0x0005000a` `UNDETERMINED` | `0x08127f3c` |
| `+0x7C` | `0x00000804` `UNDETERMINED` | `0x080afb1c` |
| `+0x80` | `1` … `0` around setting the window | `0x0808b000`/`0x0808b030` |
| `+0x88` | `0x01000000` `UNDETERMINED` | `0x080afb0c` |
| `+0x8C` | `while (LCD[0x8c] & 3);` — bits [1:0] = transfer in progress, and it is a spin **with no timeout** twice over | `0x080a4090`, `0x080b0128` |

**Where that engine reads from is `UNDETERMINED`**: there is no source address
register in the 256 bytes of the window, and there is no DMA pointing at it.
The model accepts it, counts it and declares `+0x8C` free, which is what the
unimplemented window already did; the difference is that now it says so out loud.

Two things are verified and narrow down the search:

- **The 256-byte window is enough.** With the LCD modeled at `0x38300000..0xFF` and
  the rest of the block falling to the machine's *io floor*, a 25 s boot without a disk
  produces **not a single** access to `0x383000xx` above `0xFF`
  (`-d unimp`, zero matches for `addr 0x383`). So the source
  register is not in the LCD block.
- **Nor is it DMAC0.** No value `0x38300040` ever appears in the
  `-d unimp` log; the only destinations DMAC0 receives are `0x3ca00010` (I²S
  FIFO) and descriptors at `0x22008c5c`/`0x22010020`. Rockbox **does** push the
  frame via DMAC0 (`lcd-s5l8702.c:172`, `dstperi = S5L8702_DMAC0_PERI_LCD_WR`);
  Apple does not. Apple uses the `+0x70` engine, and that is the whole difference.

### The complete "paint a frame" sequence, disassembled

`0x08127f14` is the entire function, and it explains why zero pixels come out without
anything failing:

```asm
08127f14: push {r3,r4,r5,lr}
08127f1c: bl 0x8015710          ; (0x2080, 0, 1)
08127f30: bl 0x8127268
08127f3c: str r0,[r4,#0x78]     ; LCD[0x78] = 0x0005000a
08127f54: bl 0x80bcef8          ; (64,64,0,0,0) -> writes 0x389003AC
08127f58: ldr r0,[0x089115e4]   ; the panel driver record
08127f64: cmp r0,#0 ; beq +     ; <- null check
08127f84: blx [r0+0x0c]         ; (0,0,0,320,240)   <- THE DRAWING
08127f8c: str r0,[r4,#0x74]     ; LCD[0x74] = 0x014000f0
08127f94: str r0,[r4,#0x70]     ; LCD[0x70] = 1     <- "presents"
```

That pointer **is zero in the image** (`0x089115e4` holds `0x00000000` in the
file), so someone sets it at runtime, and the null check
at `0x08127f64` is real. Three measurements say it passes:

- With a disk, at 45 s, `xp /1wx 0x089115e4` returns **`0x08911fe0`**, one of
  the three panel driver records. `[0x08911fe0 + 0x0c] = 0x080c1164`.
- Without a disk, `-d in_asm` of 25 s: `0x08127f14` is entered **22 times** and
  `0x080c1164` is entered **22 times** — once per boot attempt. The null branch
  is never taken.
- And `0x080c1164` is exactly the function that the header of `ipod_classic_lcd.c`
  identifies as Rockbox's `displaylcd_setup()`.

That is: **the retailOS does call "draw the whole screen" on every boot,
and the only thing that call emits is `0x2A`, `0x2B`, `0x2C` and eight parameter
bytes.** Same as in Rockbox: `displaylcd_setup()` only sets the window. What
comes next in Rockbox is `displaylcd_dma()`; what comes next in Apple is
`LCD[0x70] = 1`. The pixels never go through the CPU in either
case.

### The serious candidate for the source: `0x38900000`

> **RESOLVED, August 10, 2026 — see §13.** It was. It is the compositor: five
> layer descriptors with source address, stride, format, size and
> position, at `+0x058 + k*0x18`. The suspicion that `+0x208` was the source
> pointer was **false** (it is always 0, written by `0x08127428`); the
> source addresses are at `+0x060/78/90/a8/c0` and `+0x038..+0x044`. What
> was spot on was the method: counting values and not just accesses.

A later audit withdrew the "dismissed" label this block carried. The
test that dismissed it was "no register receives a value that looks like a
DRAM address", which is true but doesn't say what it seemed to say. What is
there, measured in a 25 s boot without a disk with `-d unimp`, are **793
distinct addresses written**, and two of them are unmistakable:

| Register | Value | What it is |
|---|---|---|
| `+0x400` … `+0xFFC` | `value == offset - 0x400`, i.e. `0,4,8,…,0x3FC` | **768 words = 3 × 256: an identity gamma/palette table.** Only a video path writes that |
| `+0x214` | `0x013f00ef` | **exactly the panel geometry**, `(319<<16)|239` |
| `+0x208` | `0` | written right after the geometry — the shape of a source pointer that is zero |
| `+0x3AC` | bitfield from `0x080bcef8(64,64,0,0,0)` | configuration |
| `+0x0d4..0xEC` | `1`, `0x1000` | |
| `+0x0E0` | `0x50004000` (from `0x081459d4`) | `UNDETERMINED` |

And the code that drives it is **the same** that drives the LCD frame engine:
of the eighteen `0x38900000` literals in the image, fourteen are in
`0x08127xxx`, the same function that writes `LCD[0x74]` and `LCD[0x70]`.

- That `0x38900000` is the video composition/output block from which the
  `+0x70` engine takes the frame: **`INFERRED`**, with the two measurements above in
  favor and none against.
- That `+0x208` is the source address: **`INFERRED`**, unconfirmed.
- The next cheap experiment: map `0x38900000` as a real device
  (today it falls to the *io floor*), log who writes `+0x208` and with what, and
  see whether in any boot it ever holds something other than zero.

### The panel, which is modeled

`ipod_classic_lcd.c` carries a 320×240 RGB565 memory, a window with
`0x2A`/`0x2B`, a write pointer after `0x2C`, and a `qemu_console`. All the
numbers come from a measurement, none from a datasheet:

- **8-bit command set**: the guest writes `LCD_CON = 0x80000c21`, and
  `0x080b6f8c` only produces that value via the `lcd_get_type() ∈ {0,1}` branch, which
  is Rockbox's `LCD_CMDSET_8BIT` (`lcd-6g.c:201-214`).
- **320×240**: the three panel driver records the
  firmware chooses between (`0x08911f90`, `0x08911fb8`, `0x08911fe0`) all three start with
  `0x00f00140` = `(240<<16)|320`; the window it sets is `0..0x13f` × `0..0xef`;
  and `+0x74` receives `(320<<16)|240`.
- **RGB565**: the frame mode is `LCD_MODE_P16`.

Validation with a control case, because a panel that is not tested is not a
measurement: `tools/ipod_classic/lcdtest.S` emits exactly that sequence and pushes
76 800 words. `screendump` + `tools/ipod_classic/pixcount.py` return
**76 800 pixels lit and 1920 distinct colors**, which is pixel-for-pixel what
the test writes. If the window arithmetic or the format were wrong, that
number would come out obviously wrong.

```bash
arm-none-eabi-as -o /tmp/l.o tools/ipod_classic/lcdtest.S
arm-none-eabi-objcopy -O binary /tmp/l.o /tmp/l.bin
qemu-system-arm -M iPod-Classic -kernel /tmp/l.bin -display none -monitor stdio
(qemu) screendump /tmp/shot.ppm
python3 tools/ipod_classic/pixcount.py /tmp/shot.ppm
```

When the retailOS draws (see §4, it happens on the error path) it emits
`0x2A`, `0x2B`, `0x2C` — *column address set*, *page address set*, *memory
write* — which is the command set of an ILI9341-style controller. Which one is
really fitted is chosen at runtime with the GPIO *strap*
`PDAT(6)` bits [5:4], which we don't model.

---

## 4. The crash loop without a disk, and why it was invisible

Without `-drive`, the machine didn't sit idle: **it crashed and rebooted**,
about nineteen times every twenty seconds. With `-d int`: 19 *Data Aborts*, 19
semihosting calls and 19 writes to `SWRCON`.

It couldn't be seen because the diagnostics went out on **stderr** and the
traces discarded it. The firmware was saying it out loud the whole time:

```
CPU_exception_handler
qemu-system-arm: warning: S5L8702: guest asked for a software reset (SWRCON = 0xaa5), next image "--------"
```

> Lesson, and it's number 1 in [09-history-5g.md](09-history-5g.md): if the
> firmware prints something, listen to it. Semihosting was already on by
> default on this machine and even so we spent a while looking at `-d unimp`
> histograms instead of reading the two lines that explained everything.

### The cause

```
...with DFSR 0x5 DFAR 0xea00002a
```

`0xEA00002A` isn't an address: it's an ARM branch instruction. And it is
`0xEA000002 + 0x28`, where `0xEA000002` is exactly the word that `main` writes
to `[0x22000000]` (step 3 of its initialization).

The aborting instruction is a pair of C++ hops:

```asm
0815b230:  ldr r0,[sp]        ; pointer to the object
0815b234:  bic r0,r0,#1
0815b238:  ldr r1,[r0]        ; pointer to the vtable
0815b23c:  ldr r1,[r1,#0x28]  ; <-- aborts
0815b240:  blx r1
```

It is a **virtual method call on a null object**. And here's the interesting
part: since the firmware itself maps VA 0 onto IRAM0, the null dereference
**doesn't fail** — it calmly reads a word from the vector table — and only the
second one blows up. A null pointer on this machine doesn't give a clean
fault: it gives a plausible value and a crash two hundred bytes later.

**Correction from August 2026, and it is a fundamental one: the null does NOT
come from the resource lookup.** Measured with `gdbpoke.py` (§10), stopping at
each instruction of that function:

| Stop | What is read |
|---|---|
| `0x0815b1e0` | `r0 = 0x086aef48` — the `"FONT"` lookup **finds** the resource |
| `0x0815b1f4` | `r0 = 0x0874a444` — the `"Str "` one **does too** |
| `0x0815b20c` | `r5 = 0x0be6770c` — not null, doesn't jump to the exit |
| `0x0815b230` | `[sp] = {0, 0}` — **here** the null appears |
| `0x0815b23c` | `r1 = 0xea000002`, the word at VA 0; the next load aborts |

And the `FONT` record it returns, read from the guest, is
`0dad0001 0000003a 01d58101 0dad0002`: name id, size 58, style.

So the provider chain works and the resource exists. What fails is the
construction of the text object, and the reason is concrete:

```
0x0815b22c  bl 0x08239658   "make me a string with this font"
0x08239658  -> 0x08238fa0 -> 0x082393ec
0x082393ec  looks up the font by name, size and style in TWO lists:
              [0x08912cd8 + 8]  fonts already instantiated
              [0x08912cd8 + 4]  available font files
            if neither matches, returns {0,0}
both lists are filled by a single function, 0x08239220, which does this:
  0x080518a4(1, &vol, &path)   ->  vol = 4, path = "Resources/Fonts"  (0x08380628)
  0x08239134(vol, path)        ->  walks THAT DIRECTORY and registers each file
```

`0x08239134` is a real directory iterator — it skips entries that begin with
`.` (compares with 46) and calls `0x08107184` for each name.

**Conclusion, VERIFIED end to end: the retailOS carries no font inside the
image. They all live in `\Resources\Fonts\` on the disk's `rsrc` partition**
(`Helvetica.ttf`, `HelveticaBold.ttf`, `CJK11.ttf`, `CJK16.ttf`, four
`MonoHope-*.sbit`). With no volume mounted the list is empty, no font matches,
and the caller calls a virtual method on the `{0,0}`.

A non-destructive experiment confirms it: with `gdbpoke.py --skip
0x0815b230=0x0815b270:0` that call is skipped without touching Apple's image,
and the boot **falls back into the same loop**, now with `DFAR 0xEA00003E`
instead of `0xEA00002A` — another virtual method, slot `+0x3C`, on another null
of the same origin. It's not a fault that can be sidestepped: without fonts,
the interface's entire object graph comes out null.

**With a disk attached, the whole loop disappears** — but for the wrong reason:
the interface doesn't even get as far as trying (§9).

---

## 5. The ATA bridge at `0x38700000`

It is not a *task file*: the eight device registers are at `0x54..0x74` but
they aren't read directly. Reading one starts a bus cycle whose result appears
later in `ATA_PIO_RDATA`. Rockbox writes it out in full (`storage_ata-6g.c`)
and Apple's driver is the same function compiled from a different source
(`0x08096824`).

### The DMA

Specified twice, and the two agree: Rockbox in `storage_ata-6g.c:851-880`, and
Apple's driver traced register by register.

```
<ATA command 0xc8 to ATA_PIO_CSD>
ATA_TBUF_START = buffer        (ATA_SBUF_START when writing)
ATA_TBUF_SIZE  = bytes
ATA_XFR_NUM    = bytes - 1
ATA_CFG |= flags ; ATA_CFG &= ~(BIT(7)|BIT(8))
ATA_IRQ = BITRANGE(0,4) ; ATA_IRQ_MASK = BIT(0)
ATA_COMMAND = BIT(0)           <- and only now does it move
<wait for the interrupt>
ATA_COMMAND = BIT(1)           <- done
```

`ATA_CFG` bit 4 selects the direction: set = source buffer = write to disk.

**The order is what shapes the model, and it is the opposite of the intuitive
one.** The buffer address is programmed *after* the ATA command, so
`start_dma` can't know where the data goes: it can only hold on to the
*callback* the IDE core gives it, and it is the write of the start bit that
triggers it.

Two traps that cost an afternoon each:

- `s->bus.dma` has to be claimed **after** `ide_bus_init_output_irq`, not
  before: the last thing that function does is set `bus->dma = &ide_dma_nop`.
  Claiming it before makes every DMA command get accepted and silently ignored,
  which looks a lot like a disk that doesn't answer.
- Checking the DMA by the absence of complaints doesn't work. It is checked by
  reading guest memory: after the first transfer, `0x0be5d3e0 + 0x1fe`
  contains `55 aa`, the MBR signature of sector 0 of the image, in the right
  byte.

---

## 6. Where it stopped with a disk (historical, **superseded** by §13 and §14)

> **This section describes the state before August 10, 2026.** With a disk it
> no longer stops here: the "What does NOT execute" table below is exactly the
> one that §14 measures again, and it comes out all **1**. It is kept because
> the method (measuring function entries, not mid-function addresses) is what
> was needed to get to §14. **Without a disk**, everything in this section is
> still valid.

With a disk, the system reached its **idle loop** and stayed there:

```
22003b1c:  mcr p15,0,r0,c7,c0,4    ; wait for interrupt (ARM926)
22003b30:  b   0x22003b0c
```

`PC = 0x22003b20`, SVC mode, `SP = 0x22010b70` (the initial SVC stack). The
RTXC kernel is up, the scheduler runs, timer interrupts come in, and **all
application tasks are blocked**.

What it does do before going to sleep:

- MIU, clock, PMU over I²C, GPIO, click wheel, external interrupt controller,
  and the ATA.
- Two DMA reads, both at **LBA 0**: eight sectors and then sixty-four. The
  bytes arrive (verified, §5). After that, nothing more.
- It polls `WHEELINT` (`0x3C200014`) about **120 times per second** — measured
  in the audit with two runs of 20 s and 50 s: 3071 and 6770 reads, i.e. 123/s
  at steady state. (The first version of this page said "three times per
  second"; that was off by a factor of 40.) It is still not a blocking spin — a
  spin would be millions — and it still shows the same thing: **there are tasks
  running besides the idle one**, which is the best proof that the scheduler is
  alive.

### What does NOT execute, and this is the important data point

| Address | What it is | Does it execute? |
|---|---|---|
| `0x0805712c` | the firmware partition parser: at `0x08057198` it compares `]ih[` (`0x5B68695D`, verified at `0x080572d4`) with offset `0x100` of what it read | **no, never** |
| `0x0815b1b8` | the UI code that draws text (the one that crashed without a disk) | **no** |
| `0x08239658` | the construction of the string object | **no** |
| `0x08236130` | the resource lookup | yes, once |

> **Correction (audit).** The first version of this table measured the parser
> with `grep -c '^0x08057160'`, and **that measurement was invalid**:
> `0x08057160` is `add r0,sp,#4` in the middle of a function, not an entry, so
> a `-d in_asm` log would have given zero even if the code ran on every boot
> (see §8). The function's real entry is `0x0805712c` — `push {r1..fp,lr}` —,
> and measured there the conclusion **stands**: zero, with and without a disk.
> The other three addresses are function entries (`push` as the first
> instruction), so those were always valid.

In other words: **the blockage is upstream of both the firmware partition
parsing and the drawing**, and it isn't the storage, because the storage
works.

### Hypotheses ruled out by measurement

| Hypothesis | Ruled out because |
|---|---|
| "It's missing a valid MBR" | With an MBR and two partitions (type 0 + FAT32) it does exactly the same two reads |
| "The firmware partition has to be at LBA 0" | With the raw `Firmware-24.9.1.2` image at sector 0, and `]ih[` verified at `0x100`, the same two reads |
| "The disk is too small" | With 64 MB, 289 MB, 512 MB and 80 GB: the same two reads |
| "It's blocked in the ATA DMA" | It was, and no longer is: the transfers complete and the guest acknowledges the interrupt |
| "It's blocked in the LCD" | It was, and no longer is; now, **with a disk**, it touches the block exactly five times and stops (`LCDTRACE=1`: `+0x00 = 0x80000000`, `+0x00 = 0x80100db1`, `+0x88`, `+0x20`, `+0x7c`, all from `0x080afaf4`) — not twice, as the previous version of this line said |
| "The click wheel has it stalled" | It polls it ~123 times/s from a periodic task (measured: 3071 reads in 20 s, 6770 in 50 s); it isn't a blocking spin |

### The next thing to figure out

What the blocked task is waiting for. The most promising route is not guessing
peripherals, it's asking the kernel: the image has **RTXCbug** compiled in,
RTXC's interactive debugger, with its REPL (`"RTXCbug> "`, `"Bad command"`,
`"G - Go to Multitasking Mode"`, `"** Task Register Snapshot **"`) and a full
semihosting retarget, including `SYS_READC`. If we manage to get in, it tells
us directly which tasks there are and what they're blocked on.

Candidates to rule out, in order of cost. **Numbers 1 and 4 were the right
ones, and both were needed: see §13 and §14.**

1. ~~The `0x38900000` block~~ **RESOLVED (§13): it is the compositor.** What
   the machine covered with the *io floor* — reading zero and dropping writes —
   wasn't neutral: six functions do read-modify-write on `+0x008`, so each one
   erased what the previous one had set.
2. Real GPIO: today it returns zero for everything. `PDAT(11)` bit 1 = 0
   selects parallel ATA, which is what we want, but `PDAT(6)` (panel type) and
   `PDAT(10)` (headphones) also read zero and maybe they shouldn't.
3. ~~The EIC (`0x39A00000`), which gets programmed and never fires.~~ Modelled
   in `ipod_classic_eic.c`: the standby path waits on `ALVCON` and wakes
   through it.
4. ~~DMAC0 (PL080), which gets programmed with descriptors in DRAM and never
   completes.~~ **RESOLVED (§14), and it was the cause of the blockage with a
   disk.** This line was written from the very beginning and in last place on
   the list; the cost of fixing it turned out to be the lowest of the four,
   because QEMU already ships `hw/dma/pl080.c`.

---

## 9. With a disk: where the interface stopped, instruction by instruction (historical)

> **Superseded by §14, which corrects the final diagnosis.** The call chain
> below is well measured and is still the right one; what was wrong was the
> last line, "the gap is between *the DMA finished* and *wake whoever asked for
> it*" read as an **ATA** problem. It wasn't: what didn't return was the
> **DMAC0** `memcpy` that sits inside that same read call. See §14. Today, with
> `E.img`, the whole chain is traversed and `0x081228d0` **is** reached.

This answers §6's "what the blocked task is waiting for", which had been open
from the beginning. All measured with `gdbpoke.py` and a zero-filled 64 MB
disk; with `A.img` (valid MBR, real firmware partition and FAT32 with
`iPod_Control`) it comes out exactly the same.

The chain, from the outside in. Each link is a real debugger stop:

```
0x0811c008   creates the RTXC task "MP3ExampleTask"
0x0811bf68     body of the interface task              <- reached
0x0811bfb8       blx [obj+0x204] -> 0x081a3134         <- enters and does NOT return
0x081a3134         blx [obj+0x208] -> 0x081a3360       <- enters and does NOT return
0x081a3360           loop over 9 subsystem factories; stops at no. 8
0x0824caa8             new(0x124) + constructor 0x0824cc8c
0x0824cc8c               bl 0x08035bf8                 <- SysInfo reader
0x08035bf8                 bl 0x080ca440
0x080ca440                   bl 0x0815f818
0x0815f818                     bl 0x0815f7c0
0x0815f7c0                       bl 0x0812289c
0x0812289c   81228b4: bl 0x0805c430      ; takes the mutex at [obj+0x2c]
             81228c4: ldrb r0,[r4,#0x28] ; <- THE FLAG
             81228c8: cmp  r0,#0
             81228cc: beq  0x81228bc     ; 81228bc: bl 0x080c8968  = wait
```

**`while (![obj+0x28]) wait();`** — the interface waits until there is a
mounted volume. With a disk, `0x081228d0` (the loop exit) is never reached;
without a disk it is. The CPU goes to the idle loop because the task is blocked
on a kernel object, not spinning: that's why there are 27,000 IRQs and no other
exception.

Who raises the flag: **only** `0x08122a04`, with two callers, `0x08059ae4` and
`0x081229f0`, both inside `0x08059aa0` = "mount the volume". And `0x08059aa0`
has **a single** caller, `0x083287e0`, which is the body of the task that
consumes queue 4:

```
0x0832878c   for(;;) { msg = KS_receive(4); switch (msg[0x10]) {
                 case 0: 0x08059aa0(dev, 0);   // mount
                 case 1: 0x08126638(dev);      // no medium
                 case 2: 0x081263dc(dev); ... } }
```

Without a disk, that task receives messages and mounts (the probe fails, but
**it raises the flag anyway** and the interface carries on). With a disk **it
receives none**: `0x083287b4` is not reached in 90 s.

### And the message is sent: it's the receiver that never gets to listen

This has to be said carefully because the first reading was the wrong one. The
only sender of type-0 messages in the image is `0x0813b160` (static message at
`0x0813b21c`, `[msg+0x10] = 0`), reached from `0x0813b220`, which is the body
of the **`"ATAWorkLoopTask"`** task (string at `0x0813b2bc`, created at
`0x0813b234` with priority 24). It seemed reasonable to think that with a disk
it didn't run. **That's false, and measuring it takes a minute:** with `A.img`
and without a disk, the first four stops at `0x0813b160`, `0x0813b1ac` and
`0x0813b1c0` are **identical register for register**. The message is sent in
both cases.

What changes is the **receiver**. Before its first `KS_receive(4)`, task
`0x0832878c` makes two calls:

```asm
832878c: push {r4,r5,r6,lr}
8328790: mov r0,#1
8328794: bl 0x081c207c        ; get device 1               <- returns
8328798: mov r5,r0
832879c: bl 0x08126638        ; <- WITH A DISK IT DOES NOT RETURN
83287a0: mov r1,#0 ; mov r0,#4 ; bl 0x080159a0   ; receive  <- never reached
```

Measured in both configurations: without a disk the stops go as far as
`0x083287a0` and repeat; with a disk they stop at `0x0832879c` and end there.

And from there downward, each link is another real stop:

```
0x08126638   lock([dev+0x10]) ; blx [[dev+4]+0x18] -> 0x0823d420
0x0823d420     the storage probe: allocates 4096 B, LBA 0
0x0823d52c       bl 0x0827aad0                       <- WITH A DISK IT DOES NOT RETURN
0x0827aad0         bl 0x0827a3f8 ; blx [r0+8]        <- returns (r0 = 0)
0x0827ab10         bl 0x0827aa1c -> 0x0827a980       ; read and classify LBA 0
0x0827a9ac           blx r3 -> 0x080aea1c            <- THIS IS WHERE IT STOPS
                       r0 = 0x8be5d3e0 (uncached alias of 0x0be5d3e0),
                       r2 = 1  -> "read me ONE block"
0x080aea1c             blx [[0x08966004+4]+0 +0x0c]  ; the storage read
```

`0x0827a980` is short and says it all: it calls the read, and if it returns
zero it looks at `buf+0x1FE` for `0xAA55` (→ `0x080aee04`, MBR parser) and
otherwise at `buf+0` for `0x4552` `'ER'` (→ `0x080a38cc`, Apple Partition Map
parser). **It never gets to look.**

And what makes this a finding and not a dead end: **the read does happen**.
`ATATRACE=1` shows the `0xC8` command, `ATA_TBUF_START = 0x0be5d3e0`,
`ATA_TBUF_SIZE = 0x1000`, the transfer, the guest reading `ATA_IRQSTAT`,
acknowledging it with a one and stopping the engine with `ATA_COMMAND = 2`. The
disk delivers, the interrupt arrives and the guest acknowledges it — and even
so the read call doesn't return. **The gap is between "the DMA finished" and
"wake whoever asked for it"**, not in the ATA.

It is the same point with a zero-filled disk, with `A.img` (512-byte MBR, real
firmware partition and FAT32 with `iPod_Control`) and with `E.img` (MBR in
4096-byte units): all three stop there. The disk contents still don't matter,
and now we know why: nobody gets to look at them.

> **An audit nuance, because the original sentence said "identical 682-line
> traces" and that's more than can be claimed.** Repeated with `ATATRACE=1` on
> a zero-filled image and another with a valid MBR: **680 lines each and a
> single difference**, `ATA_TBUF_START` (`+0x3c`), which comes out as
> `0x0be5d3c0` in one and `0x0be5d3e0` in the other. And it isn't the disk
> contents: the same zero-filled image gives `0x0be5d3e0` in one run under
> `gdbpoke` and `0x0be5d3c0` in another without a debugger. There is
> **run-to-run variation** in where that buffer lands, so "identical" has to be
> read as "the same except for the buffer address". The conclusion doesn't
> change; the word "identical" was too strong.

### What got fixed along the way: GPIO pin 86

There is a second path to the same flag and it was broken by one bit.
`0x08328720` is a polling loop that decides between "mount" and "no media":

```asm
bl 0x08294d7c ; cmp r0,#0 ; bne sin_medio     (PMU 0x4b bit 2)   measured: 0
bl 0x08295508 ; cmp r0,#0 ; bne sin_medio     (PMU 0x12/0x4b)    measured: 0
bl 0x080a1474 ; cmp r0,#0 ; bne sin_medio     ([0x089120a1]!=100) measured: 0
bl 0x080aeca4 ; cmp r0,#0 ; movne r1,#0       gpio_get_pin(86)   measured: 0  <-
mov r0,#4 ; bl 0x080b2ce8                     ; announce {r1}
```

`0x080aeca4` is literally `gpio_get_pin(86)`, and `0x3CF00000` was
`create_unimplemented_device`, i.e. zero: the firmware concluded "no media"
even when there was a disk. Now there is a GPIO model
(`hw/arm/ipod_classic/ipod_classic_gpio.c`) and pin 86 follows whether there is a `-drive`;
measured after the change, `0x080aeca4` returns 1 and the message becomes
type 0. **Necessary and not sufficient**: `0x080b2ce8` dispatches via
`[[0x08911ad8]+0x10]`, and that pointer is still 0 at 35 s, so the
announcement is dropped. What pin 86 fixes is real and was wrong; what remains is
something else.

What pin 86 is on the board: `UNDETERMINED`. What it does in Apple's firmware:
verified above.

### The concrete obstacle that remains

> **RESOLVED, August 10, 2026 — see §14, and with a fundamental correction.**
> The diagnosis "the hole is between *the DMA finished* and *wake up whoever
> requested it*" pointed at the right gap but the wrong peripheral: **the ATA has
> nothing to do with it**, its cycle closes completely and its semaphore wakes. What
> does not return is a later `memcpy` via **DMAC0**, which slept on a semaphore with no
> *timeout* waiting for an IRQ 16 that nobody generated because `0x38200000` was
> `create_unimplemented_device`. Everything that follows in this section describes
> real symptoms; the conclusion about the ATA does not.

> **With a disk, the read of a block from LBA 0 (`0x080aea1c`, called from
> `0x0827a9ac`) never returns, even though the ATA DMA completes and the guest
> acknowledges the interrupt.** Everything else is checked and works: the ATA
> delivers the bytes, the resource providers are mounted, the embedded
> container is in RAM, the panel draws whatever it is sent and the window is set
> correctly.

What is known about the ATA at that point (`ATATRACE=1`, zeroed disk, `A.img` and
`E.img`: **the three traces are identical, 682 lines**):

- Command `0xEC` (IDENTIFY) completes.
- Two DMA reads, both at LBA 0: `0x1000` bytes to `0x0be5d3e0` and
  `0x8000` bytes to `0x0be55320` — which are exactly the 4096 buffer of
  `0x0823d420` and its second pass.
- Then the driver closes cleanly (`ATA_CFG = 0x640`, reads `+0x70`/`+0x7c`)
  and **never speaks again**. It is not spinning on any register: the hang
  is above the driver.
- Without a disk the trace ends at `WR +0x70 = 0xec` with no response. The IDENTIFY fails,
  `0x0823d420` exits via its early branch (`0x0823d44c`) without ever reaching
  `0x0827aad0`, and that is why the no-disk path does not hang: **the one that
  finds a disk hangs, not the one that doesn't.**

The first thing to do tomorrow is to step into `0x080aea1c` with `gdbpoke.py`
—`blx [[0x08966004+4]+0 +0x0c]`, two or three more levels— and find out on which
kernel object the request falls asleep. The suspicion, `INFERRED`: the
end-of-transfer notification does not reach the task waiting for it. Two
things to rule out first, both cheap: that the ATA interrupt line
(`S5L8702_ATA_IRQ`) is on the VIC the firmware has unmasked, and that
`0x0823d420` is not also waiting for a second interrupt from the ATA
block that the model does not generate (the model only raises `ATA_IRQSTAT` bit 0, from
the IDE core's INTRQ; what the other bits do is `UNDETERMINED`).

`INFERRED`, and it has to be checked before believing it: if that gets unblocked, a
volume is mounted; if the volume is the `.ipsw`'s `rsrc` partition,
`\Resources\Fonts\Helvetica.ttf` appears; with that, `0x082393ec` stops returning `{0,0}`
and the interface composition can finish. Only then will we know where
the `+0x70` frame engine reads from, which is the last unknown on the path to
pixels.

---

## 7. How to reproduce it

```bash
wsl -d Ubuntu-24.04 -e bash -c "cd /mnt/c/Desarrollo/qemu-ios/build && ninja qemu-system-arm"
cd build

# Without a disk: the crash cycle. Look at STDERR, which is where the firmware talks.
./qemu-system-arm -M iPod-Classic -kernel ../assets/classic6g/osos_decrypted.bin \
    -display none -monitor none -serial null

# With a disk: clean boot to the idle loop.
dd if=/dev/zero of=/tmp/ipod.img bs=1M count=64
./qemu-system-arm -M iPod-Classic -kernel ../assets/classic6g/osos_decrypted.bin \
    -display none -monitor none -serial null \
    -drive if=ide,format=raw,file=/tmp/ipod.img

# Which peripherals it asks for and which are missing.
... -d unimp,guest_errors -D /tmp/u.log

# The whole conversation with the ATA, which is how the protocol in §5 was extracted.
ATATRACE=1 ... 2>/tmp/ata.txt

# Where it is stopped.
(sleep 25; printf 'info registers\n'; sleep 2; printf 'quit\n') | \
    ./qemu-system-arm ... -monitor stdio
```

To find out whether a piece of code ever got executed — the most useful question of
the whole session — `-d in_asm` and search for its address:

```bash
./qemu-system-arm ... -d in_asm -D /tmp/asm.log
grep -c '^0x0805712c' /tmp/asm.log     # 0 = no block started there
```

> **And here is the trap, which cost a false measurement.** In this build
> `-d in_asm` does not disassemble (there is no capstone): it prints **a single address
> line per translation block**, the one at the start, followed by
> `OBJD-T: <bytes>`. Verified: `0x22003b1c` — the idle loop's `wfi`, which is
> executed thousands of times — shows up **zero** times, because its block starts at
> `0x22003b0c`.
>
> So `grep -c '^0xDIRECCION'` only answers "did this run?" if the address
> is a **block entry**: the target of a `bl`/`b`, or the `push` a function
> starts with. For any other it always gives zero and means nothing.
> Before using it, disassemble the address and check that there is a prologue.

---

## 8. What was learned about the method

- **`-d cpu` is useless here.** It generates 2 GB and 45 million lines in 25 s and doesn't
  even reach the first fault. To locate an exception, `-d int,in_asm`
  interleaved: the translation block printed right before
  `Taking exception` is the one that causes it.
- **`grep -c '^0xDIRECCION'` on an `in_asm` log** answers "did this ever
  run?" in a second, and that question reordered the entire investigation: without
  it we would have kept fixing the firmware partition parser, which
  never executes. **But it only works for block entries** — see the warning
  in §7. The first version of this page applied it to an address in the
  middle of a function and got a zero that meant nothing; as it happened,
  the conclusion was correct by another route. It is exactly the method error
  that [09-history-5g.md](09-history-5g.md) warns about: check the
  tool before believing the measurement.
- **A null pointer does not always fault.** On this machine VA 0 is mapped by
  the firmware itself, so `ldr r1,[r0]` with `r0 = 0` returns a word
  from the vector table and the crash happens later and somewhere else. A `DFAR`
  that looks like an ARM instruction (`0xEAxxxxxx`) is the clue.
- **Counting is necessary, but not enough.** "One pixel word in 45 s" is a
  fact; "so the framebuffer goes via DMA" was an inference with at least one other
  equally good explanation (that the firmware crashes before pushing pixels),
  and it slipped in as if it were a measurement. When a count comes out almost zero, the first
  hypothesis to rule out is "it never got to try". (Resolved: it was zero
  pixels and eight command parameters; see §3.)

---

## 10. `gdbpoke.py`, and why it changes the pace of work

`-d in_asm` only answers "was a block starting here translated?". The question
that was needed was "what is in `r0` when the guest reaches this address?",
and for that a debugger is needed. This WSL doesn't have `gdb-multiarch`, so
there is a new 200-line one that speaks the GDB remote protocol directly
to QEMU's `-gdb tcp::PORT`:

```bash
sh tools/ipod_classic/gdbrun.sh 1300 $HOME/6g/z64.img 90 \
   --break 0x0815b1e0 --break 0x0815b230 --hits 6 --mem 0x0bede7a4:16
```

`gdbrun.sh` starts QEMU with `-S -gdb`, waits and launches `gdbpoke.py`.
`QEMU_EXTRA` adds arguments (`-d int -D ...`). All of §9 and the correction to
§4 come from this tool; without it they would have been days of `-d in_asm`.

Besides `--break`/`--mem`/`--step` it has `--skip FROM=TO[:R0]`, which rewrites
the PC at a stop: it serves to **skip a call without touching the guest
image**, which is how it was shown that dodging the failure at `0x0815b230` doesn't
lead anywhere (§4).

> **A trap that cost an hour and that you need to know about.** QEMU does **not** have
> "ignore the breakpoint I just left" logic:
> `accel/tcg/cpu-exec.c:346` always fires when `pc == bp->pc`. A bare `c`
> from a stop stops again at the same place **forever**, and the
> first version of the tool got eight identical stops from a function
> with 374 callers without any alarm going off. The way out is three lines
> above in the same file: *"Singlestep overrides breakpoints"*. `cont()`
> does `s` and then `c`.
>
> And the control case that uncovered it: put the breakpoint on a function with
> many callers and check whether `lr` changes between stops. If it doesn't, the
> tool is broken, not the firmware.

Second trap, about addresses: **breakpoints are on virtual
addresses**. The firmware maps VA 0 onto IRAM0, so the vector table
executes at `0x00000000`, not at `0x22000000`, and the Data Abort handler is
at **`0x00003400`** (`[0x22000010] = 0xEA000CFA`). Putting the breakpoint at
`0x22003400` never fires.

## 12. Independent audit (August 2026, before there was an image)

> This audit **predates** §13/§14: its rows "the retailOS gives 0
> pixels" and "the hang with a disk is at `0x080aea1c`" describe the state at
> that time. The third audit, which does see an image, is in §16.

Everything above was measured again from scratch. What **is confirmed**:

| Claim | How it was checked |
|---|---|
| The panel draws | `lcdtest.S` → `screendump` → 76 800/76 800 pixels, 1920 colors, and **looking at the image**: a red ramp in X and green in Y with the cut at x=256 that the test's `and #0x1f` predicts |
| The retailOS gives 0 pixels, with and without a disk | `screendump` + `pixcount.py` in both configurations |
| With a disk there is no hidden exception | 40 s of `-d int` = 33 838 `Taking exception 5 [IRQ]` and nothing else; `R15 = 0x22003b20`, SVC |
| Without a disk, crash cycle | 28 `CPU_exception_handler` + 28 `SWRCON` in 25 s of stderr |
| Zero pixels through the data port | `LCDTRACE=1`: 29 starts × (3 CMD + 8 parameters), `0 were pixels` |
| …and it is **not** a model artifact | `-d guest_errors` gives **not one** "Invalid access": there are no 8/16-bit writes to the LCD that the `valid.min_access_size = 4` filter is dropping |
| The hang with a disk is at `0x080aea1c` | `gdbpoke.py`, two *breakpoints*: `0x0827a9ac` stops once (`r0=0x8be5d3e0`, `r2=1`, `r3=0x080aea1c`, `lr=0x0827a994`), `0x080aea1c` stops once (`lr=0x0827a9b0`) and in 120 s neither stops again |
| Nobody gets to look at the sector | `-d in_asm` with a disk: `0x080aee04` (MBR parser) and `0x080a38cc` (APM parser) = **0** blocks |
| `0x0805712c`, `0x0815b1b8`, `0x08239658` do not execute with a disk | `-d in_asm`, and the four addresses verified as prologues (`e92d….` = `push`) before believing the zero |
| GPIO pin 86 | `0x08325584` disassembled: `[GPIO_BASE + ((pin>>3)<<5) + 4] >> (pin&7)`, and `0x080aeca4` is `mov r0,#86; bl 0x8325584`. `GPIOTRACE=1` with a disk: `rd +0x144 -> 0x40`, i.e. pin 86 = 1 |
| `"Resources/Fonts"` at `0x08380628` | read from the file |
| `aupd_decrypted.bin` does not touch the LCD | `LCDTRACE=1`: zero accesses |
| No regression on the 5G | `navtest.sh`: 76 548 px in the 7 frames, the highlight moves (12 312, 12 114, 31 632) |

What **is corrected**:

1. "With a disk it only touches the LCD block twice" → it is **five writes**
   (§6).
2. "The three images give identical 682-line ATA traces" → 680 lines and
   one difference, and the difference is race noise, not from the disk (§9).
3. The header of `ipod_classic_gpio.h` said that pin 86 was "the whole reason"
   a boot with a disk did not draw. **It is not**, and §9 itself already
   said so: the announcement is dropped later at `0x080b2ce8` because `[0x08911ad8]`
   is still zero (verified at 45 s with `xp /1wx`). Corrected in the header.
4. `0x38900000` had been dismissed with the test "no register receives
   a DRAM address". It is the block with the identity gamma table and the
   panel geometry: **the best candidate there is** for the frame engine's
   source. Dismissal withdrawn and written up in §3.
5. The claim "where the `+0x70` engine reads from is undetermined" was
   correct but stopped too early: `0x08127f14` was eight
   instructions away from the two that were documented, and it shows that the drawing
   object **is not null** and that the drawing call **does happen** (§3).

## 11. Files from this batch

| Path | What it is |
|---|---|
| `tools/ipod_classic/findlit.py` | where a 32-bit literal is in the image, in VA, and which `ldr rX,[pc,#imm]` load it |
| `tools/ipod_classic/audit_run.sh` | boots the retailOS, does a `screendump` and summarizes stderr |
| `tools/ipod_classic/audit_peek.sh` | boots, waits and reads physical memory words via the monitor |
| `hw/arm/ipod_classic/ipod_classic_lcd.c` | now with a panel: 320×240 RGB565 memory, window, `0x2A/0x2B/0x2C`, `qemu_console`, and the frame engine registers `+0x70..+0x8C` |
| `hw/arm/ipod_classic/ipod_classic_gpio.c` | GPIO. A single pin modeled, 86, and the reason is in the header |
| `tools/ipod_classic/gdbpoke.py` | GDB remote protocol client: breakpoints, registers, memory, jumps |
| `tools/ipod_classic/gdbrun.sh` | starts QEMU with `-S -gdb` and drives it |
| `tools/ipod_classic/lcdtest.S` | panel control case: draws a 320×240 ramp |
| `tools/ipod_classic/pixcount.py` | counts non-black pixels in a `screendump` |

### From the August 10 batch (§13, §14)

| Path | What it is |
|---|---|
| `hw/arm/ipod_classic/ipod_classic_disp.c` | the compositor at `0x38900000`: registers with honest readback, the five layer descriptors and the composition onto the panel |
| `include/hw/arm/ipod_classic/ipod_classic_disp.h` | the entire register map, with the provenance of each line marked (V) verified in the disassembly, (M) also measured, (U) undetermined |
| `tools/ipod_classic/ppm2png.py` | converts a `screendump` to PNG **to look at it**, and also reads raw framebuffers pulled with `pmemsave` (`--raw 320x240:argb8888`) |
| `hw/arm/ipod_classic/ipod_classic.c` | DMAC0 and DMAC1 become `TYPE_PL080` with IRQ 16/17 and system memory as the master port |

### From the §18 batch

| Path | What it is |
|---|---|
| `tools/ipod_classic/uirules.py` | reads the **interface rule table** —(trigger, handler, actions)— from Apple's binary, with the format documented in its header. It is what turns "does MENU do anything on this screen?" into a query |
| `tools/ipod_classic/evtrace.py` | starts QEMU with a monitor **and** gdbstub, lets it boot freely, attaches, arms the breakpoints and **then** presses the key from a thread. `--str rN` follows a pointer and shows the string at the end |
| `tools/ipod_classic/tests/timer_audit.c` | bare-metal test of the timer block: measures three free-running counters against `USEC_TIMER` and isolates which writes reset the count |

---

## 13. `0x38900000` is the compositor, and that is where the pixels come from

§3 left it as "the best candidate". It was. It is the block that composes the
frame from layers in DRAM, and the LCD frame engine (`+0x70`) only
presents it.

### How we went from "793 addresses" to a register map

With the block falling to the *io floor*, all you could see was **which** addresses received
writes. Two steps opened it up:

1. **The twenty-seven `ldr rX,=0x38900000`.** A scan of all the PC-relative `LDR`s
   in the image whose literal falls in `0x389xxxxx` gives 27 sites
   for `0x38900000` plus five offset bases —`0x389000f0`, `0x38900180`,
   `0x3890031c`, `0x38900364`, `0x38900400`—, and disassembling those 27 yields almost
   the entire map. The list is **complete**, not "what showed up".
2. **The trace with the guest's PC.** `DISPTRACE=1` prints each access with
   `pc~`, and that found what step 1 could not: `+0x200..+0x214` are written
   from `0x081273f8..0x08127428`, which reaches the block via `r6` and not via a
   literal of its own. Both methods are needed.

### The map, with the evidence for each line

Everything is in `include/hw/arm/ipod_classic/ipod_classic_disp.h`, register by
register. What matters here:

| Register | What it is | How we know |
|---|---|---|
| `+0x008` | control register, six bitfields | six different functions read-modify-write it |
| `+0x058 + k*0x18` | **the five layer descriptors**, k = 0..4 | `0x08126ffc..0x0812716c`, a five-way `switch` that writes the same six registers |
| `+0x0d4` | **layer order**: the low byte picks one of the six stacking orders of layers 0..2 | 2.0.5: `0x0814482c` writes it; the per-layer setters map a logical layer through the permutation table at `0x083f35ec` (see the end of §18.11) |
| `+0x0d8/+0xe0/+0xe8` | three layer configurations | `0x080b0140` builds them as `(fmt<<28)\|(a<<16)\|(b<<12)\|c` |
| `+0x214` | `0x013f00ef` = 319 and 239, **the panel geometry** | literal at `0x08127468` |
| `+0x3ac` | `(w<<20)\|(h<<8)\|…`, bit 0 = rotated | `0x080bcef8`, and `0x08127f54` calls it with `(64,64,0,0,0)` → `0x04004000`, which is exactly what the boot writes |
| `+0x400..+0xffc` | three 256-entry gamma tables | `0x08127268` builds `tbl[i] = i*4` and `0x0806fe9c` writes it |

The layer descriptor, which is what draws:

```
+0x00  source stride in bytes
+0x04  (align<<24) | (fmt<<8) | ...   bit 16 set if NOT 32 bpp
+0x08  SOURCE ADDRESS, already advanced to the first visible pixel
+0x0c  (W<<16) | H     destination size
+0x10  W * bytes_per_pixel / 4
+0x14  (X<<16) | Y     destination position
```

And the bits-per-pixel table comes from the `switch` at `0x08126f90`, which converts the
format into a multiplier `r1 ∈ {1,2,4,8}` and computes the stride as
`r1 * srcW / 2` — i.e. `r1` is twice the bytes per pixel:
`fmt 0 → 4 bpp, 1 → 8, 2..5 → 16, 6..7 → 32`.

### What a real boot programs, measured

```
layer 0: stride 0x500 (1280 = 320*4), fmt 7 (32 bpp), source 0x09767a24,
         320x240 at (0,0)
layer 1: stride 0x280 (640 = 320*2),  fmt 3 (16 bpp), source 0x0be2326c,
         108x240 at (212,0)          <- 212 + 108 = 320 to the pixel
```

**And the image was there all along.** `pmemsave 0x09767a24 0x4b000` and
viewing it as 320×240 at 32 bits gives the whole language screen, already composited.
Nothing had to be invented: it just had to be copied to the panel.

`W` and `H` are **signed**: while the right-hand panel slides off the
screen the firmware writes `+0x07c = 0xffc900f0`, i.e. width −55. A
model that reads them as unsigned draws 65 481 columns of garbage.

### The byte order, which is the only INFERRED part of the path

The 32-bit source is read as *little-endian* ARGB8888 (bytes B, G, R, A) and
the 16-bit one as RGB565. The argument, in order of weight:

1. **The panel is RGB565 with red on top**, and that is not our claim:
   Rockbox's `firmware/export/config/ipod6g.h:85` says
   `#define LCD_PIXELFORMAT RGB565` for this exact device. So a 16-bit layer
   delivered as is is RGB565, and read that way the "Welcome" panel is the
   greyish-blue gradient the iPod really has.
2. **The 32-bit layer carries the same drawing already composited** — its right
   third *is* that gradient. Only the ARGB8888 reading makes the two match; the
   other one turns both brown.
3. And then the selection bar comes out as `0xff0484da` = **#0484da**, Apple's
   selection blue. The other way round it comes out as #da8404, an orange that appears
   nowhere in this interface.

It is still an inference — there is no *datasheet*, and the block has color
matrix tables with which it could permute channels. It is two lines of
`ipod_classic_disp.c` if better evidence turns up.

### When compositing happens, and why not at `+0x70`

The model composites in `gfx_update`, i.e. just before showing the panel, not when
`LCD[0x70] = 1` is written. The reason is a measurement: **with a disk, `LCDTRACE` counts
a single start of the frame engine in 45 s**, and afterwards the interface keeps
re-pointing the layer descriptors during the animation without touching
`+0x70` again. A model that only composited there would show a frozen frame.

It is a **modeling decision**, not a measurement: it behaves like an engine that
scans its layers continuously, which is what the register set looks like.
It is stated out loud in `ipod_classic_lcd_gfx_update()` and it is the
first thing to check if the screen is ever a frame behind.

### What is stored but not honored

The gamma tables, the four color matrix coefficient tables, the
scaler (`+0x04c/+0x50/+0x54`), the destination rectangle (`+0x030/+0x034`) and
the rotation bit at `+0x3ac`. **All of them are the identity in every measured
boot** — the gamma tables are literally `tbl[i] = i*4` —, so
honoring them today would not change a pixel and there would be no way to check whether they are
right. They are read and returned; nothing more.

---

## 14. Why the boot with a disk was stuck: DMAC0, not the ATA

**Fundamental correction to §9.** The sentence "the hole is between *the DMA finished*
and *wakes whoever requested it*" pointed at the right place but at the wrong
peripheral. The ATA completes its whole cycle: both DMA transfers
finish, the interrupt arrives and the ATA's RTXC semaphore is raised and wakes
whoever was waiting. What never comes back is a **later** `memcpy`, done by
DMAC0.

The chain, disassembled:

```
0x2200844c   memcpy via DMA (r0 = destination, r1 = source, r2 = 0x1000)
  0x22004808.., 0x220049c4.., 0x22004b64..   programs DMAC0 channel 0
  0x22008600   dma_wait(controlador, canal):
                 sema = [0x2200adec + ctrl*32 + canal*4 - 32]   -> 0x5f (95)
                 KS_wait(sema)        <- code 1, NO timer node
```

`KS_wait` without a *timeout* never expires. The only thing that can raise that
semaphore is DMAC0's terminal-count ISR, `0x220027b4`, registered at
`0x080357f4` on **IRQ 16**, entry `0x22002810`. And `0x38200000` was
`create_unimplemented_device`.

It was also doing silent damage: `0x22004b68` does a read-modify-write
on `C0Control`, and with the block returning zero the final write came out as
`0x0c000000` instead of carrying the `I` bit, the widths and the count. **A model that
only generated the interrupt without returning the reads correctly would still fail.**

They are real PL080s, and two sources agree on it: Rockbox names them
(`firmware/target/arm/s5l8702/dma-s5l8702.h:33-36`, "2 PL080 DMA controllers" at
`0x38200000` and `0x39900000`) and the offsets the retailOS uses match
byte for byte with `hw/dma/pl080.h`. QEMU already ships the model, so it was a matter of wiring it:

- a `0x1000` region, not `0x100000`, so the rest of the block keeps falling
  to the *io floor* and `-d unimp` reports it if the guest touches it;
- IRQ 16 and 17, which the guest already has unmasked
  (`VIC0 INTENABLE = 0x21a3010f`); †
- and the master port has to be system memory, because the guest
  programs the channels with the **cached** alias (`0x0be55320`) while the CPU
  runs on the uncached one (`0x8be55320`).

> † **Two different readings of `INTENABLE`, and they are not reconciled.** Here
> it is `0x21A3010F`; `ipod_classic.c:640` and `ipod_classic_wheel.h:149` say
> `0x01A3010F`, taken at another point in the boot. They differ **only in bit
> 29**. Neither conclusion depends on that: bits 16, 17 and 23
> are set in both (`3` in nibble 19..16, `A` in 23..20). Which of
> the two is the steady state is `UNDETERMINED`, and whoever measures it should correct it
> here and in the two places in the code.

### What got unstuck, measured

`-d in_asm` with `E.img`, counting only verified function prologues. All
these addresses gave **zero** in every previous trace of the project:

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

It is exactly the chain the two previous reports predicted, link by
link, and now it is measured end to end. That the screen has text in
nine different scripts is the independent proof: those fonts only
exist in `\Resources\Fonts\` of the disk's `rsrc` volume.

`0x080a38cc` (Apple Partition Map parser) is still at zero, and now for the right
reason: `E.img` has an MBR, so the other branch is taken.

---

## 15. How to reproduce the image

```bash
wsl -d Ubuntu-24.04 -e bash -c "cd /mnt/c/Desarrollo/qemu-ios/build && ninja qemu-system-arm"

# the second partition: an EMPTY FAT32. Nothing needs to be inside -- the
# fonts are in the FIRMWARE partition, not here (see the note below)
truncate -s 64M ~/6g/fat32.img
mkfs.vfat -F 32 -n IPOD ~/6g/fat32.img

# the disk, from the .ipsw, with MBR in 4096-byte units
python3 tools/ipod_classic/mkdisk6g4k.py ~/6g/E.img \
        assets/extracted/Firmware-24.9.1.2 ~/6g/fat32.img

# boot and measure
sh tools/ipod_classic/audit_run.sh px 45 -serial null \
   -drive if=ide,format=raw,file=$HOME/6g/E.img
#   => non-black: 75463 (98.3%) ; distinct colours: 404

# and LOOK at it, which is half of the measurement
python3 tools/ipod_classic/ppm2png.py /tmp/aud/px.ppm /tmp/px.png
```

> **Where the fonts come from, and it is worth saying because §4 can be
> misread.** The FAT32 volume in that recipe is **empty** (`mdir` lists
> nothing) and even so the screen comes out with text in nine scripts. Volume 4
> `rsrc` with `Resources/Fonts` **is not in the FAT32**: it is one of the four
> images in the firmware partition; `fwtool.py list
> assets/extracted/Firmware-24.9.1.2` shows it as
> `[0] rsrc off=0x00007000 len=0x04e00000` (81 MB). So the only
> real ingredient is the `Firmware-24.9.1.2` from the `.ipsw`; the FAT32 only has
> to exist so that the second MBR entry points to something. Checked by
> creating the FAT32 from scratch with the two commands above: same image,
> same 75 463 pixels.

Time to first pixel: between 22 and 30 s of wall-clock time on this machine
(measured with three `screendump`s at 22, 30 and 38 s: black, image, the same image).
After that it stays still: the interface animation finishes and there is no wheel
input to move it.

To see what the compositor does:

```bash
DISPTRACE=1 ./qemu-system-arm -M iPod-Classic ... 2>&1 | grep -E 'SOURCE|LAYER'
```

### Regressions checked

| Test | Result |
|---|---|
| `lcdtest.S`, the panel control case | 76 800/76 800 pixels, 1920 colors — **same as before** |
| retailOS **without** disk | 0 pixels and the same crash loop — same as before, and it is the guest's |
| `tools/ipod_video/navtest.sh` (the 5G) | 76 548 px in all 7 frames, the highlight moves (12 312, 12 114, ~31 600) |

> Note on the last number: it varies from run to run (31 632, 31 603,
> 31 588 in three passes) because the test presses keys by wall-clock time. The
> 76 548 and the two jumps of 12 312 / 12 114 are stable.

### Next steps, in order of cost

1. ~~**The click wheel** (`0x3C200000`, currently unimplemented). The guest
   polls it ~120 times per second, so someone is already listening; the model is missing.
   Without it the interface can be seen but not used.~~ **DONE, §17.** And the
   sentence was wrong: the guest does **not** poll. It does a single polled
   transaction at boot and then waits for **IRQ 23**. Corrected in §17.6.
2. **The boot without a disk** is still in the crash loop of §4, and it is now known
   to be unfixable that way: there are no fonts inside the image.
3. **The 4- and 8-bit layers** are not decoded (the palette is missing) and none has
   been seen in use. `-d unimp` will report it if they appear.
4. **The scaler and the color matrix**, if the guest ever stops
   writing the identity — for example when playing video.

---

## 16. Third independent audit (August 10, 2026)

Everything in §13, §14 and §15 was re-measured from scratch, with the tree as
it is and without trusting any previous figure.

### What is **confirmed**

| Claim | How it was checked here |
|---|---|
| **There is an image, and it is Apple's interface** | `audit_run.sh px 45` with `E.img` → **75 463/76 800 pixels, 404 colors**, and the image **looked at**: "Language" title bar with battery and lock, a list of nine languages including 日本語 and Ελληνικά, a blue selection bar over "English", and the "Welcome/Bienvenue/Willkommen" panel on the right. It is neither garbage nor a test pattern |
| Reproducible | Four different boots give the **same** 75 463; one of them with a disk **built from scratch** with the §15 recipe (`truncate` + `mkfs.vfat` + `mkdisk6g4k.py`) |
| **It draws and does not die** | 50 s of `-d int,guest_errors,unimp` with a disk: **0 `Taking exception 4`**, 0 `SWRCON`, 0 `CPU_exception_handler`, 43 840 IRQs and nothing else. The mode changes to `abt`/`fiq`/`und`/`irq` are one each, from the `msr cpsr` the firmware uses to set up per-mode stacks; they are not exceptions |
| Without a disk nothing changes | 30 s → **0 pixels**, 30 *Data Aborts* + 30 semihosting + 30 `SWRCON`: the same crash loop as §4 |
| Panel control case intact | `lcdtest.S` → **76 800/76 800, 1920 colors**, and the ramp looked at |
| No regression on the 5G | `navtest.sh` → **76 548 px** in all 7 frames and the highlight moves: 12 312, 12 114, 31 598 |
| The 4 KB window is enough | In the 50 s boot with `-d unimp` **not a single `s5l8702.io:` line appears**: zero accesses to the *io floor* during the whole boot |
| The layer descriptor *switch* | Disassembled by hand: jump table at `0x08127068`, five branches at `0x08127080/b0/e0/110/140`, all writing `base + 0x00/04/0c/10/14` |
| The bytes-per-pixel table | The *switch* at `0x08126f90` gives `r1 ∈ {1,2,4,8}` for `fmt` 0 / 1 / 2‑5 / 6‑7 and then `mul r2,r1,r2 ; lsr r2,r2,#1`, i.e. `r1 = 2 × bpp`. Matches `disp_bpp()` entry by entry |
| Bit 16 of `+0x04` | `cmp r1,#8 ; orrcc r3,r3,#0x10000` at `0x08127050`: set exactly when it is **not** 32 bpp |
| The §14 addresses are real entry points | `0x0805712c`, `0x080aee04` and `0x08040cf8` start with `push`; `0x081228d0` is not a prologue but **is** the head of a translation block (it is the fall-through target of the `beq` at `0x081228cc`), so measuring it with `-d in_asm` is valid |
| DMAC0/1 are PL080s and are on IRQ 16/17 | Rockbox, an independent source: `s5l87xx.h:1893`, `#define IRQ_DMAC(d) (16+(d))` |

### The missing control experiment: were both really needed?

§13 and §14 claim that the image needed the compositor **and** the PL080s.
The compositor is obvious, it is the only thing that draws; the DMAC0 part was an
inference over a long chain. It was checked by removing it:

> With the compositor in place and both PL080s removed, a 45 s boot with `E.img`:
> **0 pixels, 0 colors, and not a single `SOURCE ADDRESS` line**. The
> compositor is there and nobody ever gives it a source address.

In other words: **both were needed**, and §14 is not a nice story but a
cause. (The patch was a temporary `getenv`; it was reverted, it is not in the tree.)

### What is **corrected**

1. **The header of `ipod_classic_disp.c` said the opposite of what the
   file does**: "a register file… deliberately less than a composition
   engine… a compositor here would be inventing pixels", thirty lines
   above `ipod_classic_disp_compose()`. Rewritten.
2. **`ipod_classic_disp_compose()` said "no two layers have ever overlapped in
   a boot we have traced". That is false**, and it was exactly the fact supporting the Z
   order. Measured with `DISPTRACE=1`: layer 1 goes `(x=80,w=240)`, `(82,238)`,
   `(95,225)`, `(122,198)`, `(168,152)`, `(232,88)`, `(310,10)`, `(400,−80)`, with
   `x + w = 320` at every step, on top of a 320×240 layer 0 at (0,0).
   It is a **sweep** and always overlaps. Corrected, and with that the Z order goes from
   "no measurement" to "one measurement in favor": the sliding layer has to
   be **in front** or there is no transition.
3. **The LCD end-of-frame warning was lying.** `panel_frame_go()` was still
   emitting via `LOG_UNIMP` "no source address exists anywhere in this window
   and no DMA channel points at LCD_WDATA, so where it would read from is
   UNDETERMINED", which stopped being true in §13. Rewritten: what is still
   UNDETERMINED is not where it reads from, but whether `+0x70` **latches** a frame
   or only enables scanning.
4. **The source address counter was counting transitions, not writes.**
   `if (value != last_src)` with two slots that alternate prints one line per
   write, not "one per distinct value" as the comment said: it produced 21
   lines per boot to `stderr`, which is exactly the channel lesson 1 asks to
   keep clean. Now **the first** one is announced and that is it, and the final total
   counts **all** writes. Incidentally, those writes no longer skip the
   trace: `DISPTRACE=1` claimed "every access" and left out `+0x060` and `+0x078`
   because of an early `return`.
5. **`composed_frames` was incremented and never printed.** It now appears in
   the `LCDTRACE` totals, next to the frame engine starts.
6. **The header said the layer *switch* writes "the same six
   registers".** It writes **five**: `+0x08`, the source address, comes from
   elsewhere (`0x081276dc`). It is the difference between "this layer has geometry"
   and "this layer has something to read", which is exactly what distinguishes a
   boot without a disk from one with a disk.
7. **The §15 recipe was not reproducible**: it asked for a `~/6g/fat32.img` without saying
   how to make it. It is now documented, and with it a fact that had not been written down: that
   FAT32 is **empty** and it does not matter, because the fonts are in the
   `rsrc` image of the **firmware** partition, not in the FAT volume.
8. **`0x00001000` as the value of the three layer configurations** is from a
   boot **without** a disk. With a disk, `+0x0e0` and `+0x0e8` are rewritten 17 and 21
   times. Noted in the header.
9. **§3, §6, §9 and §12 described the previous state without saying so**, and §6
   flatly contradicts the §14 table (the same nine addresses, "never" in
   one and "1" in the other). Marked as historical.

### What **could not** be verified, and it must be said

- **Multi-layer compositing has never been seen in an image.** Layer 0
  alone already contains the whole screen: it was dumped with
  `pmemsave 0x09767a24 0x4b000` and `ppm2png.py --raw 320x240:argb8888`, and it is the
  same as what appears on screen. Layer 1, dumped the same way (`0x0be2326c`,
  `rgb565`), is only the gradient **without text**. So the 75 463 pixels
  prove slot 0 very well —address, *stride*, format and byte order— and
  **do not prove** the Z order or the clipping, because the layer 1 sweep lasts
  less than a second and no `screendump` has caught it (tried at 20, 21, …,
  28, 30 and 32 s: black until 28 s, the screen already still at 30 s). If someone
  wants to close this, the cheap way is a per-slot counter in `disp_draw_layer()` and
  a `screendump` triggered from the model itself the first time two layers
  overlap.
- **The wheel's "~120 polls per second"** is an older figure. In the
  boot with a disk in this audit there are 3 175 reads of `0x3C200000` in
  50 s, but with `-d int` on, which changes the wall-clock time. The conclusion
  ("someone is already listening") does not depend on the number.
- **The byte order is still INFERRED**, and clearly marked in the code. The
  image supports it in a way that is hard to argue with —the selection bar comes out
  `#0484da` and not `#da8404`—, but that is an argument about Apple's colors,
  not a *datasheet*.

---

## 17. The click wheel, and the screen that can finally be moved (August 10, 2026)

Up to here the retailOS **could be seen**. Now **it can be navigated**: the wheel moves the selection
bar through the language list, and it can be shown with a measurement that
distinguishes "an item moved" from "an icon blinked".

New pieces:

| File | What it is |
|---|---|
| `hw/arm/ipod_classic/ipod_classic_wheel.c` | the `0x3C200000` block, IRQ 23, and the keyboard handler |
| `include/hw/arm/ipod_classic/ipod_classic_wheel.h` | the register map and where each line comes from |
| `tools/ipod_classic/navtest.sh` | the test: boots, waits, presses, captures and decides |

And a one-line fix in `ipod_classic_pmu.c` that turned out to be worth as much
as the whole wheel model; see §17.4.

### 17.1 The register map

The base and the offsets are from Rockbox (`firmware/export/s5l87xx.h:1642-1651`,
`WHEEL_BASE 0x3C200000`, and `IRQ_WHEEL 23` at `:1901`). The **names**, on the
other hand, are almost all ours: Rockbox only names `WHEELINT`,
`WHEELRX` and `WHEELTX` by their function, and calls the other five `WHEEL00`, `WHEEL04`,
`WHEEL08`, `WHEEL0C` and `WHEEL10`. `WHEELCON`, `WHEELGO`, `WHEELDIV`,
`WHEELSTAT`, `WHEELMASK` and `WHEELPEND` are inventions of this port to say what
we believe each one does. The semantics, though, are from Apple's disassembly,
with `--adjust-vma=0x07FF51B8`.

| Off | Name | What the retailOS does | Status |
|---|---|---|---|
| `0x00` | `WHEELCON` | init `0x08056198`: `&=0xFFFF0000`, `&=~0x70000`, `&=~0x80000`, `&=~0x400000`, `\|=0x100000`, `\|=0x200000`. Before each TX it turns bit 21 off and on | sequence **VERIFIED**, bits **UNKNOWN** |
| `0x04` | `WHEELGO` | `\|= 1` launches the transfer (`0x0832797c`) | **VERIFIED** |
| `0x08` | `WHEELDIV` | `= 0x0003A980` once (`0x080561e4`) | value **VERIFIED**, meaning **UNKNOWN** |
| `0x0C` | `WHEELSTAT` | bit 0 "word available", bit 3 "TX in progress" (`0x08327994`) | **VERIFIED by use** |
| `0x10` | `WHEELMASK` | init `\|= 1`; saved/zeroed/restored around each polled transaction (`0x080afb48`) | mask **INFERRED**; see §17.4 |
| `0x14` | `WHEELPEND` | init `= 7`; the ISR writes 7 at the end (`0x08047a38`); the loop writes 1 to clear | **VERIFIED**, write-1-to-clear |
| `0x18` | `WHEELRX` | the received word (`0x080479bc`) | **VERIFIED** |
| `0x1C` | `WHEELTX` | `0x80000000 \| (cmd >> 1)`. Measured on the bus: `0xC000011D` | **VERIFIED** |

The four pins: the init calls `0x08325468(pin, 2, 0)` for `0x72..0x75`, which
is Rockbox's `PCON14 = (PCON14 & ~0x00ffff00) | 0x00222200`
(`button-clickwheel.c:386`). Confirmed on the bus: the GPIO model logs
`0xe0202/0xe0302/0xe0402/0xe0502` at `+0x200`.

### 17.2 The two packets

**The one that moves the interface** — the ISR `0x080479a8` accepts it if
`(rx & 0xBC0000FF) == 0x8000001A`:

```
 31    30        25..16          12..8        7..0
 +---+--------+---------------+-----------+-----------+
 | 1 | finger |   position    |  buttons  |   0x1A    |
 +---+--------+---------------+-----------+-----------+
```

Rockbox describes the same packet with the looser mask `0x800000FF`
(`button-clickwheel.c:121`): two independent sources in agreement.

**The reply to a polled command** — validated at `0x0832783c` with
`bic #0x7F000000` + `bic #0x00FF0000` + `cmp`: bit 31 and bits 15..0 equal to the
command, bits 30..16 free for the data.

Buttons, bits 8..12 of the packet, and the key code that `0x0805c11c` emits:
SELECT→3, forward→1, back→2, PLAY→4, MENU→0. The bits are Rockbox's.

### 17.3 Why the model paces itself with a timer

The rotation decoder `0x0807a670` imposes three hard conditions:

1. **96 positions per revolution** (the `±96` at `0x0807a6a4`, and Rockbox's
   `WHEELCLICKS_PER_ROTATION`).
2. **Two consecutive packets with bit 30 set** are needed before any
   delta is computed: the first one only arms.
3. The threshold goes back to **6** with each packet without a finger, and the accepted window is
   **−35..+35** (`0x0807a6dc`).

From 2 comes the design decision that is not obvious: **packets are
spaced out with a timer, one every 40 ms of virtual clock, instead of being
queued all at once**. The ISR overwrites `0x089116d8+0x20` on each packet; if
two come in before the wheel task runs once, the arming packet
disappears and the movement is read as a first touch and dropped. A FIFO is not
enough: the guest needs to see each packet as its own interrupt.

A key press is `IPOD_WHEEL_STEP` positions, **6** by default, which
is not a round number chosen by eye but the threshold the firmware itself
has at `0x089116d8+0x04` at rest, read via the monitor.

### 17.4 The two traps, and the second one cost an afternoon

**First: `WHEELMASK` has to read back honestly.** The firmware saves it,
zeroes it around a polled transaction and restores it. While the
block was `create_unimplemented_device` every read gave zero, so what it
restored was **zero**, and you can see it in the log: twelve writes of `0` to `+0x10`
versus a single one of `1`. Without honest readback the interrupt stays masked
forever and no press ever arrives.

**Second, and it is the 5G's `HI_IRQ` lesson again: the PMU's HOLD
SWITCH.** With the wheel already modeled, turning worked and **the buttons did not**.
Measured, with the key held for three seconds and reading the firmware's own
status block:

```
0x089116d8+0x1C = 1   the packet arrived and the ISR decoded it
0x089116d8+0x14 = 1   the wheel task ran the edge detector
                      0x0805c0e4 and called the dispatcher 0x0803f390
```

...and the screen did not change. The cause is inside the dispatcher, at
`0x0803f3e0` —the function's **21st** instruction, after the chain
of four comparisons that remaps the key code; an earlier version of
this paragraph said "one instruction after the entry" and was false—.
`0x0803f3e0` calls `0x08030d48`, which is a bare `b 0x080634a4`, and from there
`0x08294f80`, which reads **one byte of PMU register `0x87` over I2C**
(`mov r0,#0x87` / `and r0,r0,#2` / `lsr r0,r0,#1`) and keeps
`(byte >> 1) & 1`; `0x080634a4` returns `1 − that` (`rsbs r0,r0,#1` /
`movcc r0,#0`); and `0x0803f3e4` **returns immediately if it is nonzero**
(`cmp r0,#0` / `popne {r4,r5,r6,pc}`). With the register at zero, every button was
silently dropped.

(And there is a second gate right behind it, at `0x0803f3ec`, which also does
`pop` if a byte from another block is zero. It doesn't get in the way —the buttons work—
but it is worth knowing it is there before blaming the hold switch the
next time.)

Rockbox says what that bit is, and arrives at the same polarity from the other side
(`firmware/target/arm/s5l8702/ipod6g/pmu-6g.c:244`):

```c
pmu_input_holdswitch = !(pmu_read(PCF50635_REG_GPIOSTAT)
                            & PCF50635_GPIOSTAT_GPIO2);
```

That is: **bit set = hold off**. The fix is one line in
`ipod_classic_pmu_reset()`, marked as a CONVENIENCE VALUE: a machine with no
physical switch to move has to boot with it off or it cannot be
used.

And the moral, which is the same as with `HI_IRQ`: **turning does not go through
`0x0803f390`**, so it was not affected. The failure looked like the wheel's and was
the PMU's. Checking the whole path means checking it for each type of
event, not once.


### 17.5 What is seen, measured and looked at

`sh tools/ipod_classic/navtest.sh`, with `~/6g/E.img`. Keys: `q`/`e` turn,
Enter selects, `w` is MENU, `a`/`d` are back/forward, `s` is play — the
same as the 5G, plus the two that were missing, so that a single habit works for
both machines.

```
00-idle             75463 px lit
01-idle-again       75463 px lit        0 px differ
02-wheel-fwd        75432 px lit     8000 px differ (8000 in the list, 0 in the pane)
03-wheel-fwd2       75463 px lit     8000 px differ (8000 in the list, 0 in the pane)
04-wheel-back       75432 px lit     8000 px differ (8000 in the list, 0 in the pane)
05-select           76800 px lit    50540 px differ (38400 in the list, 12140 in the pane)
06-select-later     76800 px lit        0 px differ
07-menu             76800 px lit        0 px differ
```

That run is from a machine with a clean boot (zero reboots) and **passes
everything but the last line**: `10-menu-back` missed the MENU press, which
is the input failure of §18.8 and not the compositor's. **In two other runs
of the same test the last line passed as well**, and with the measurement that matters:

```
10-menu-back   0 px different from 08-menu-wheel-back  -> MENU WENT BACK TO THE SAME SCREEN
10-menu-back  23 px different from 08-menu-wheel-back  -> MENU WENT BACK TO THE SAME SCREEN
```

**Zero and twenty-three pixels** —the 23 are the battery icon from §18.7—
against the 6 088 it costs to change screen. It is not "many pixels
changed": it is **the same frame it left from**.

And **looked at**, which is the other half of the measurement:

- `00-idle`: the language list with the blue bar on **English**.
- `02-wheel-fwd`: the bar on **日本語**. One press of `e`, one item.
- `04-wheel-back`: `q` brings it back to **日本語** from **Čeština**.
- `05-select`: **a whole other screen** — the musical note icon on the
  gray gradient and the text **「ミュージックが…」**. That is: the selection not
  only changed screen, it changed to the **language that was highlighted**. That
  coincidence is what turns "many pixels changed" into "the interface
  did what it was asked".

The **8000 pixels** are exactly what it costs to move the bar: two 160×25
boxes turning on and off. The noise floor of this screen is
**0** —`01-idle-again` checks it on every run before believing anything—, so
there is no way to confuse it with the status bar clock, which this screen
doesn't have anyway.

On the model side, in the same run:

```
WHEELTRACE first command: tx 0xc000011d -> cmd 0x8000023a
WHEELTRACE first packet queued: 0x8000023a
WHEELTRACE irq 23 raised for the first time (pend 0x1, mask 0x1)
WHEELTRACE totals: 14 packets queued, 0 dropped with the FIFO full,
                   1 commands, 13 rising edges on IRQ 23
```

Three things to say about that:

- **`pend 0x1, mask 0x1`**: the mask is 1 and not 0. It is the direct proof
  that honest readback of `WHEELMASK` was what was missing (§17.4).
- **`1 commands`**: before there were **five** `WHEELTX` writes, the five
  retries of `0x0832780c` against a mute block. Now the first one gets an answer.
- **`0 dropped`**: the FIFO never filled up, i.e. the guest drains at the rate
  the model produces. That counter is there precisely so that, if some
  day the ISR stops running, it says so instead of leaving us guessing.

Repaints: **about 420** writes of layer 0's source address in the
run (417 in the patch run, 419 in the audit run: the count depends on
where the capture lands within the double buffer and is **not** an exact number to
reproduce), versus **10** in an idle run. What matters about the figure is
the order of magnitude: forty times more than at rest.

The eight `WHEELTRACE` counters, on the other hand, **are** exact and match
the keys one by one: 14 packets = 1 polled reply at boot + 3 turns
× (arm + movement + finger off) + Enter (press and release) + `w` (press and
release). And 13 rising edges = 14 − 1, because the boot packet is
queued with `WHEELMASK` at zero and that one doesn't raise the line. Reproduced identically in
the audit.

> **That "press and release = two packets" stopped being true in §18.8**, and for
> a measured reason: two packets per press lost the entire press
> whenever the wheel task did not run between them —six times out of ten—. Now
> the button state is **repeated** while they are pressed and for
> 400 ms after each edge, so the packet count of a run
> with keys is considerably higher and can no longer be predicted by hand. Turns don't
> change.

### 17.6 Corrections to what was written earlier

1. **§6, §15 and §16: "the guest polls the wheel ~120 times per second, so
   someone is already listening".** Withdrawn. The firmware does **a single**
   polled transaction at boot (with five retries) and from then on it is
   **purely interrupt-driven**, IRQ 23: it registers `0x080479a8` at
   `0x08047a68` and its task goes to sleep forever in `wait_event(5)`. The thousands
   of `+0x14` reads that were being counted were the wait spin of that single
   transaction against a register that returned zero. The conclusion back then
   ("someone is already listening") was correct by coincidence; the mechanism was not. A
   model that only returned data in `WHEELRX` without raising the line would have
   produced **not a single** event.
2. **§15, "next in order of cost", item 1.** Done.

### 17.7 What is still unknown

> **RESOLVED in §18, and the first two items on this list were wrongly
> framed.** "The screen that follows choosing a language" was **the main menu
> with the list covered**: the guest drew it in full and our compositor put
> on top of it a layer the guest had already turned off. And the
> "Welcome" panel **has no rule** telling it to follow the highlight: the one
> cited here belongs to another screen. They are left written as they were, with what was
> believed then, because §12 lives off that.

- **MENU does not go back from the screen that follows choosing a language.** Measured
  twice, with waits of 15 and 25 s, and in both: **0 pixels**. Whether it is
  because that first-boot screen has nowhere to go back to, or because
  something is missing, is **UNKNOWN**. What is measured is that the key arrives
  intact: with `gdbpoke.py` and breakpoints along `0x0803f390`, the
  remapped code (`0x6f` for SELECT) passes through the dispatcher, reaches
  `0x08170e94`, builds the `'kDwn'` message and puts it in the
  EventManager queue, and the release does the same with `'kUp '`. The event arrives; what
  the screen decides to do with it is up to the application. **It is the missing milestone.**

  **Measured in the audit, and it narrows the search considerably:** that screen **is not
  deaf**. With the same run, after Enter:

  ```
  04-select        76800 px    the musical note and 「ミュージックが…」
  05-post-wheel    76800 px   50307 px differ      <- one `e`, the wheel
  06-post-menu     76800 px       0 px differ      <- one `w`, MENU
  07-post-select   76800 px       0 px differ      <- another Enter
  ```

  The **wheel does move it**: the note disappears and the plain gradient remains. And it
  is not that the animation carries on by itself, because in the `navtest.sh` run that same
  screen stays **identical** for 40 s without touching anything (`06-select-later`, 0 px).
  That is: the input layer delivers **turns and buttons** to that screen, it
  reacts to the turn and **decides to do nothing** with MENU or with a second
  Enter. That points to the screen being the first-boot assistant's and
  having nowhere to go back to, not to something missing underneath — but it is still
  **INFERRED**, not verified.

  And on the **language screen** MENU doesn't do anything either (0 px), nor does `a`
  (left/previous): also measured in the audit, with 8 s waits.
- **The right panel does not change when turning** (0 pixels on the right in all three
  turns). The rule `list.pid.….delayedselected1 -> ShowSetting_Language` says
  it should. That it doesn't fire points to the timed half of the interface
  —`TimerEventManager`, `0x081059c4`— and it is the most promising thread left.
- **Bits 1 and 2 of `WHEELPEND`/`WHEELMASK`**, the meaning of each bit of
  `WHEELCON`, the `0x3A980` of `WHEELDIV` and the command `0x8000062A`: all
  **UNKNOWN**, and none is needed.
- **Why the real hardware accepts both `WHEELTX` encodings**
  (Rockbox's `0x8000023A` and Apple's `0xC000011D`): **UNKNOWN**. The model
  accepts both and says so in the code.
- **The real pace of the wheel** (the timer's 40 ms) is a tuning
  constant, not a fact about the device. `IPOD_WHEEL_TICK_MS`, `IPOD_WHEEL_UNTOUCH_MS`
  and `IPOD_WHEEL_STEP` let it be moved for experimentation.

### 17.8 Regressions checked

| Test | Result |
|---|---|
| `tools/ipod_classic/navtest.sh` | the three turns give 8000 px each, and the idle floor is still 0 |
| retailOS at rest, two consecutive captures | 0 px of difference, same as before the wheel |
| **`tools/ipod_video/navtest.sh` (the 5G)** | **76 548 px in the seven frames, and the highlight moves: 12 312, 12 114, and ~31 600** (31 602 in the patch run, 31 609 in the audit run). The first two are stable; the third **is not and cannot be**, because the 5G screen that Enter goes into does have a clock —§15 records it as "~31 600" for that reason—. No regression |
| The `0x3C200000` block is no longer in `ipod_classic_unimplemented[]` | the modeled window is 4 KB, not 1 MB: the rest of the block still falls to the *io floor* and `-d unimp` would say so. It doesn't |
| `stderr` of the whole run | not one exception, not one reboot, not one `unimp`: leaving out `DISPTRACE` and `WHEELTRACE`, the run prints **four** lines and all four are informational. Navigating and dying would not be navigating |

### 17.9 How to reproduce it

```bash
wsl -d Ubuntu-24.04 -e bash -c "cd /mnt/c/Desarrollo/qemu-ios/build && ninja qemu-system-arm"

# the disk, same as in sec.15
truncate -s 64M ~/6g/fat32.img && mkfs.vfat -F 32 -n IPOD ~/6g/fat32.img
python3 tools/ipod_classic/mkdisk6g4k.py ~/6g/E.img \
        assets/extracted/Firmware-24.9.1.2 ~/6g/fat32.img

# the whole test: boots, waits, checks that the screen is still,
# presses, captures and decides. Exits 0 if the three turns moved the highlight.
sh tools/ipod_classic/navtest.sh

# and LOOK at the PNGs it leaves in ~/6g/nav, which is the other half
```

By hand, to play around:

```bash
cd build && ./qemu-system-arm -M iPod-Classic \
    -kernel ../assets/classic6g/osos_decrypted.bin -serial null \
    -drive if=ide,format=raw,file=$HOME/6g/E.img -snapshot
# q / e turn the wheel, Enter selects, w is MENU, a / d back / forward,
# s is play. WHEELTRACE=1 counts packets, commands and interrupts.
```

---

## 18. The missing gate: the layer enable bits, and the menu that does go back (August 10, 2026)

The two symptoms left open in §17.7 —**MENU does not go back** and
**the right panel does not follow the highlight**— had **a single cause**, and it was
in our model, not in Apple's firmware:

> `ipod_classic_disp_compose()` drew **every layer that had a source
> address**. The guest, when finishing a transition, **turns the layer off but leaves
> the whole descriptor in place**. From the first screen change on, **layer
> 2** —320×240 at (0,0), source `0x0bfb4ff8`— stayed on top of layer 0
> **forever**, and covered the entire interface. The keys worked; what
> they did could not be seen.

With the gate in place, the retailOS **can be navigated**: main menu in the chosen
language, the wheel moves the highlight **and the right panel follows it**, SELECT
enters Music and **MENU returns to the exact frame that was left**.

### 18.1 The measurement that uncovered it, and why none of the earlier ones could

The earlier investigations measured **what the screen showed**. This one
looked at **what the guest had drawn**. With the screen showing the musical
note and "No Music" over a plain gradient —no list and no title bar—:

```bash
(qemu) pmemsave 0x08000000 0x04000000 "/tmp/dram.bin"
# the two buffers that layer 0 alternates between, taken from the dump:
python3 tools/ipod_classic/ppm2png.py fb0.bin fb0.png --raw 320x240:argb8888
```

`0x09767a24` and `0x089d42d4` contained **the complete main menu**: the
"iPod" bar with the battery, the list (Music, Videos, Photos, Podcasts, Extras,
Settings, Shuffle Songs) with Music highlighted and its arrow, and the right panel
with the note and "No Music".

And `0x0bfb4ff8`, the source of **layer 2**, contained exactly the frame
that was visible: the gradient with the note and the text, **without the list and without the title
bar**.

In other words: **the guest was drawing correctly; we were covering it up.** That turns "MENU does
nothing" into "MENU does something, and it is not visible", which is a different fault from a different place.
Method trap no. 4 —counting accesses without looking at the values they carry— has
its sibling here: **looking at the screen without looking at the buffer**.

### 18.2 Where the gate is, with two independent proofs

**1. The writer, disassembled.** `0x081271c8` receives an index in `r1` and a
boolean in `r2`, jumps through the table at `0x081271d8` and does `bic` / `orrne`
on the word it read from `+0x008`, storing it at `0x08127250`:

| index | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| bit | `0x40` | `0x20` | `0x10` | `0x08` | `0x04` | `0x80` |

**2. The order of the writes, traced with `DISPTRACE=1`.** Always the same: the
descriptor is programmed, the **source address** is written, and only then
is the bit turned on:

```
+0x060 = 0x089d42d4  (layer 0)  ...  +0x008: 0x41118101 -> 0x41118141   bit 6
+0x078 = 0x0be2326c  (layer 1)  ...  +0x008: 0x41118141 -> 0x41118161   bit 5
+0x090 = 0x0bfb4ff8  (layer 2)  ...  +0x008: 0x41118161 -> 0x41118171   bit 4
```

and when the transition ends it turns them off —`0x41118151`, then `0x41118141`— **without
touching either the source address or the size**. A screen at rest stays
at `0x41118141`: **layer 0 alone**, which is exactly the one carrying the good
frame.

**VERIFIED** by both routes and by the result: compositing only layer 0
shows the main menu, which is what the guest had drawn.

Not to be confused with **bits 16, 20 and 24**, which also look like one per layer and
which the previous version of `ipod_classic_disp.c` ruled out **rightly**: those are
set once at init and never move. The gate is bits 4..6, which
move all the time. The comment back then said "and nothing else gates
it"; it was that sentence that was wrong, not the ruling-out.

### 18.3 The fix

`hw/arm/ipod_classic/ipod_classic_disp.c`, in `disp_draw_layer()`:

```c
if (k < S5L8702_DISP_NLAYER_GATED &&
    !(s->reg[S5L8702_DISP_CONTROL >> 2] & S5L8702_DISP_LAYER_EN_BIT(k))) {
    return false;
}
```

with `S5L8702_DISP_LAYER_EN_BIT(k) = 1u << (6 - k)`. Layers 3 and 4 are **not**
filtered: we do not know which bit governs them, no boot has ever written their
descriptors, and that is why they drop out earlier at the `src == 0` check. It is
noted in the code.

### 18.4 So what was really going on with MENU?

Two different things, and they have to be separated.

**On the first-boot language screen, MENU must do nothing, and Apple
wrote it that way.** A new tool, `tools/ipod_classic/uirules.py`, which reads
the interface rule table directly from the binary:

```
$ python3 tools/ipod_classic/uirules.py assets/classic6g/osos_decrypted.bin \
      --grep button.menu.up --raw
0073e628 SettingsMenus_ResetAllSettings_Language_Layout  button.menu.up1 ->
         (no handler)  (no actions)
```

Handler of length 0, zero actions: an **empty** rule. In the whole image
there are **108** `button.menu.up` **rules**; **62 do something and 46 do not**. Measuring MENU there
measures nothing. **VERIFIED** by reading the bytes by hand at
`0x0073e620..0x0073e71d` before writing the parser, and the whole format
is documented in the header of `uirules.py`.

**On any screen that does have somewhere to go back to, MENU goes back.** Measured in
§18.6.

### 18.5 Same with the right panel

The rule that §17.7 considered unfulfilled,
`list.pid.….delayedselected1 -> ShowSetting_Language`, **does not belong to this
screen**: it belongs to `list.pid.229444534`, the "Language" item of the **Settings
menu**. The first-boot list is `list.pid.229444595`, which appears
**only once in the 9.9 MB of the image** and **only** with `.chosen1`:

```
$ uirules.py osos_decrypted.bin --grep 229444595 --raw
0073e6be SettingsMenus_ResetAllSettings_Language_Layout
         list.pid.229444595.chosen1 -> HandleLanguageAfterReset
         navigator.PopToMainScreen()
```

There is **no** rule saying that the "Welcome" panel follows the highlight.
**VERIFIED**, exhaustive search.

Where the firmware **does** say so is in the **main menu**: 48
`list.pid.2294433NN.delayedselected` rules, 24 of them with
`navigator.SwitchLayout(MainMenus_Main_Screen_XXX)` and the rest with
`Handle…Hilited` handlers. And there, now, **it follows**: one wheel click moves the bar
about 7 950 px in the list **and** repaints about 7 200 px of the panel, which goes from the
musical note of "ミュージック" to the film strip of "ビデオ". Looked at, not
just counted.

And incidentally: `delayedselected` is the **timed** half of the interface, the one
§17.7 suspected was dead. It is alive, and now it shows.

### 18.6 What is visible now, measured and looked at

`sh tools/ipod_classic/navtest.sh`, extended to reach the main menu and
test MENU there:

```
00-idle               75463 px lit
01-idle-again         75463 px lit        0 px differ (0 in the list, 0 in the pane)
02-wheel-fwd          75432 px lit     8000 px differ (8000 in the list, 0 in the pane)
03-wheel-fwd2         75463 px lit     8428 px differ (8428 in the list, 0 in the pane)
04-wheel-back         75432 px lit     8000 px differ (8000 in the list, 0 in the pane)
05-select             76074 px lit    19503 px differ (12842 in the list, 6661 in the pane)
06-menu-idle          76074 px lit      875 px differ (0 in the list, 875 in the pane)
07-menu-wheel         76046 px lit    15028 px differ (7952 in the list, 7076 in the pane)
08-menu-wheel-back    76074 px lit    15151 px differ (7975 in the list, 7176 in the pane)
09-into-music         75802 px lit     7045 px differ (6088 in the list, 957 in the pane)
10-menu-back          75802 px lit      933 px differ (0 in the list, 933 in the pane)   <- MENU was lost in THIS run

  floor of the language screen: 0 px (limit 200)
  floor of the main menu LIST: 0 px (limit 200)

VERDICT
  02-wheel-fwd           8000 px  -> the highlight moved
  03-wheel-fwd2          8428 px  -> the highlight moved
  04-wheel-back          8000 px  -> the highlight came back
  05-select             19503 px  -> SELECT changed the screen
  07-menu-wheel        list  7952 px + pane  7076 px  -> the highlight moved AND the pane followed it
  08-menu-wheel-back   list  7975 px + pane  7176 px  -> the highlight moved AND the pane followed it
  09-into-music          7045 px  -> SELECT entered Music
  11-menu-at-top       list     0 px + pane   875 px  -> did not navigate, which is correct
```

And **looked at**, which is the other half of the measurement:

- `05-select`: the **main menu** in Japanese —ミュージック, ビデオ, 写真,
  Podcast, エクストラ, 設定, 曲をシャッフル— because 日本語 was the highlighted
  language. It is the same "it did what it was asked" proof that SELECT
  already gave. In another run, with **Čeština** highlighted, it came out as *Hudba, Videa,
  Fotografie, Podcasty, Dodatky, Nastavení, Náhodná hudba* and the panel said
  *Žádná*.
- `07-menu-wheel`: the bar on **ビデオ** and the right panel with the **film
  strip**.
- `09-into-music`: the **Music** screen (Cover Flow, プレイリスト,
  アーティスト, アルバム, コンピレーション, 曲, ジャンル, 作曲者,
  オーディオブック).
- `10-menu-back`: **the main menu again**, and the same frame as
  `08-menu-wheel-back`.

The strong verdict is not the size of the change but **where it goes**:
`10-menu-back` is compared against `08-menu-wheel-back`, the frame that was
left. "Many pixels changed" does not prove that it goes back; "it returned to the
frame we left" does.

### 18.7 The noise floor: two things move on their own, and both should

`navtest.sh` failed the run if two consecutive captures of a still
screen differed at all. That no longer holds as-is, and both reasons are measured:

1. **The battery icon**: **23 px** at `x=145..151, y=4..13`, with the charging
   green among the colors. It repaints whenever it feels like it: one run of this
   script measured 0 px there and the next 23.
2. **The main menu panel's label**: about **960 px** at
   `x=184..293, y=165..182`, i.e. the **two lines of text under the icon**.
   The Japanese label 「ミュージックがありません」 does not fit on one line and **scrolls
   on its own**. The left half of those same frames differs by
   **0 px**.

So the floor is checked against a **200 px limit** instead of against
zero, the measured value is **always printed**, and the main menu is levelled
**by its list only**. Moving the highlight costs 8 000 px, forty times the
limit, and the lowest threshold in the whole verdict asks for 3 000: there is no overlap.

### 18.8 The other half of the problem: a button that was sent ONCE, and a loss that remains open

With the screen now visible, something came to light that could not even be measured before:
**SELECT and MENU are sometimes lost**. A dedicated test —enter Music and leave,
five times, one press every 13 s, `~/6g/inout`—:

```
run 1 (model at the time)        in1 6343 | out1    0 | in2    0 | out2 6343
                                 in3    0 | out3    0 | in4 6343 | out4 6343
                                 in5    0 | out5    0
WHEELTRACE totals: 23 packets queued, 0 dropped, 1 commands, 22 rising edges
```

**23 packets is exactly what is expected** —1 from the boot probe + 11
presses × 2 edges— and **0 dropped**. The model delivered everything and the
guest heeded four of eight presses that had a destination. Nothing in the model
distinguishes the ones that worked from the ones that did not.

**What was found and has been fixed** is *constraint 2* of §17.3
applied to the buttons, which was the half that had been left exempt:

> The ISR overwrites the same status block on every packet. A press
> was **exactly two packets**, one with the bit set and another with the bit
> cleared. And the retailOS acts **on release** —its rules are called
> `button.menu.up` and `button.select.up`—, so it needs the wheel task
> to run and see the bit **set**, and run again and see it **cleared**.
> If the task does not run in that gap, the whole press disappears without a
> trace.

The real device does not have that problem because it **streams**: while something
is happening it reports at its own pace, and the button state goes in **every**
packet. Now the model does too (`S5L8702_WHEEL_BTN_REPEAT_MS`, 400 ms, ten
ticks; adjustable with `IPOD_WHEEL_BTN_REPEAT_MS`): it repeats the state one packet
per tick while a button is pressed and during the window after each
edge, so that the rising edge gets the same treatment as the falling one.

Two more runs, now with the repetition:

```
run 2       in1 0 | out1 0 | in2 0 | out2 0 | in3 0 | out3 0
            in4 6343 | out4 6343 | in5 6343 | out5 6343      (134 packets, 0 dropped)
run 3       in1 6343 | out1 6665 | in2 0 | out2 322 | in3 6343 | out3 6343
            in4 6343 | out4 6343 | in5 0 | out5 0            (134 packets, 0 dropped)
```

Counting **only the presses that had somewhere to go**:

| | SELECT from the main menu | MENU from the Music screen |
|---|---|---|
| without repetition (run 1) | 1 of 3 | 2 of 3 |
| with repetition (runs 2 and 3) | 5 of 10 | **5 of 5** |

**The honest thing is to say both.** The repetition **eliminates a failure mode
that the model demonstrably had** —two packets per press against an ISR
that overwrites its status block— and **MENU has not failed even once** since
it has been in place. But **it does not fix the SELECT loss**, which is still there, and therefore
**the loss has another cause that has not been found**. It is not being sold as
fixed.

**The number-one suspect for what remains**, and it is where I would look next:
the gate from §17.4. The key dispatcher reads the PMU's register `0x87` over I2C
**on every key** and **returns without doing anything** if the bit comes out non-zero
(`0x0803f3e0` → `0x08294f80`, and the `popne` at `0x0803f3e4`). A read
that returns something else —the PMU model has a register pointer that
auto-increments and is shared between transactions, `ipod_classic_pmu.c:131`— drops
the key silently and with exactly this look. The experiment: a breakpoint
at `0x0803f3e4` with `evtrace.py`, read `r0` on each press and see whether
the lost ones are those carrying `r0 != 0`. **It has not been done.**

Immediate practical consequence: `navtest.sh` can fail because of this without
anything having broken. If it fails **only** at `09-into-music`, repeat it before
looking for culprits.

### 18.9 A method trap that cost two runs: orphaned QEMUs

Two consecutive runs of `navtest.sh` failed with the screen black and
**116 and 68** lines of `guest asked for a software reset (SWRCON = 0xaa5),
next image "diskmode"` on `stderr`. It was not the patch. It was this:

```
$ ps aux | grep qemu
... 24 qemu-evt processes from an earlier session, at 80 % CPU each
$ uptime
load average: 16.00     # on 12 cores
```

`navtest.sh` does `pkill -9 -x qemu-system-arm`, which by design does **not** touch a
binary named differently —the private copy `~/evt/qemu-evt` used by
`evtrace.py` exists precisely so that one session does not kill another—. The price
is the symmetric one: **orphans are not cleaned up on their own**. With the machine that heavily
loaded the guest does not meet its own boot deadlines in time and
reboots into *diskmode* in a loop.

Before believing an intermittent failure: `ps aux | grep qemu` and `uptime`.

**But not everything is load, and this is new.** With the machine empty —load 0.4, a
single QEMU, 7 GB of free memory— two consecutive runs rebooted **1 and 3
times**, and the reboot falls **between 48 and 52 s**, i.e. **before the
first key**. The footprint in the captures is exactly the one the
events investigation described without naming it: where the language list
should be, the **Low Battery** screen appears (76 797 px) and shortly afterwards **black**
(0 px). Now it has a name, because `stderr` says it:

```
qemu-system-arm: warning: S5L8702: guest asked for a software reset
                 (SWRCON = 0xaa5), next image "diskmode"
```

In other words: **the "Low Battery" screen at 46 s and the reboot into disk mode are
the same event**. It remains unexplained —the PMU's ADC returns 844 of 1023,
which is not low battery (`ipod_classic_pmu.c:47`), so the trigger is something
else—, but it is no longer a mystery without edges: it is a concrete guest path
that decides to reboot into `diskmode`, and the place to start is which PMU
register it reads before deciding so.

That is why `navtest.sh` now counts those reboots and **warns loudly** when there is
any: a run that reboots has all its timings shifted and its
verdict is worthless. Repeat it.

### 18.10 Regressions checked

| Test | Result |
|---|---|
| `navtest.sh`, the usual half: the three turns on the language screen | **8 000, 8 428 and 8 000 px**, and the floor at **0 px**. The same figures as §17.8 (the extra 428 are the battery icon from §18.7). No regression |
| `navtest.sh`, the new half | the main menu appears, its two halves move together, SELECT enters Music and MENU returns to the same frame. See §18.6 |
| **`tools/ipod_video/navtest.sh` (the 5G)** | **76 548 px in all seven frames and the highlight moves: 12 312, 12 114 and 31 598.** The first two, spot on with those of §17.8; the third falls on the "~31 600" that the same table already recorded as not pixel-reproducible because that screen has a clock. **No regression** |
| retailOS **without a disk** | **0 px**, `CPU_exception_handler` and the usual software reset: the crash cycle of §4, same as before. The compositor does not composite because nobody gives it a source address, and now also because nobody turns on a layer |
| `tools/ipod_classic/lcdtest.S` | it does not program any layer, so the gate decides nothing for it: it keeps painting through the panel's data port |
| Migration | `vmstate_ipod_classic_wheel` moves to version 2 with the addition of `button_repeat_until`. Nothing migrates this machine today, so the old format is not kept alive |

### 18.11 What is still unknown

- **The low byte of `LAYERn_CFG`** (`+0x0d8`, `+0x0e0`, `+0x0e8`) is `0xff` in
  layers 0 and 1 and **`0x00`** in layer 2, across the 106 writes of one
  run. It looks like a **constant alpha**, and if it is, layer 2 is invisible
  even while its enable bit is set. 2.0.5 writes it at `0x08144470`, through
  the layer-order mapping below. **It is not modeled**: the bit
  already explains the whole symptom, and this would only affect the frames **in the
  middle of a transition**. **INFERRED**, unresolved. If someday a
  transition looks covered for an instant, this is it.
- **The 4-bit fields of `LAYERn_CFG` at `[31:28]` and `[15:12]`**, which
  `0x081278fc` (2.0.5: `0x081447a8..0x08144808`) writes in pairs `(1,0)`,
  `(0,1)`, `(5,4)`, `(9,8)`, `(11,10)`. Each pair has the shape `(1 - w, w)`,
  so they look like per-layer blend weights. **UNKNOWN**, and not modeled.
  Menus and Now Playing use `0x900080ff` / `0x500040ff` / `0x50004000` for
  layers 0 / 1 / 2; a running game uses `0x500040ff` for layer 0 and
  `0x900080ff` for layer 1.
- **Indices 3, 4 and 5 of the `CONTROL` writer** (bits `0x08`, `0x04`,
  `0x80`): only 5 is seen in use, twice in an entire run.
  **UNKNOWN**.
- **Layers 3 and 4**: nobody programs them and no bit is known for them.
- **The suffix byte of each trigger** in the rule table: it is ASCII and
  is `'1'` in all 1 380 rules of the image. What it is for is **UNKNOWN**.
- **The intermittent "Low Battery" screen** at 46 s still has no
  explanation of its own; see the suspicion in §18.9.

**`+0x0d4` is the layer order.** 2.0.5 stores `(order | b << 8)` there at
`0x0814482c` and keeps `order` in the display object at `+0x1e7`. Every
per-layer setter then maps logical layer `i` to physical layer
`table[order * 3 + i]`, where `table` is the 18 bytes at `0x083f35ec`: the six
permutations of `{0, 1, 2}`. Read back to front:

| `order` | back → front | Measured on |
|---|---|---|
| 0 | 1, 2, 0 | |
| 1 | 2, 1, 0 | menus, Now Playing |
| 2 | 0, 2, 1 | |
| 3 | 2, 0, 1 | |
| 4 | 0, 1, 2 | a running game |
| 5 | 1, 0, 2 | |

Order 1 sets the direction. Now Playing keeps the player interface in layer
0, with real alpha, over opaque album art in layer 1, so layer 0 must be in
front. In a game, order 4 puts the game frame in layer 1 in front of layer 0,
which is then an opaque `#353535` surface. With `DISP_ALPHA=1` the compositor
stacks layers 0..2 in this order. `b` (bits 15:8) is `2` on menus and `0` in a
game. **UNKNOWN**.

---

## 19. Fifth independent audit: the timer, measured once and for all (August 10, 2026)

The §18 batch left `tools/ipod_classic/tests/timer_audit.c` in the tree **without
writing down anywhere what the test answers**. This section fixes that:
the test was run, it says five things, and **three of them are model
bugs**.

```
$ sh tools/ipod_classic/tests/build.sh
$ ./build/qemu-system-arm -M iPod-Classic -kernel assets/tests/timer_audit.bin \
      -display none -monitor none -serial stdio
```

### 19.1 The starting hypothesis is REFUTED, with the measurement in hand

The work order came with a strong suspicion inherited from a previous
audit: "`TBCNT` does not advance in the model, it reads `0x1` twice", and hence
anything **delayed** —`delayedselected`, `TimerEventManager`— would be left
waiting forever. **It is not true.** All three free-running counters advance, and
**at the correct frequency**, which is the half that really matters:

| counter | programmed | measured in 200 ms | expected | error |
|---|---|---|---|---|
| `TECNT` | 1 MHz (`ECLK/12`) | 200 091 | 200 000 | +0.05 % |
| `TFCNT` | 12 MHz (`ECLK/1`) | 2 401 271 | 2 400 000 | +0.05 % |
| `TGCNT` | 10 kHz (`ECLK/16/75`) | 2 001 | 2 000 | +0.05 % |

And `tools/ipod_classic/tests/timer_irq.c`, the one that gave rise to the suspicion, today
prints `counter runs: yes 0x00000003 0x00000004`.

**And it is not just any counter: it is the one the retailOS uses.** Searching for the
block base literal in the image (`findlit.py ... 3C700000`) yields
three places, and two of them are timer E:

```
0x08324e48   TECON   = 0x440        (ECLK, /1)
             TEPRE   = 11           -> 12 MHz / 12 = 1 MHz
             TEDATA0 = 0xffffffff   (free-running)
             TECMD   = 3            (CLR | EN)
0x08324e38   ldr r0, [r0, #0xb4]    <- reads TECNT, and does nothing else
```

That is: the retailOS builds itself **exactly Rockbox's `USEC_TIMER`**
(`clocking-s5l8702.c:401`) and reads it with a one-instruction function. That
counter runs correctly. The timer hypothesis is **ruled out by direct measurement
on the counter the guest actually reads**, not by resemblance.

### 19.2 Real bug 1: an `ack` reset the count, and it cost 14 % of the clock

In the 16-bit timers the status bits live **in `CON`**, so
acknowledging forces a write to `CON` —`TBCON = TBCON`, which is literally what
Rockbox's `INT_TIMERB` does (`kernel-s5l8702.c:31`)—. And in the model
**every** write to `CON` called `chan_reload()`, which ends in
`ptimer_set_limit(..., reload = 1)`: **the count went back to the top**. On the real
machine only `TB_CLR` in `CMD` initializes the counter.

Consequence: every period came out **one interrupt latency longer**.

```
ack TBCON=TBCON: TBCNT 3289 -> 139   COUNTER WAS RESET
timer B interrupts in 2000042 us: 1713   expect 2000
timer F interrupts in 2000040 us: 1997   expect 2000 (ack via TSTAT, CON untouched)
```

The third line is **the control experiment**, and it is what turns this into
a proof instead of a suspicion: timer F has the same period, the
same VIC and the same handler, and differs **only** in that its status lives
in `TSTAT` and its `ack` does not touch `CON`. F kept time; B did not.

**FIXED** in `ipod_classic_timer.c`: the `CON` case only reloads if the
**configuration** bits —clock source, mode, interrupt enables—
actually change. A write that only clears status leaves the count alone.

```
ack TBCON=TBCON: TBCNT 3364 -> 3513   kept counting
timer B interrupts in 2000047 us: 1999   expect 2000
```

**No regression, and with the reason why:** the retailOS **does not use**
the 16-bit timers —its only uses of the block are timer E
above and a stray read of `+0x08` inside a function that sums bytes—,
so this fix is inert for it, and `navtest.sh` afterwards gives the same figures
to the pixel as before (§19.5). The beneficiaries are Rockbox and any bare-metal
test.

### 19.3 Real bug 2: `TICNTH`/`TICNTL` are not decoded

`s5l87xx.h:782` places a 64-bit free-running counter at `TIMER_BASE+0x80/0x84`, and
`s5l87xx.h:793` calls it `FIVE_USEC_TIMER`. `decode_offset()` covers A..D at
`0x00..0x7f` and E..H from `0xa0`: **`0x80` and `0x84` fall in the gap**, so
they are logged as "bad offset" and **read zero forever**.

```
TICNTH/L 0x00000000/0x00000000 -> 0x00000000/0x00000000  FROZEN
```

This really is, literally, the fault the work order described —a
time counter that does not run— but **it does not affect the retailOS**: there is not a single
`0x3c700080` or `0x3c700084` literal in the 9.9 MB of the image. It would affect a
guest that spun on `FIVE_USEC_TIMER`, which would hang
forever.

**NOT FIXED, on purpose.** Adding the register is easy; knowing its value at
reset, whether it can be stopped and at exactly what frequency it runs is not stated by
any source we have, and **an invented counter that runs at the
wrong rate is worse than one that is absent**, because the latter is noticeable and the former
is not.

### 19.4 Real bug 3: writing `DATA0` or `PRE` also resets the count

Same origin as §19.2 and same discrepancy with the real machine:

```
rewrite TBDATA0: TBCNT 3872 -> 403   COUNTER WAS RESET
rewrite TBPRE:   TBCNT 5411 -> 119   COUNTER WAS RESET
```

**NOT FIXED**, and the reason is noted in `chan_reload()`: neither Rockbox nor the
retailOS writes `DATA0` or `PRE` with the timer running —they write them
stopped, right before `CMD_CLR`—, so there is nothing measurable to gain, and
deferring the limit would break the opposite case (a driver that sets `DATA0` and
then `EN` without `CLR` and needs the new limit already live).

### 19.5 Four `navtest.sh` runs, and what they really say

Two before the timer fix and two after. **The figures do not move by
a pixel**, which is what was expected from §19.2.

| run | timer | verdict |
|---|---|---|
| 1 | before | **loses SELECT at `09-into-music`** (875 px). Everything else passes |
| 2 | before | **passes entirely.** `10-menu-back` at **898 px** from `08-menu-wheel-back` |
| 3 | after | **passes entirely.** `10-menu-back` at **0 px** from `08-menu-wheel-back` |
| 4 | after | **loses SELECT at `05-select`** (0 px), and with that drags down the whole second half |

The figures of run 3, which is the good one and the post-fix one: turns **8 000 /
8 000 / 8 000** px over a floor of **0 px**, `05-select` **19 340** px, `07` and `08`
**list 7 952 + panel 7 176** px, `09-into-music` **6 963** px, `10-menu-back`
**6 963** px and **0 px difference** from `08-menu-wheel-back`, `11-menu-at-top`
**0 px** in the list.

**Looked at as PNG, not just counted**, which is the other half of the measurement:

- `08-menu-wheel-back`: the main menu, "iPod" bar, **ミュージック**
  highlighted with its arrow, and the panel with the musical note.
- `09-into-music`: the **Music** screen —ミュージック bar, **Cover Flow**
  highlighted, and below it プレイリスト, アーティスト, アルバム, コンピレーション,
  曲, ジャンル, 作曲者, オーディオブック—. SELECT really went in.
- `10-menu-back`: **the main menu again**, "iPod" bar, ミュージック
  highlighted. The only thing separating it from `08` is the label that scrolls by itself
  in the panel. **MENU goes back, and goes back to the screen it left.**

And **that run's `stderr` is two lines**: the image load and the first
`SOURCE ADDRESS`. Not one exception, not one software reset. *Going back and dying
is not going back*, and here it did not die.

Other regressions:

| Test | Result |
|---|---|
| `tools/ipod_video/navtest.sh` (the 5G) | **76 548 px** in all seven and **12 312 / 12 114 / 31 629**. The first two exactly match §17.8; the third within the "~31 600" that screen moves by its clock. **No regression** |
| `timer_audit.bin` and `timer_irq.bin` | both pass; the figures are above |

**The §18.9 trap struck again, under a new name.** Two more runs
came out with the captures on the wrong screens and **3 and 1 software
resets**. It was not the patch: it was an `ipod-classic-run` process —a private
copy of the binary, under another name— at **93 % of a core** for twenty
minutes. `pkill -9 -x qemu-system-arm` does not see it, just as it does not see `qemu-evt`, and
for the same deliberate reason: so that one session does not kill another.

What did work: **the reset warning that §18 put into `navtest.sh`
fired both times** and kept those figures from being read as a timer
regression. The missing warning is now added —**count QEMU-like processes
before starting and say so**—, because the existing one only found out
*afterwards*, when the run had already been wasted. It does not kill them: they may be
another session's work in progress.

### 19.6 The SELECT loss is wider than §18.8 said

§18.8 and the `navtest.sh` header warned that the run can
fail **at `09-into-music`**. Measured here: **the first
SELECT, the one on the language screen, is also lost** (run 4, `05-select` = 0 px, with no
reset or exception in `stderr`). And when that one is lost, **it drags down the whole
second half of the test**, because the following captures land on the language
screen: `07`/`08` still give ~8 000 px in the list and **0 in the panel**, which
is exactly right for that screen (§18.5) and looks like a panel bug
if you do not check where you are.

Tally for this audit, counting only presses that have a target:

| | hits |
|---|---|
| SELECT (`05` and `09` together) | **5 of 7** |
| MENU with somewhere to go back to (`10`) | **2 of 2** |
| MENU at the top of the stack (`11`), where the correct thing is to do nothing | **4 of 4** |

The §18.8 conclusion holds and is reinforced: **repeating the button
state did not fix the loss, and the loss does not depend on the screen**. The `navtest.sh`
header is corrected to say it can fail at `05-select`
as well as at `09-into-music`.

### 19.7 What is confirmed from the §18 batch, checking it separately

- **The layer gate is real and the bits are what it says.** Disassembled
  again from scratch, with the load base from §1 (`0x08000000 + (offset -
  0xAE48)`; with the naive base `0x08000000` you land in a C++ *thunk* and
  it looks as though the evidence were made up): `0x081271c8` loads `0x38900000` from the
  literal pool at `0x081272ac`, reads `[r3, #8]`, branches via
  `cmp r1, #5 / addls pc, pc, r1, lsl #2`, and the six branches do `bic`/`orrne`
  with `0x40, 0x20, 0x10, 0x08, 0x04, 0x80` and store at `0x08127250`.
  **VERIFIED instruction by instruction.**
- **The write order too.** In a run of our own: `+0x060 =
  0x089d42d4` and then `+0x008: 0x41118101 -> 0x41118141` (bit 6); `+0x078 =
  0x0be2326c` and then `-> 0x41118161` (bit 5). **VERIFIED.**
- **The layer 2 descriptor is the one that stayed on top.** `+0x094 =
  0x014000f0` (320×240), `+0x09c = 0` (at the origin), `+0x090 = 0x0bfb4ff8`,
  written **only once in the whole run** and never cleared. **VERIFIED.**
- **The empty MENU rule belongs to that screen, and now for a better reason than
  the label.** `uirules.py` warns that the name on the left is "the closest one
  going backwards", and rightly so: under the same label there also appear
  `button.menu.up1 -> HandleExit` (`0x0073e774`) and a **stopwatch** rule
  (`button.playpause.up1 -> Stopwatch_Screen_Running`), which cannot belong to a
  language screen. Reading the bytes, the groups are `u32 count;
  RULE[count]`, and the `count = 3` that starts at `0x0073e624` contains **the empty
  MENU rule, `controller.GotoScreen_Language1` and
  `list.pid.229444595.chosen1`**: all three from the language screen.
  `HandleExit` falls into the **next** group, with `count = 5`, which is the
  stopwatch one. The §18.4 conclusion holds, but **the good proof is
  group membership, not the label**.
- The `uirules.py` counts check out: **1 380** rules, **108**
  `button.menu.up` of which **46** have neither handler nor actions, and
  `list.pid.229444595` appears **only once** and only with `.chosen1`.

### 19.8 A minor annotation correction

`S5L8702_WHEEL_BTN_REPEAT_MS` was presented as backed by "the real device
transmits", when two constants further up in the same file say that **the
real wheel's rate is UNDETERMINED**. The only checkable fact is that the state
of the five buttons travels in every status word
(`button-clickwheel.c:273`); that the wheel keeps reporting **after** release
is not stated by any source. The 400 ms are marked as a **model choice**, like its neighbors.

### 19.9 What remains open

- **The SELECT loss**, §19.6. Still without a cause, and the PMU
  hold-switch experiment from §18.8 **still has not been done**.
- **`TICNTH`/`TICNTL`**, §19.3.
- **The reload on `DATA0`/`PRE`**, §19.4.
- **`CON[MODE]` modes other than 0**, which are logged as not
  modeled and treated as interval.
- Everything in §18.11, which remains the same.
