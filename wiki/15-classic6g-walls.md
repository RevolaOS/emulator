# The walls of the iPod classic 6G, and how each one fell

A synthesis chronicle of the port of the **iPod classic 6G** (Samsung S5L8702
SoC) to QEMU: from an unreadable encrypted firmware to an iPod that boots
Apple's original retailOS, draws its interface, can be navigated, and **lists
music**. Each entry is a real obstacle: the **symptom** it showed up as, the
underlying **cause**, and **how** it was unblocked. Addresses are firmware VAs
(base = offset in `osos_decrypted.bin` + `0x07FF51B8`).

The technical detail of each one is in the documents cited; this is the map.

---

## 1. The firmware was encrypted — it couldn't even be read

**Symptom.** The `osos` from the `.ipsw` measured an entropy of 7.99 bits/byte,
zero ASCII strings. Unreadable. ([01](01-firmware-ipsw.md))

**Cause.** AES encryption with the GID key, which lives inside the S5L8702's
silicon and can't be extracted.

**How it fell.** There was no need to *have* the key: it was enough for **the
device itself to decrypt it**. With the user's physical iPod running emCORE,
its AES engine decrypted the payload over USB in 1.2 s. A fine detail that
cost effort: `hwkeyaes()` resets the IV to zero at the start of each call, so
chunking broke the first block of each chunk; it is repaired with an XOR
against the previous last encrypted block. Result: entropy 5.76, 42,014
strings, `Copyright 2001-2008 Apple Inc.`. ([10](10-classic6g-emcore.md))

---

## 2. It booted, but without a disk it killed itself

**Symptom.** The retailOS reached its RTXC idle loop and went into a crash
loop: 19 Data Aborts + 19 software resets in 20 s, invisible because they went
out on **stderr** via semihosting (`CPU_exception_handler`).

**Cause.** A C++ virtual call on a **null** object. The null came from a
`"FONT"`/`"Str "` resource lookup that returned NULL, and the firmware didn't
check it. Without fonts, there's no interface, and the system dies.

**How it fell.** Attaching a disk. The fonts live on the disk, not in the
firmware. ([11](11-classic6g-retailos.md) §4)

---

## 3. Black screen despite reaching the main loop

**Symptom.** The system ran cleanly (thousands of IRQs, zero exceptions) but
drew nothing.

**Cause (first layer).** An RNG model at `0x3C100000` gated reads behind an
"enable" bit that was never written with a non-zero value → hang on the first
draw, a loop of 48 million iterations.

**How it fell.** Removing the false gate. The inherited comment said that
`0x08031db0` wrote 1 to the enable; the literal there was `0x08913CFC`, a flag
in bss. It was verified by disassembling, not by reading the comment.
([11](11-classic6g-retailos.md) §19.2)

---

## 4. The big image wall: the video compositor

**Symptom.** The firmware issued LCD commands but not a single pixel reached the
panel. An agent went so far as to rule out `0x38900000` because *"no register
receives anything that looks like a DRAM address"*.

**Cause.** `0x38900000` **is the video compositor**, not a simple LCD. The image
is composed in layers in DRAM and an engine pushes it to the panel — that's why
no pixel write went over the bus. The ruling-out was false: 793 distinct
addresses were written, among them an **identity gamma table** (768 words =
3×256) and the **panel geometry down to the pixel** (`+0x214 = 0x013F00EF` =
319×239).

**How it fell.** Modeling the compositor (`ipod_classic_disp.c`): five layer
descriptors, and dumping the DRAM framebuffer with `pmemsave` revealed **the
entire language screen already composed**. There was no need to invent pixels;
they had to be copied to the panel. ([11](11-classic6g-retailos.md) §13-14)

> **Lesson.** Counting accesses to a block without looking at *what values* it
> carries is how the most important piece of the project got ruled out by
> mistake.

---

## 5. The DMA `memcpy` that never returned

**Symptom.** With a disk, the interface waited forever. The diagnosis pointed
at the ATA.

**Cause.** The ATA was **innocent**: its cycle completed and its semaphore woke
up. What hung was a **later** `memcpy` via DMAC0 (a PL080) that slept with no
timeout waiting for the terminal-count IRQ.

**How it fell.** Wiring up QEMU's two PL080s (`hw/dma/pl080.c`) with system
memory as the master port. ([11](11-classic6g-retailos.md) §14)

---

## 6. The fonts "that weren't there"

**Symptom.** The loader went to `Resources/Fonts` and found nothing. Our own
wiki described the `.ipsw`'s `rsrc` image as "filled with `0xFF`, generated on
the device".

**Cause.** False. The `rsrc` **is a real FAT16 volume** named `IPODRESOURC`,
with Helvetica and the CJK fonts inside. It read as empty because a glance
without the container's **`0x1000` bias** lands in the padding; the volume
starts at `0x7e00`.

**How it fell.** Applying the bias and mounting that volume. That's where the
kanji and the Greek on the language screen come from. ([01](01-firmware-ipsw.md))

---

## 7. The compositor was covering the interface

**Symptom.** The keys worked but the screen didn't change: *what they did
couldn't be seen*.

**Cause.** `disp_draw_layer()` drew every layer that had a source address. The
firmware, when finishing a transition, turns the layer off but **leaves the
descriptor in place**. Layer 2 stayed on top of layer 0 forever, covering the
menu, which had been drawn underneath the whole time.

**How it fell.** Honoring the `CONTROL` bits (`+0x008`): bit 6 → layer 0, bit 5
→ layer 1, bit 4 → layer 2. It was uncovered by dumping layer 0's buffer and
**looking at it** — the whole menu was there. ([11](11-classic6g-retailos.md)
§18)

---

## 8. The keys: the click wheel and the PMU gate

**Symptom.** First, no input at all. Then, turning worked and **the buttons
didn't**.

**Cause (two layers).** (a) The click wheel at `0x3C200000` wasn't modeled; and
`WHEELMASK` didn't return what was written, so the firmware masked itself out.
(b) With the wheel in place, the buttons fell into a **silent gate**: 21
instructions into the key dispatcher, the firmware reads PMU register `0x87`
(hold switch) and returns if it says "locked". Our PMU returned zero for it.

**How it fell.** Modeling the wheel (IRQ 23, honest reads of `WHEELMASK`,
packets spaced by a timer) and reporting the hold switch as *off*.
([11](11-classic6g-retailos.md) §17)

> **Lesson, refined twice.** The model raising a line doesn't mean the firmware
> receives it. You have to check the whole path — and *for each type of event*:
> turning and buttons followed different paths.

---

## 9. "MENU doesn't go back" — the wall that didn't exist

**Symptom.** On the language screen, MENU did nothing (0 px).

**Cause.** None. On that screen **Apple left the rule empty**: it is the first
in the welcome sequence and there is no stack to go back to. Of 108
`button.menu.up` rules, 62 do something and 46 don't. From a deeper screen,
MENU works — the user confirmed it by navigating.

**How it fell.** By recognizing that the model was doing the right thing. The
"0 px" measurement was valid; the conclusion "there's a bug" was the mistake.
([11](11-classic6g-retailos.md) §18.5)

---

## 10. Music (a): the FAT32 volume was rejected at mount time

**Symptom.** "No Music" even though the disk carried the library. The firmware
read the volume header **only once** and never touched it again — no
directory, no `iPod_Control`.

**Cause.** The volume classifier sees the `0xAA55` that **every FAT boot sector
carries** at `+0x1FE` and treats it as a nested MBR. Finding no partition
entries, it falls back to a last resort: `memcmp(boot+0x34, "FAT", 3)`.
`mkfs.vfat` leaves those bytes at zero (the real `"FAT32   "` is at `+0x52`,
which that path doesn't look at) → rejection.

**How it fell.** Writing `"FAT"` at `boot+0x34`, a reserved field that no real
FAT driver reads (verified against Rockbox). Isolated A/B: with the marker, 85
reads inside the partition; without it, 3. Before that, sector size
(512/2048/4096), partition type (0x0B/0x0C) and cluster count were ruled out by
measurement. ([13](13-classic6g-music.md) §5-6)

---

## 11. Music (b): the `iTunesDB` signature was twenty zeros

**Symptom.** The volume mounted, the firmware parsed the `iTunesDB`, but the
library came out empty: validation of the **signature** (`hash58`,
`mhbd+0x58`) failed. Forcing the correct FireWire GUID wasn't enough.

**Cause.** The signature this firmware expects is **20 zero bytes**. The
algorithm is libgpod's `itdb_hash58` byte for byte, but the S5L8702's SHA-1 is
a **hardware accelerator at `0x38000000` that QEMU doesn't model**: its reads
return 0, the digest comes out 0, the HMAC key comes out 0, and the expected
hash collapses to twenty zeros (measured at the `memcmp` `0x08328598`).

**How it fell.** Zeroing the `hash58` of each copy of the `iTunesDB`
(`mkdisk6g4k.py`, annotated as a stub until the accelerator is modeled).
Decisive A/B: real libgpod signature → empty library; zeros → "1 曲". The
firmware **does validate**, and it demands zeros. ([13](13-classic6g-music.md)
§7-8)

> **Irony.** We started out fearing the signature, dismissed it as premature
> when it turned out to be the mount, and in the end the signature was the wall
> again — with an answer nobody would have guessed without measuring it.

---

## 12. retailOS audio: silence, and the DSP theory that turned out false

**Symptom.** With the music now listed, on pressing play the player came up
halfway (icon yes, bar and info no) and made no sound.

**What works.** The SoC's audio path was modeled —the I²S at `0x3CA00000`, the
CS42L55 codec over I²C `0x4A`, the DMA pacing— along with PCM capture to WAV.
But the retailOS WAV comes out with **RMS 0: silence**. Something isn't
producing the PCM.

**The cause we believed —and it was false.** We said: *"the retailOS decodes on
the CalmADM2E DSP (`0x39000000`/`0x3F000000`), which isn't modeled"*, and that
making sound would require **emulating an entire DSP**. We deduced it from
**245 and 213 "references"** to those addresses in the firmware. Later RE
**measured it and knocked it down**
([19-classic6g-dsp-hle.md](19-classic6g-dsp-hle.md)):

- The retailOS **does not touch** `0x39000000`/`0x3F000000` via MMIO in any
  reachable state. **The "references" were the `float 0.5f`** — `0x3F000000`
  is 0.5 in IEEE-754 — and data (ARGB colors). We counted the decimal number
  0.5 and called it a DSP.
- The hardware at `0x391/2/3` that does get driven during playback is an
  **image scaler** (the album art), not audio.
- **Strong evidence that the retailOS decodes audio in software** (ARM):
  strings `MP3ExampleTask`, `MeCCADecode`, `alac: bit depth`, an `AudioCodecs`
  table.

**Status.** If decoding is in software, **there's no DSP to emulate** and audio
should flow like Rockbox's; the silence has another cause, upstream. The
experiment that decides the architecture —a watchpoint on the audio DMA
source, to see who writes the PCM— is under way. The already-proven route that
**does make sound** is Rockbox (13-14).

> **This wall is the best example of the pattern below, and we committed it
> ourselves.** We wrote this very document warning that *counting references
> instead of looking at the value* is the trap that cost the most — and right
> after that we built a DSP theory by counting 0.5. The method that tripped us
> up was the one that got us out.

---

## 13. Rockbox: building and booting the free firmware on the machine

**Why.** Rockbox decodes MP3 **on the ARM**, not on a DSP. Its PCM would come
out for real through the same I²S we already capture. And it's open source,
with symbols, so debugging it is incomparably easier than the retailOS done
blind.

**Wall (a): building.** The Rockbox tree includes the `ipod6g` target, but
Rockbox officially wants its own `arm-elf-eabi-gcc 4.9`. It was built with the
system's `arm-none-eabi-gcc 13.2` (it correctly detected `arm v5, classic`),
yielding `rockbox.bin` (1.13 MB), `rockbox.elf` (with symbols) and
`rockbox.zip` with the codecs — including `mpa.codec`, the ARM MP3 decoder.

**Wall (b): booting.** Rockbox loads at `0x08000000` (LMA of the vector table),
same as the osos, so it boots via `-kernel`. But it hung, because of two
things the real bootloader does and we were skipping:

1. **The stack.** `crt0.S` (app build, not bootloader) calls `memory_init`
   *before* setting up its stacks. With SP=0 it gave a data abort. SP is seeded
   to `0x22020000` (top of IRAM0) in a hand-off that runs after the devices are
   reset.
2. **The microsecond clock.** `udelay` spins on `USEC_TIMER == TECNT` with no
   timeout, and its init is `#ifdef BOOTLOADER`. TimerE is started at 1 MHz
   just like `usec_timer_init()`.

And a third, for video: **Rockbox pushes the frame via 16-bit DMA** to the LCD
(`displaylcd_dma`, DMAC0), while the retailOS uses 32. The panel's access
floor, set to 4 bytes, rejected every Rockbox write → 0 pixels. It was lowered
to 2, without breaking the retailOS's 32-bit path.

**How it fell.** With those three adjustments (all in the model, annotated and
without touching Rockbox), **Rockbox boots to its cabbiev2 main menu,
navigable**: the wheel moves the highlight, SELECT opens. Verified with
inspected screenshots and no retailOS regression.
([16-classic6g-rockbox.md](16-classic6g-rockbox.md))

---

## The pattern that repeated: counting instead of looking at the value

Time and again in this project, something was taken for granted **by counting
or assuming** instead of looking at the concrete value — and it was wrong:

| Taken to be… | Was actually… |
|---|---|
| `0x38900000` "no register with a DRAM address" → ruled out | the video compositor, the central piece |
| the `rsrc` image "empty, generated on the device" | a FAT16 with the typefaces |
| the ATA "to blame for the hang" | innocent; it was a later DMA `memcpy` |
| **245+213 "DSP references" → the retailOS decodes on a DSP** | **the `float 0.5f` and data; there is no such DSP** (wall 12) |

The first three cost us hours and we recovered from them. The fourth we wrote
**into this very document as a theory**, and another RE knocked it down by
measuring. It is the purest case: we fell into the trap that our own doctrine
warns against.

The doctrine, now with no excuse: disassemble the binary instead of reasoning
about the hardware; look at **what** data a register carries, not just **how
many** accesses there are (`0x3F000000` may be a peripheral… or the number
0.5); and treat every measurement as suspect until the instrument has been
validated with a control case.

---

## Walls still standing

- **Rockbox makes sound — resolved.** Rockbox boots to its menu, plays an MP3
  (decoding on the ARM) and the PCM comes out **through the host's speakers**
  and into a WAV that correlates at 1.000 with the original.
  ([16](16-classic6g-rockbox.md) §7-8)
- **retailOS audio — reframed, in progress.** It is **no longer** believed that
  emulating a DSP is needed (wall 12): the evidence says the retailOS decodes in
  **software**. The decisive step —finding out with a watchpoint who produces
  the PCM and why it's silence— is under way. If software decoding is
  confirmed, making sound would mean unblocking the path, not emulating a
  processor. ([19](19-classic6g-dsp-hle.md))
- **"Connect to Power" / reboot at ~46 s.** The power model makes the retailOS
  believe the battery is empty (it reads the charge via a path we don't model;
  the ADC we provide is the one Rockbox wants, not Apple). Narrowed down.
- **The S5L8702 bootrom.** `0x20000000` returns four repeated constants: it is
  locked after boot. No dump is possible via emCORE; it would take DFU + an
  exploit. ([10](10-classic6g-emcore.md))
