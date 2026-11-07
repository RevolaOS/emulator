# The iPod classic 6G (S5L8702) audio path in QEMU

What it takes for the retailOS to **produce sound**, what is modeled now, and where
the remaining wall is. Everything below is **measured** except where it says
`INFERRED` (reasoned from something verified) or `UNDETERMINED` (we do not
know and do not pretend to). The traces are reproducible with the commands at the end.

> Context: the music list already works (Music -> Songs shows the tracks
> with title and artist; see [13-classic6g-music.md](13-classic6g-music.md)).
> What was missing is that selecting a song **plays sound**. This document is
> that front.

---

## 0. Status by milestone

| Milestone | Status |
|---|---|
| — Model the I²S block (previously `create_unimplemented`) | **done** — `ipod_classic_i2s.c`, with readback, FIFO and WAV capture |
| — CS42L55 codec as an I²C slave at `0x4A` | **done** — `ipod_classic_cs42l55.c` |
| 1. Now Playing is drawn in full (bar + info) | **NOT reached** — blocked earlier: buttons do not register in the song list (§5) |
| 2. The playback position advances over time | **mechanism done and demonstrated** on the boot push; the milestone (bar) needs real playback (§6) |
| 3. Dump the decoded PCM to a playable WAV | **infrastructure done; content = silence**, blocked by the DSP (§4) |

Honest summary of where I stop, with measured evidence:

- **WAV capture works** (12288 bytes = 6144 samples per boot,
  44100 Hz 16-bit stereo, valid WAV), but what it captures is **silence**
  (RMS 0, no "NON-ZERO" warning). Reason: **the retailOS decodes the MP3 on the
  CalmADM2E DSP, not on the ARM**, and the DSP is not modeled (§4).
- **I do not reach Now Playing** because there is a wall *before* audio: in the **song
  list**, the wheel **turns** (moves the highlight) but the **buttons**
  (SELECT and MENU) **do not register** — 100+ attempts, with and without a prior wheel
  touch, on two disks, with various press durations. MENU is pure
  navigation (no audio at all), so the wall is in **input/UI**, not in the audio
  path, and **it is not caused by my changes** (§5).
- The **I2STXCOM readback** that the reports proposed as the key to Now
  Playing turned out to be **harmless but not the key**: the firmware does a
  read-modify-write (`ldr; orr #6; str`, VA `0x2200876c`) that **writes 6 regardless of
  what** the read returns (§5).

The path to "a WAV with the song" is still **Rockbox** (it decodes on the
ARM), but only the bootloader is built and booting it is a big detour (§7).

---

## 1. The I²S block map — base `0x3CA00000`

Base confirmed by two sources that agree: `I2S_MEM_BASE`
(`include/hw/arm/ipod_classic/ipod_classic.h:102`) == Rockbox `I2S_BASE`
(`assets/rockbox/firmware/export/s5l87xx.h:1182`). Until now it was
`create_unimplemented_device`; now it is a real device
(`hw/arm/ipod_classic/ipod_classic_i2s.c`).

| Off | Register | Value the retailOS writes | Tag |
|---|---|---|---|
| +0x00 | `I2SCLKCON` | `0x00000001` (clock on) | **VERIFIED** (live trace, pc~`0x22008800`) |
| +0x04 | `I2STXCON` | `0x0b100019` | **VERIFIED** (== Rockbox `pcm-s5l8702.c:204`) |
| +0x08 | `I2STXCOM` | `0x6` = start (Rockbox uses `0xe`/`0xa`) | **VERIFIED**; and it is **read back** |
| +0x10 | `I2STXDB0` (FIFO) | DMA destination: 16-bit PCM samples | **VERIFIED** |
| +0x30 | `I2SRXCON` | `0x00001000` | **VERIFIED** |
| +0x34 | `I2SRXCOM` | — | INFERRED (not seen in playback) |
| +0x38 | `I2SRXDB` | — | INFERRED |
| +0x3C | `I2SSTATUS` | — (neither read nor written in my runs) | **UNDETERMINED** |
| +0x40 | `I2SCLKDIV` | `0x110`=272 | **VERIFIED** (pc~`0x22008714`) |

Frequency and format — **VERIFIED**: `I2SCLKDIV = MCLK/samplerate`,
`MCLK = 12 MHz` ⇒ 272 → 44100 Hz. The retailOS `set_freq` routine is at VA
`0x0808e028`: table 8000/11025/12000/16000/22050/24000/32000/44100 with the same
32 kHz trick as Rockbox (at 48 kHz with MCLK/3), and it ends in
`cscodec_write(5, code)` (`0x0808e138`), which sets the codec's `CLKCTL2`. Sample =
**16-bit** (DMA `WIDTH_16`), **stereo** interleaved L/R.

### What the model does (milestone 1)

A register file with **honest readback**. The key: the retailOS writes
`I2STXCOM=0x6` and **reads it back** to confirm the transmitter started;
while the block was `create_unimplemented` that read returned 0. Measured with
the new model: a read of `+0x08` returns `0x00000006` (before, 0). The
`I2SSTATUS` (+0x3C) returns 0 for now — its bits are UNDETERMINED and no
driver we have reads it; if drawing Now Playing turns out to be gated on it,
it will show up in the `I2STRACE` trace during real playback (§5).

`I2STRACE=1` traces every access (except the FIFO, which are thousands) with its value and the
guest PC, just like `DISPTRACE`/`ATATRACE`/`WHEELTRACE`.

---

## 2. The transport: DMAC0 (PL080) to the I²S FIFO

Controller: **DMAC0** at `0x38200000` (`hw/dma/pl080.c`, IRQ 16). The
playback channel (Rockbox `dma_play_ch_cfg`, `pcm-s5l8702.c:61-75`):

- `srcperi = MEM` (DRAM, `sinc = INC`), `dstperi = IIS0_TX = 0xA`
  (`dma-s5l8702.h:50`), fixed destination `0x3CA00010` (FIFO, `dinc = INC_DISABLE`).
- `swidth=dwidth=WIDTH_16`, chained LLIs, terminal-count IRQ per chunk.

Corroborated in the model: the peripheral FIFO table of the retailOS DMA
infrastructure lives in IRAM and contains `0x3CA00010` (TX) and `0x3CA00038`
(RX), at `0x22008a94` (VERIFIED by disassembly). The only destination DMAC0
programs for audio is `0x3CA00010`.

---

## 3. The CS42L55 codec (I²C0 `0x4A`)

**VERIFIED**: it is a Cirrus Logic **CS42L55**, I²C0 bus, 7-bit slave `0x4A`
(8-bit `0x94`). Two sources agree: Rockbox `cscodec-6g.c:39`
(`i2c_write(0, 0x94, reg, 1, &data)`) and the retailOS, whose `cscodec_write` is at
VA `0x0808e1ac` (loads `r0=0x4a`, length 2, and calls the same I²C routine the
PMU uses, `0x08325a20`). The retailOS codec init (VA `0x08086000`, 19
`cscodec_write`s) reproduces `audiohw_preinit()` almost byte for byte, with
`CLKCTL2=0x0b` = 44100 Hz by default.

The model (`ipod_classic_cs42l55.c`) is an I²C slave with a register pointer and
auto-increment —like the PMU—, 128 registers, honest readback (which is
what the drivers' read-modify-write needs), and the chip id at `0x01`.
Before, it did not exist: writes to `0x4A` got NACK and reads returned
garbage.

---

## 4. The milestone 3 wall: the DSP decodes, and it is not modeled

**The ARM does NOT decode the MP3 in the retailOS.** The **CalmADM2E** DSP does
(`ADM_SFR_BASE 0x39000000` / `ADM_BASE 0x3F000000`), currently `create_unimplemented`.
Evidence:

- **VERIFIED** (experiment): with `AUDIOWAV=/path.wav` I captured everything the
  DMA dumps into the FIFO during a boot. Result: **6144 samples, all 0** (the
  chain starts and clocks out silence). The WAV comes out valid (44100 Hz, 16-bit
  stereo) but mute.
- **INFERRED-strong**: the firmware references the DSP space hundreds of times
  (0x39000000 / 0x3F000000); Rockbox, which decodes on the ARM, does not touch the DSP.
  The PCM buffer is filled by the DSP, and without a DSP the buffer stays at zero.

Implication: **capturing recognizable audio from the retailOS requires modeling the
CalmADM2E** (a programmable DSP with Apple's decode firmware loaded) —
a big job, outside the scope of this phase. The realistic alternative is
Rockbox (§7).

---

## 5. Milestone 1 — the real wall: buttons do not register in the song list

**Now Playing is not reached**, and the obstacle is *before* audio. Measured with
`tools/ipod_classic/iplay.sh` (navigation + captures) on `Emulti.img` (4
Linkin Park tracks) and `E4kzero_rebuilt.img` (1 track):

- **Menu** navigation works as before: language → main menu
  → **Music** → submenu → **Skladby/Songs** → **song list**. SELECT
  enters each submenu (measured: `diff` of 6000–38000 px per transition) and the
  list comes up with its 4 tracks, title and artist. **Zero regression.**
- In the **song list**, the **wheel turns**: an `e` moves the highlight from
  "In The End" to "Numb - Encore" (nonblack 75372→75462→75515, VERIFIED in the
  `P05/P06` captures).
- But the **buttons do nothing** there: **SELECT** on a song does not start
  playback or draw Now Playing, and **MENU** does not go back either.
  **Measured**: 100+ SELECT presses (holds of 60/120/250/400 ms, with and without
  a prior `-w` wheel touch, on both disks) → **0 screen changes**;
  and **0 I²S activity** (the `I2STXCOM=6` counter stays at 2, the two
  from boot; the WAV stays at 6144 samples). The decisive discriminator:
  **MENU** from the list (capture `P07`) does **not** navigate either, and MENU is
  pure navigation, **with no audio at all** — so the wall is in **input/UI**,
  not in the audio path.

What it is NOT (ruled out by measurement):
- **It is not my I²S or my codec**: playback aborts *before* touching the I²S
  (0 new accesses), and MENU does not touch audio at all. Reintroducing the block
  does not change this.
- **It is not the disk**: identical with 4 tracks and with 1 track.
- **It is not that the wheel is dead**: it turns and moves the highlight on the same
  screen where the button is ignored.
- **It is not the battery reset**: **0 resets** in all runs.

What it is: **UNDETERMINED** between (a) the list widget consumes the wheel's button
packets in a way the model/harness does not reproduce, and (b) a playback path
that aborts higher up. The previous investigation (i2s-dma) did observe
playback start **once** (2 new `I2STXCOM=6`) by firing SELECT
by hand; **I could not reproduce it** in this phase. It is the next concrete
obstacle: **make SELECT on a song register in the list widget**
(compare how the firmware consumes button packets in a menu vs. in the
list — it is the same wheel, a different consumer).

About the readback that was going to be the key to Now Playing: the firmware **does read**
`I2STXCOM` (VA `0x2200876c`), but in a read-modify-write —
`ldr r1,[TXCOM]; orr r1,#6; str r1,[TXCOM]`— that **writes `6` regardless of
the read**. So returning the written value (my change) is correct and
harmless, but it is **not** what gates Now Playing. `I2SSTATUS` (+0x3C) **is not
read** in any of my runs.

---

## 6. Milestone 2 — DMA pacing: mechanism done and demonstrated

**The mechanism is implemented and demonstrated**, gated by `AUDIOPACE=1` (by
default **off**, and then the build is byte-for-byte identical to the previous one).

What it does (`hw/dma/pl080.c` + `ipod_classic_i2s.c`):
- A `paced_lines` field (bitmap of request lines) is added to the PL080. A
  line that is **not** marked transfers at full speed **exactly as before**
  — the change is inert unless a peripheral opts in. The `IIS0_TX` line
  (`0xA`) is marked only under `AUDIOPACE`.
- For a marked line, a `mem->peripheral` transfer (flow 1) only advances while the
  peripheral has requested data (`pl080_pace_request`), and the request is **consumed on
  each terminal-count**: **one chunk per request pulse**.
- The I²S starts a `QEMUTimer` when it sees `I2STXCOM` with the run bit, and on each
  tick requests a chunk and **reschedules the next tick according to the samples that
  moved** (self-correcting cadence).

**Measured** (boot with `AUDIOPACE=1 I2STRACE=1`): a drain of **2048
samples** schedules the next one at **23211 µs** — i.e. 1024 frames / 44117 Hz
= 23.2 ms, **the real 44100 Hz cadence**. Without pacing those samples go in all at
once. **Safe**: with `AUDIOPACE=1` the music **is still listed** (I reach the
list, `diff=38030`), **0 resets**, **0 hw_error** (Apple programs the channel with
flow<4, contrary to what was feared; verified by a clean boot).

Honest limit: I can only demonstrate it on the **boot's silence push**
(6144 samples), because I do not reach sustained playback (§5). In
that push, **some chunks slip through without pacing** (only 1 of ~3 drains via the
gated path) because not all of them carry the terminal-count bit that consumes the
request; with real playback (Rockbox uses the I-bit per chunk) pacing
would engage continuously. **The real milestone 2 —the advancing bar—
requires reaching playback first.**

---

## 7. The Rockbox route for milestone 3

Since the I²S+PL080 transport is identical to Rockbox's, booting Rockbox (which
decodes MP3 on the ARM) on this same model would produce **non-zero** PCM through
the same FIFO, capturable to WAV with the same `AUDIOWAV` — that would be milestone 3.

Status: `assets/rockbox/` only has the built **bootloader**
(`build-boot/bootloader-ipod6g.ipod`, IM3 format), **not** a full build of
the app (`rockbox.ipod`). Booting it requires building the entire app for ipod6g,
dealing with the IM3 format (loaded by the bootrom, which we do not have), and having Rockbox
handle storage/LCD through ITS OWN paths (which touch unmodeled registers). It is a
big detour, outside the scope of this phase, but it is **the recommended path**
for real audio. The fork `assets/ref/freemyipod-qemu` (branch s5l8702-dev)
boots Rockbox on this SoC and can serve as a reference.

---

## 8. How to reproduce it

```bash
# build
wsl -d Ubuntu-24.04 -e bash -c "cd /mnt/c/Desarrollo/qemu-ios/build && ninja qemu-system-arm"

# boot with a disk, capturing the FIFO PCM to a WAV
cd build
AUDIOWAV=/tmp/audio.wav I2STRACE=1 ./qemu-system-arm -M iPod-Classic \
    -kernel ../assets/classic6g/osos_decrypted.bin \
    -drive if=ide,format=raw,file=~/6g/Emulti.img,file.locking=off \
    -snapshot -display none -serial null -monitor none

# the same with DMA pacing enabled (milestone 2): samples go in at 44.1kHz
AUDIOPACE=1 I2STRACE=1 ./qemu-system-arm ...   # -> "I2STRACE drain: chunk N ..."

# navigate to a song and try to play it, capturing the screens
sh tools/ipod_classic/iplay.sh ~/6g/Emulti.img ~/6g/play.wav

# measure the WAV (RMS 0 = silence; the DSP is not modeled, see §4)
python3 - <<'PY'
import wave,array,math
w=wave.open('/tmp/audio.wav'); d=w.readframes(w.getnframes())
a=array.array('h'); a.frombytes(d)
print('frames',w.getnframes(),'rms',math.sqrt(sum(x*x for x in a)/len(a)))
PY
```

Trace/experiment variables: `I2STRACE=1` (I²S accesses + pacing
cadence), `AUDIOWAV=<path>` (dumps the FIFO PCM to WAV), `AUDIOPACE=1`
(gates the audio DMA to the sample rate).
