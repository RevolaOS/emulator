# HLE of the retailOS audio DSP: specification and inventory of unknowns

Handoff document for **two reverse-engineering investigations** (one of
static disassembly, the other of runtime tracing) that pursued the same
goal: **to specify exactly what our DSP model has to do
for Apple's retailOS to play audio in the emulator**, so that the
next workflow can implement the HLE without investigating again.

It is the direct continuation of [17-classic6g-dsp-retailos.md](17-classic6g-dsp-retailos.md)
(the starting plan) and of [14-classic6g-audio.md](14-classic6g-audio.md) (the
downstream audio path, already modeled). It **corrects** the premise of [17](17-classic6g-dsp-retailos.md)
with measurement.

Firmware VA addresses = offset in `osos_decrypted.bin` + `0x07FF51B8`.
Markers used throughout the document:

- **VERIFIED** — measured at runtime or read from the disassembly.
- **INFERRED** — reasoned from something verified; the strength is indicated.
- **UNKNOWN** — not known, and not pretended. A way to find out is proposed.

---

## 0. Verdict that changes the frame (read this first)

**The premise of [17](17-classic6g-dsp-retailos.md) — "the retailOS references the
CalmADM2E DSP A GREAT DEAL (245 and 213 literals) and decodes the MP3 there" — does NOT
hold.** The two investigations, by independent routes (disassembly and
tracing), arrived at the same thing:

1. **The retailOS does not touch `0x39000000` (ADM_SFR) or `0x3F000000` (ADM) in any
   reachable state.** VERIFIED by two separate measurements: 0 MMIO accesses during
   idle boot, full navigation and dozens of PLAY attempts.
2. **That "reference count" was an artifact.** The dominant literal
   `0x3F000000` is the IEEE-754 **float `0.5f`** (gain math), and
   `0x39000000` appears as the FourCC constant `0x39530000` and as data
   (ARGB pixels, byte tables). **Not a single pointer load** to either of the
   two bases, not a single `base+offset` access to the CalmADM2E's register
   map. VERIFIED by disassembly.
3. **The retailOS audio decoder is Apple's "MeCCA" framework with
   loadable codecs (`AudioCodecs`), decoding in software** (strings
   `MeCCADecode`, `MeCCA_MediaPlayer`, `MeCCA_StreamCache`, `AudioCodecs`,
   `alac: bit depth = %d`, `MP3ExampleTask`, `ERROR: unknownCodec loaded !!!`).
   INFERRED (strong).
4. **The real wall is not "modeling the DSP", it is that playback is
   unreachable:** the buttons do not register on the **song list**
   (`wiki/14 §5`), so the `play` path **aborts before** touching any
   decoding peripheral. Without sustained `play` there is no protocol to
   trace. VERIFIED.

**Consequence for the HLE:** that the retailOS audio mechanism is the
CalmADM2E via Rockbox's MMIO windows is **NOT VERIFIED — and the available
evidence contradicts it.** Building an HLE there blindly would be useless. This
document delivers (a) the register map *as it would stand if the CalmADM2E
hypothesis were confirmed* (from Rockbox, with no proven use), (b) what is actually
verified, and (c) a **decision-tree implementation plan** whose
first node is a cheap measurement that decides where the HLE goes.

---

## 1. Reconciling the two investigations

The two agree on the essentials (§0). They differ on one point that must be
recorded honestly:

| Point | Code investigation (disassembly) | Trace investigation (runtime) |
|---|---|---|
| Was "Now Playing" reached? | **Yes** (by pixel metric: `diff=28160` over list `diff=38030`), and on pressing PLAY it captured **18 accesses to `0x39300000`** (PCs `0x080a23e4`, `0x0814051c–58`), with a **polling loop** at pc~`0x08140540` | **No**: SELECT/PLAY **does not register** on the list; 30 rounds without a screen change, 0 accesses to any unmodeled block |
| Accesses to `0x39/0x3F` (ADM)? | **0** | **0** |
| Audio? | `play.wav` = **0 non-zero samples** | silence (did not reach play) |

**Reconciliation (INFERRED):** one investigation observed activity at
`0x39300000` during a PLAY attempt; the other did not even catch PLAY. What is
compatible with both is that **the accesses to `0x39300000` belong to the
Now Playing screen's compositor (cover art scaling), not to audio** — they match
the `MeCCA` 2D scaler (see §2b). That is: even if one run managed to
draw Now Playing, the **audio remained silence and the ADM remained untouched**.
Neither of the two observed an *audio decoding* peripheral.
The `0x39300000` block is, with strong evidence, **video/image**, not the audio
DSP — it is a **red herring**.

---

## 2. Question 1 — The register MAP

Two things that [17](17-classic6g-dsp-retailos.md) mixed up must be separated: the
*candidate* map (CalmADM2E, from Rockbox, **with no proven use in the retailOS**) and
the map *actually driven* (`0x391/2/3`, video).

### 2a. Candidate map: CalmADM2E (from Rockbox `s5l87xx.h:64-137) — NOT VERIFIED IN USE

Reproduced so that the implementer has it at hand **if** the decision node
of §6 confirms that audio goes through here. **Warning: not a single retailOS
access to these registers has been measured.** Two views of the same block.

**The host ↔ DSP mailbox:**

| Register | `ADM_BASE=0x3F000000 +` | `ADM_SFR_BASE=0x39000000 +` | Role (according to Rockbox) |
|---|---|---|---|
| `CONFIG0` / `CONFIG1` | `0x00` / `0x02` (16b) | `0x00` (`ADM_CONFIG`, 32b) | configuration/control |
| `COMMUN` | `0x04` (16b) | `0x04` (32b) | **communication control** (the "doorbell": the ARM gives the order here) |
| `DDATA0..7` | `0x06..0x14` (16b) | `0x10..0x2C` (32b) | **host → DSP data** (8 words: command arguments) |
| `UDATA0..7` | `0x16..0x24` (16b) | `0x30..0x4C` (32b) | **DSP → host data** (8 words: response) |

**The pointers to buffers in DRAM** (the host tells the DSP where its
regions are; the HLE would read them to know where to read the compressed data from and where to
write the PCM):

| Register | `ADM_BASE +` (H/L, 16b each) | `ADM_SFR_BASE +` (32b) | Role |
|---|---|---|---|
| `IBASE` | `0x26`/`0x28` | `0x50` | start of the **DSP firmware** (CalmRISC code). The HLE **ignores** it — it does not execute the Calm |
| `DBASE` | `0x2A`/`0x2C` | `0x54` | start of the CalmRISC's **data area** |
| `XBASE` | `0x2E`/`0x30` | `0x58` | MAC X area |
| `YBASE` | `0x32`/`0x34` | `0x5C` | MAC Y area |
| `S0BASE` | `0x36`/`0x38` | `0x60` | start of **sequential buffer 0** (INFERRED: input or output) |
| `S1BASE` | `0x3A`/`0x3C` | `0x64` | start of **sequential buffer 1** (INFERRED: the other one) |

**Sequential buffer status/control (only in the `ADM_BASE` view, 16b):**

| Register | `ADM_BASE +` | Role |
|---|---|---|
| `CACHECON` / `CACHESTAT` | `0x3E` / `0x40` | Calm cache control/status |
| `SBFCON` | `0x42` | **control** of the sequential buffers (starts/stops ring mode) |
| `SBFSTAT` | `0x44` | **status** of the sequential buffers (candidate for "block ready") |
| `SBL0OFF` H/L | `0x46`/`0x48` | current offset of ring 0 (how much consumed/produced) |
| `SBL1OFF` H/L | `0x4A`/`0x4C` | current offset of ring 1 |
| `SBL0BEGIN` H/L | `0x4E`/`0x50` | start of ring 0 |
| `SBL1BEGIN` H/L | `0x52`/`0x54` | start of ring 1 |
| `SBL0END` H/L | `0x56`/`0x58` | end of ring 0 |
| `SBL1END` H/L | `0x5A`/`0x5C` | end of ring 1 |

DSP clock: `DSPCLKMD` at `CLK_BASE+0x38` = `0x3C500038`. **VERIFIED that the
retailOS does NOT load it as a literal** (trace investigation) — another sign that
the DSP is not started this way.

**What each type is, for the HLE (if it were confirmed):** `COMMUN` = doorbell/command;
`DDATAn` = input arguments; `UDATAn` = results; `*BASE` = pointers to
DRAM regions; `SBFCON/SBFSTAT`+`SBLxOFF/BEGIN/END` = the two input/
output rings and their progress; `CONFIG0/1` = global control. **Which of S0/S1 is input and
which is output is UNKNOWN** (they were never programmed).

### 2b. Map actually driven during playback: `0x39100000 / 0x39200000 / 0x39300000` — VERIFIED, it is video/image (INFERRED strong)

This is what the trace DID capture. **Driver at `0x08140000–0x08141cc8`** (plus
satellites `0x080543e0`, `0x08072810`, `0x080998a4`, `0x080a23dc`, `0x080af960`),
with **33 `ldr rX,=0x391/2/3 0000` loads** and **0** of `0x39000000`.

Register map per block (from `admscan2.py`, 443 accesses), common to all three:

| Offset | Role (INFERRED from use) |
|---|---|
| `+0x00` | **control/status**: bit0 = run/enable, bit1 = idle/done (VERIFIED by the stop→wait pattern) |
| `+0x04` / `+0x06` | 16-bit header, written **byte-swapped (big-endian)** |
| `+0x08` | header word (32b, big-endian) |
| `+0x3c` | mode (low nibble) |
| `+0x044..0x0d0`, `+0x080..0x158`, `+0x180..0x1c0`, `+0x200..0x26c` | config/coefficient banks |
| `+0x280` = 1, `+0x284` bit0 | block **enable** |
| `+0x3c0..0x3d4` | reset/clear |
| (block `0x39200000` extends up to `+0x800`) | |

Verified sequence (disassembly): **enable** (`0x080a23dc`:
`str #1,[base+0x280]; [base+0x284] |= 1`) → **byte-swapped header** to
`+0x00/04/06/08` (`0x080a23fc`) → **run** (`+0x00` bit0) → **poll idle**
(`+0x00` bit1, up to 10 times with delay `bl 0x80c4e48`) → **teardown**
(`0x081404c4`: `+0x3c &= ~0xF; +0x00 &= ~1`).

**Why it is video/image and not audio (INFERRED strong):** the setup function
(`0x08140140`) loads `r7=0x39100000` and computes **two independent scaling
ratios, one per dimension** — `r7+0x64 = (dstW<<12)/srcW >> 3` and
`r7+0x68 = (dstH<<12)/srcH >> 4` (division via `bl 0x800f990`). **Two** 2D ratios
= image scaling (the cover art), not audio resampling (1D). It matches
the strings `MeCCAVideoDecode`, `MeCCA_VideoBufferMgr`, `MPEG4`. **Recommendation:
do not build the audio HLE here.** (Confirmable: playing *video* should
activate them; *audio without cover art* should not — see §7.)

---

## 3. Question 2 — The SEQUENCE the retailOS expects

### For audio (CalmADM2E): **UNKNOWN.**

- There is **no** write to `IBASE/DBASE/XBASE/YBASE/S0BASE/S1BASE` or to
  `CONFIG0/COMMUN/DDATAn` measured in any reachable state (0 accesses,
  VERIFIED). Therefore neither the firmware load, nor the startup,
  nor the "give me a block → here is PCM" loop, nor real values **are known**.
- **Cause of the gap (VERIFIED):** the `play` path aborts upstream (the
  list wall, §0.4). The instrument is validated (it fires with a control `xp`),
  so the 0 is a **real negative**, not a broken probe.

### What IS verified (video blocks `0x391/2/3`, for reference):

enable (`+0x280`/`+0x284`) → big-endian header (`+0x00/04/06/08`) → run
(`+0x00` bit0) → **poll of `+0x00` bit1** until done → teardown (`+0x3c`, `+0x00`).
It is **polling**, not interrupt. Useful as a template of the firmware's style
(big-endian headers, control via bit0/bit1), but **it is not the audio loop**.

---

## 4. Question 3 — The exact FORMAT (input and output)

- **DSP input (what to decode, from which buffer, size): UNKNOWN.** No
  `S0BASE/S1BASE` was programmed, so there is no input buffer address or size
  to dump. It is not even known whether the DSP would be passed raw MP3 frames
  or something pre-parsed by the ARM. INFERRED (weak) from the framework: the
  codecs are loadable (`AudioCodecs`, ALAC/MP3), which suggests input = compressed
  stream from the container.
- **Output (PCM): partially VERIFIED downstream.** The format the
  I²S consumes is already modeled and tested with Rockbox: **interleaved 16-bit stereo PCM at
  44100 Hz** (48000 depending on the track; the retailOS sets `I2SCLKDIV`), read by the
  `IIS0_TX` DMA → I²S FIFO at `0x3CA00010` (see [14](14-classic6g-audio.md)). **Which
  DRAM buffer the DSP would write the PCM to: UNKNOWN** (it would be the other
  S0/S1 ring, not programmed).
- **Endianness:** the video blocks `0x391/2/3` expect **big-endian** headers
  (VERIFIED by the byte-swap at `0x080a23fc`); if the ADM followed the same firmware
  style, its headers would be too (INFERRED weak, not applicable until
  the route is confirmed).
- **Control measurement:** all the PCM capturable today is **silence** (`play.wav` =
  0 non-zero samples; the "push of 6144 zero samples" at boot is the I²S's
  **init silence**, not a decode buffer).

---

## 5. Question 4 — The INTERRUPT or signal that closes the loop

- **For audio (CalmADM2E): UNKNOWN.** No IRQ or "block
  decoded" signal was observed, because the loop never started. The natural candidate in
  the candidate map would be `SBFSTAT`/`UDATAn` + an ADM IRQ line, but **there is no
  evidence** of which one or how it is armed.
- **What was observed in the video blocks is POLLING, not IRQ** (`+0x00` bit1). It is
  possible (INFERRED weak) that audio also uses polling of a status bit
  instead of an interrupt; this would have to be confirmed once `play` is reached.
- **IRQ context already modeled downstream:** the closing of the *playback* loop
  (requesting the next block) is today provided by the **audio DMA** (DMAC0, line
  `IIS0_TX`, `IRQ_DMAC0 = 16`, with `AUDIOPACE` pacing), which **already works** with
  Rockbox. If decoding were on the ARM (MeCCA sw), that is the only IRQ that closes the
  loop and **no DSP IRQ needs to be modeled**.

---

## 6. Question 5 — The HLE implementation PLAN (decision tree)

**Do not start by writing a `MemoryRegion` at `0x39000000`.** The evidence says
it would be dead work. The plan has a **prerequisite** and a **cheap decision
node** before committing to an architecture.

### Step 0 (PREREQUISITE, blocking) — Break the song list wall

Without sustained `play` there is no protocol to trace; both investigations
crashed here. The symptom (VERIFIED): the wheel moves the highlight in the
`Music → Songs` list, but **SELECT/PLAY do not register** *on the list* (they do
register in the preceding menus). Line of attack: compare how the firmware
consumes button packets in a **menu** (works) vs. in the **list
widget** (does not) — same wheel, different consumer. It is a UI wall upstream of
audio (see [14 §5](14-classic6g-audio.md), [15](15-classic6g-walls.md)).

### Step 1 (DECISION NODE, cheap) — Trace who fills the PCM buffer

As soon as there is `play`, reapply the **DSPTRACE** instrumentation (already written and
validated; see §8) **plus** a breakpoint/watch on the audio DMA's source:
the `IIS0_TX` DMA reads from a DRAM address → **set a write watchpoint on
that address** and see **who writes it**:

- **Branch A — ARM code writes it** (PC in the MeCCA framework's range, e.g.
  near `MeCCADecode`/`AudioCodecs`, or the PCM driver in IRAM `0x22008xxx`):
  then **the retailOS decodes on the ARM** and *there is no DSP to emulate*. Audio
  would already flow as soon as the ARM decoder runs on real PCM; the HLE comes down
  to ensuring the software codec does not depend on an absent peripheral. **This is
  the most likely outcome** (INFERRED strong, from ALAC/MP3ExampleTask/MeCCA sw).
- **Branch B — retailOS writes to `0x39000000`/`0x3F000000` show up**:
  then the CalmADM2E hypothesis is confirmed and the HLE of §6-detail is implemented.
- **Branch C — another peripheral fills it** (neither ARM nor ADM): that is where the real "DSP" is; its
  base will show up in the `io_floor` with the guest PC. That one is modeled, not the ADM.

**Cost:** a single run with `play`. It decides the entire architecture. **Do not write
model code before this step.**

### Step 6-detail — The CalmADM2E HLE, ONLY if Step 1 falls in Branch B

Template: `hw/arm/ipod_video/ipod_video_vc.c` (the 5G's VideoCore HLE) and
`hw/arm/ipod_classic/ipod_classic_disp.c` (Classic device style).

1. **`MemoryRegion`** for `0x39000000` (SFR, 32b) and `0x3F000000` (ADM, 16b) —two
   views of the same internal state— registered with priority **above**
   the current `create_unimplemented_device` ones (`ipod_classic.c:343` and `:358`), as
   the trace already does. `read`/`write` handlers that maintain a `struct` with
   `CONFIG`, `COMMUN`, `DDATA[8]`, `UDATA[8]`, `IBASE..S1BASE`, `SBFCON/SBFSTAT`,
   `SBLx*`.
2. **`COMMUN` handler (write)** = the trigger. When the ARM writes the
   "decode" value (to be determined by tracing in Branch B), the model:
   a. reads `S?BASE` (the input one) + `SBLxOFF/BEGIN/END` to locate the compressed
      frame in DRAM (`cpu_physical_memory_read`);
   b. **decodes on the host** with **libmad** (MP3) or **ffmpeg/libavcodec** (MP3+
      AAC/ALAC), hooked in here — this is the decoder insertion point;
   c. writes the resulting PCM to the output `S?BASE`
      (`cpu_physical_memory_write`), advances `SBLxOFF`, marks `SBFSTAT`/`UDATAn`;
   d. **notifies** the ARM: set the status bit it waits for and, if Branch B showed an
      IRQ, `qemu_irq_raise` on the ADM line (number to be determined).
3. **Coexistence with I²S/DMA (already modeled, do not touch):** the HLE only produces PCM in
   DRAM. From there, the existing path carries it on its own: DMAC0/`IIS0_TX` with
   `AUDIOPACE` → I²S FIFO `0x3CA00000` → CS42L55 → speakers/WAV. `ipod_classic_i2s.c`,
   `pl080.c` and the codec **are not touched**.
4. **DSP firmware:** the HLE **ignores** `IBASE` and the CalmRISC code. That is the
   advantage of HLE: there is no need to have or execute the Calm firmware.

### Risks and dependencies

- **Major risk (likely):** that Branch B **does not happen** and the whole §6-detail is
  inapplicable. That is why Step 1 goes first. VERIFIED that today the evidence
  points to Branch A.
- **Build dependency:** libmad/ffmpeg in the QEMU tree (`meson` flags,
  license). Only relevant in Branch A or B.
- **UI risk:** the list wall (Step 0) may be non-trivial; without it, nothing
  else is measurable.
- **Battery risk:** the "Connect to Power" reset at ~46 s (`wiki/15`) can
  cut the capture short; enter `play` earlier or mitigate the battery model.
- **Do not break:** the instrumentation must be temporary (priority above,
  reads that return 0 just like the current `unimp`) so as not to regress Rockbox
  or the 6G/5G `navtest`. Both investigations left the tree byte-identical to the
  baseline; repeat that discipline.

---

## 7. What is still UNKNOWN and how to find out

| Unknown | How to close it |
|---|---|
| **The real audio mechanism (ARM sw vs ADM vs another peripheral)** | Step 1 of §6: watchpoint on the DRAM source of the `IIS0_TX` DMA, see the PC that writes it. One run with `play`. **It is the mother question.** |
| **The song list wall** (blocks everything) | Step 0 of §6: compare the consumption of button packets in a menu vs. the list widget |
| **Whether `0x391/2/3` are video or (less likely) audio** | Play **video** (they should activate) vs. **audio without album art** (they shouldn't). Closes the classification |
| **Decode command, I/O buffers, IRQ, input format** (only if Branch B) | Trace of `COMMUN`/`DDATAn`/`S?BASE` with the DSPTRACE instrumentation already prepared, as soon as there's `play` |
| **External sources for the ADM protocol** | Check whether **freemyipod/qemu** (branch `s5l8702-dev`) models the ADM, and whether **giek** documents MeCCA/AudioCodecs. (Note: `giek/architecture_MEMORY_MAP.md:31` labels `0x39000000` as "GPIO" —low confidence, it contradicts the working model— but it reinforces that "these windows = decoder" **is not proven**.) |

---

## 8. Consolidated confidence table

| Claim | Mark | Evidence |
|---|---|---|
| `0x39000000`/`0x3F000000` are never loaded as a pointer nor accessed via MMIO | **VERIFIED** | `ldrscan.py`/`osana.py lit` = 0 LDRs; 0 accesses during boot + navigation + 30 rounds of PLAY (two investigations); an `xp` control proves the instrument fires |
| `0x3F000000` in code = float `0.5f` (gain math) | **VERIFIED** | every `mov #0x3f000000` → `bl 0x8374b18` (soft-float); neighbor `0x3f800000`=1.0f |
| `0x39000000` in code = constant `0x39530000` (FourCC) + ARGB data | **VERIFIED** | `0x0810626c`: `orr`+`orr`; word sweep = data |
| HW actually driven during playback = `0x39100000/200000/300000` | **VERIFIED** | `io_floor` at runtime (PCs `0x080a23e4`, `0x0814051c–58`); 33 `ldr` literals |
| Those `0x391/2/3` blocks are image scalers (video/album art), not audio | **INFERRED** (strong) | two `(dst<<12)/src` ratios per dimension; strings `MeCCAVideo`/`MPEG4` |
| Sequence of the video blocks: enable→BE header→run→poll idle→teardown | **VERIFIED** | disassembly `0x080a23dc`, `0x080a23fc`, `0x081404c4` |
| retailOS audio = silence in everything reachable | **VERIFIED** | `play.wav` 0 non-zero; no audio peripheral in the `io_floor` |
| Audio decode is MeCCA/AudioCodecs in software (probably ARM) | **INFERRED** (strong) | strings ALAC/`MP3ExampleTask`/`MeCCADecode`/`AudioCodecs`; absence of driven audio HW |
| Playback is unreachable because of the list wall (SELECT doesn't register) | **VERIFIED** | 30 rounds with no screen change; reproduces `wiki/14 §5` |
| CalmADM2E register map (§2a) | **VERIFIED as Rockbox's def. / NOT VERIFIED in use** | `s5l87xx.h:64-137`; 0 measured accesses from the retailOS |
| Audio decode sequence, I/O buffers, command, IRQ, input format | **UNKNOWN** | blocked by not reaching sustained `play` |
| Downstream PCM output = 16-bit interleaved stereo 44100 Hz over I²S | **VERIFIED** | modeled and tested with Rockbox ([14](14-classic6g-audio.md)) |

---

## 9. Specification summary (for the implementer in a hurry)

1. **Don't** model `0x39000000`/`0x3F000000` yet: there's no evidence that the
   retailOS uses them for audio, and there is evidence that it doesn't.
2. **First** break the song list wall (§6 Step 0) to get `play`.
3. **Then**, one cheap run (§6 Step 1): watchpoint on the DRAM source of the
   `IIS0_TX` DMA → look at the PC that writes the PCM. That decides everything:
   - PC in ARM/MeCCA → **decode on the ARM, there's no DSP to emulate** (most
     likely);
   - writes to `0x39/0x3F` → implement the CalmADM2E HLE (§6-detail, map §2a);
   - another peripheral → model that one.
4. Whatever the PCM producer is, **everything downstream is already done**
   (I²S+DMA+CS42L55+capture). The HLE just has to leave real PCM in DRAM.

---

## 10. References and artifacts

- [17-classic6g-dsp-retailos.md](17-classic6g-dsp-retailos.md) — the starting
  plan (whose premise this document corrects by measurement).
- [14-classic6g-audio.md](14-classic6g-audio.md) — the downstream audio path
  (I²S/DMA/CS42L55), already modeled, and the measurement of the silence.
- [15-classic6g-walls.md](15-classic6g-walls.md) — the Classic's walls,
  including the list one and the battery reset; and the doctrine of validating
  the instrument.
- [16-classic6g-rockbox.md](16-classic6g-rockbox.md) — the audio path that
  **does** make sound (Rockbox decodes on the ARM).
- `assets/rockbox/firmware/export/s5l87xx.h:64-137` — definition of the
  CalmADM2E map (source of §2a; no verified use in the retailOS).
- `hw/arm/ipod_video/ipod_video_vc.c` — **the HLE precedent** (the BCM2722's
  VideoCore): model the protocol, not the ISA. Pattern to repeat if Branch B is
  confirmed.
- DSPTRACE instrumentation (temporary, reverted in both investigations; ready
  to re-apply): the `DISPTRACE` pattern in `ipod_classic_disp.c`, mapped on top
  of the `create_unimplemented_device`s at `ipod_classic.c:343`/`:358`.
- RE tools used: `ldrscan.py`/`osana.py lit` (literal loads, correct where
  `findlit.py` fails), `admscan.py`/`admscan2.py` (per-base taint),
  `dumpctx.py`; disassembly with VA base `--adjust-vma=0x07FF51B8`.

---

## 11. Audio bring-up (continued): the player DOES start; three CLOCK walls down; the decode is still silent

With the PMU fixes (reliable SELECT + no battery reset,
[15](15-classic6g-walls.md)) and the non-invasive tracer `pctrace`, "Step 0" of
§6 was tackled. Everything below is **VERIFIED** by PC trace (without stopping
the CPU) or runtime memory reads. VA = offset in `osos_decrypted.bin` +
`0x07FF51B8`.

### 11.1 The player DOES start — refutes the framing "the handler is neither dispatched nor fails"

SELECT on **Shuffle Songs** (main menu):
- `HandleShuffleSongsSelected` (0x081fa9e4) **is dispatched**.
- It passes its gate: singleton `*0x089117fc` → `+0x3f4` = 4 (number of songs;
  `HandleMusicSelected` reads the same field), 4≠0, doesn't abort.
- The full startup runs: `ShufLAUNCH` (0x081faa34) → `setup20` (0x0826b858) →
  `StartPlayback` (0x08150d84) → `playA` (0x08024798), which **queues the 4
  tracks** (it iterates the playlist accessor `0x0802fe08`).
- `ShufLAUNCH` **completes**: it returns from setup20, invokes `GotoNowPlaying`
  (0x081faa90), the handler **returns** (0x081faaa4). The Now Playing
  controller runs (region 0x081a0000–0x081b2000, 618 distinct blocks).

All of the **synchronous** startup machinery runs. The wall is downstream, in
the **asynchronous** MeCCA pipeline — not in the handler dispatch.

### 11.2 Three audio CLOCK walls (polls of hardware bits the model didn't set)

**Wall A — I2S CLKCON bit 1 (measured: 1.6e8 reads).** The retailOS's audio
driver in IRAM (0x22008808–0x2200881c) **disables** the I2S clock
(`str 0,[CLKCON]`) to reconfigure it and then **spins** `ldr CLKCON; tst #2;
beq` waiting for bit 1 ("clock stopped" status). The model returned the written
0 → infinite spin. **Fix** (`hw/arm/ipod_classic/ipod_classic_i2s.c`, CLKCON
read): bit 1 = "clock stopped", set when bit 0 (enable) is 0.

**Wall B — SM1 (0x38500000) identity register.** The audio clock init stores
0x38500000 in its handle table (setter `0x08083df0`, table @0x08ade488) and
then reads **SM1+0xc80** (getter `0x08075d64`), compares it with the magic
`0x004e7309` (literal `*0x080821cc`) and does **`b .`** (eternal hang at
0x080819b8) if it doesn't match. SM1 was unimplemented (0) → hang. VERIFIED at
runtime: at the hang SM1+0xc80 read 0. **Fix**
(`hw/arm/ipod_classic/ipod_classic.c`): SM1 goes from
`create_unimplemented_device` to a minimal device that returns `0x004e7309` at
+0xc80.

**Wall C — SM1 "clock ready" bit.** After the identity, the init **polls
SM1+0xa98** waiting for bit 2 (`tst r0,#4; beq` at 0x08081af8) — the "clock
running/lock" flag. SM1=0 → spin at 0x080a7658. **Fix**: SM1+0xa98 returns
bit 2.

**SM1 (0x38500000)**, labeled "clock divider" in the map, is actually the
**audio clock controller**: the retailOS does a multi-register handshake
(identity +0xc80, status +0xa98, writes to +0x824/+0xc48). The device models
ONLY the handshake the startup needs, not the full controller (annotated in the
code).

### 11.3 Current status: the OUTPUT gets armed, the DECODE is still silent

With the three fixes **+ `AUDIOPACE=1`** (necessary: DMA pacing is what makes
the pipeline advance; without it the audio task goes to sleep before the clock
init), the **output** is fully initialized (measured at runtime): I2S
`CLKCON=0x1`, `TXCON=0x0b100019`, **`TXCOM=0x6` (run bit ACTIVE)**, audio clock
armed.

BUT the **MeCCA software decoder still produces no PCM**: the non-invasive
mem-watch on the ping-pong buffers (0x22008c5c/0x22009c5c) measures **0
writes** after the SELECT; the I2S runs but drains SILENCE (zeroed buffer);
AUDIOWAV comes out as zero; the system enters WFI idle; Now Playing isn't
drawn. The decode task is **BLOCKED** waiting for an event (candidates: the
compressed input from disk via StreamCache that doesn't complete; or a "buffer
ready" semaphore that doesn't get posted).

Summary: **the OUTPUT walls (I2S clock/SM1) fell; the wall that remains is the
DECODE one** — the core of Branch A. The MeCCA decode doesn't start producing
PCM even though the output is now ready to consume it.

### 11.4 Tool: non-invasive mem-watch in `pctrace`

`contrib/plugins/pctrace.c` extended with `memw=0xLO-0xHI` (+`memcap=N`): it
logs every CPU STORE in the range with its PC, without stopping the CPU (memory
callback on the vCPU thread) — Branch A's watchpoint: as soon as the decode
writes PCM into 0x22008c5c, the decoder's PC will show up. **Cost**: it
registers a callback on every instruction → slows things down a lot; DON'T use
it during the full boot (the guest misses deadlines and the battery model
resets). Use a short window or only once playback is under way.

### 11.5 What comes next

- **The blocked MeCCA decode** is the remaining wall. Find the RTXC blocking
  point. QEMU 8.2's `pctrace` doesn't read registers, so the LR of the block
  needs another route: the RTXC task's stack, or a plugin using QEMU 9.x's
  register-read API. Candidates: an MP3 read (ATA/StreamCache) that doesn't
  complete; a semaphore that isn't posted.
- **SM1 (0x38500000)**: model the audio clock controller properly, not just
  the minimal handshake; the decode likely depends on the REAL clock, not on
  faked "ready" bits.
- **AUDIOPACE**: the retailOS audio startup needs it. Evaluate turning it on by
  default (safe for Rockbox according to [16](16-classic6g-rockbox.md)) or
  auto-enabling it when the retailOS starts the I2S.
- No regression: `navtest` 6G passes (05-select 19498 px, wheel both halves;
  09-into-music is the known SELECT flakiness, not a regression; 0 resets) and
  Rockbox still navigates. The CLKCON/SM1 fixes are only exercised on the
  retailOS audio path.

### 11.6 SM1 modeled as a stateful device (replaces the faked bits)

The three clock fixes from §11.2 were replaced by a **stateful QEMU device**,
`hw/arm/ipod_classic/ipod_classic_sm1.c` (+ `.h`), registered in the machine
instead of the `create_unimplemented_device`. It models the COHERENT handshake
(state derives from the guest's writes, not hand-set constants):
- **+0xc80 (ID)** returns the magic `0x004e7309`.
- **sub-blocks** (`base + {idx*0x20; 0x100/0x180/0x200/0x230}`): when a command
  is written to `sub+0x10`, `sub+0x14 & 7` starts reading **7** ("reached
  state"), which is what the 10-attempt poll at `0x080ba768` waits for.
- **+0xa98 bit2 (clock-ready)** and **+0x400 (run latch)** are derived from a
  `powered` flag that is set by the first bring-up write (channel command,
  `+0x824`, `+0x400`, `+0xc48`).
- **+0xa48 (playback position)** is derived from `i2s->tx_samples/2`.
- Everything else is a latch (stores what's written, reads 0 at reset).
  `SM1TRACE=1` prints every access with its PC.

**MEASURED (`SM1TRACE` trace, disk `~/6g/Emulti.img`, `AUDIOPACE=1`):** on
Shuffle Songs play the handshake runs **ONCE, 33 accesses, and COMPLETES
without spinning**: `rd +0xc80 -> 0x004e7309`; `wr +0x110 = 4`;
`rd +0x114 -> 7`; `wr +0x110 = 16`; `wr +0x110 = 96`; codec config
`+0x93x..+0x97x` (a table from `0x080b68b4`); at the end
`rd +0xa48 -> 0x002a9000` (position advancing). **Measured conclusion: the SM1
model is fine and is NOT the wall** — the handshake we used to fake is now
really satisfied, and still there's no sound. (It confirms the auditor's
warning from the opposite direction: modeling SM1 "for real" was correct but
insufficient.)

### 11.7 The premise "0 writes to the ping-pong" was an artifact; the wall is the DECODE

Re-measured with `pctrace memw`: the buffer is written **millions** of times
(IRAM driver `0x22000294/0x22000298` at audio rate, almost all silence) and the
**SW mixer `0x080fb2b0`** puts in exactly the **UI clicks** (`AUDIOWAV`:
~9,360 non-zero frames, peak 11444, L==R, ~15 isolated bursts — clicks, NOT
music). The **output pipe works end to end**; what's missing is for the
decoded MUSIC to enter it. (Matches the second report from the previous
session.)

### 11.8 The consumer path, fully mapped (trace + gdb) — the copy SKIPS for lack of PCM

With `pctrace range=` triggered at `StartPlayback` (0x08150d84, `hits=1`), the
footprint of the decode startup was captured: **~10,787 distinct blocks in
~367 ms**, then **zero new blocks** — the system settles into a stable loop
(back to the UI event loop `0x08105a1c`/`0x0812xxxx`). Regions touched: media
player `0x0815`, StreamCache `0x081f`, ALAC codec `0x0808`, codec table
`0x0832` (75 blocks).

The **consumer chain** (reconstructed with `pctrace pc=` and **gdbpoke** —
stack + registers in the copy, without corrupting input because the copy runs
AFTER the SELECT was dispatched):

```
  audio task 0x07ffb2d0 (RTXC task entry; no direct bl)
    -> veneer 0x07ff89f8 -> audio engine 0x0808170c   (SM1 bring-up + codec switch)
      -> mixer 0x0809a74c   (iterates the stream list: ldr r4,[r4,#4]; calls render per stream)
        -> render 0x08016f14 -> 0x08016f68
          -> COPY PCM 0x08016e44   (calls the SM1+0xa48 position getter at 0x08091710)
```

The **copy 0x08016e44** runs **ONCE** (`copyFn hits=1`) and takes the **SKIP**
branch (`copySKIP 0x08016ecc hits=1`, `copy32 0x08016eb4 hits=0`). The gate:
`ldrb r1,[r7,#5]; cmp r1,#1; bne skip` at `0x08016e88`. **gdbpoke measured at
runtime** (stable across runs): `[r7+5] = 0` (the gate requires 1), `[r4] = 0`
(32-bit path), source `r0 = 0x08889098`, descriptors `r4=0x0bb12d54`,
`r7=0x0bb12eb4`. The dump of the **source 0x08889098 is NOT PCM** (pointers to
itself + `0x5da35da3` fill); the descriptors are **half-initialized** (`0x5da3`
fill, which appears twice in the whole image → a heap value, not PCM). The copy
**is right to skip**: there are no valid samples to copy.

### 11.9 H1 CONFIRMED by measurement: the decoder doesn't write a single sample

`pctrace memw=0x08888000-0x0888c000` (the region of the source 0x08889098)
during play: **8,192 stores, from only 2 PCs — `0x22000198` and `0x220001a0`,
which are the IRAM memset/clear. ZERO writes from the decoder.** That is, the
stream buffer gets **cleared** but **never filled with PCM**. This settles H1
vs H2:

> **The MeCCA decoder never produces PCM (H1). It's not a routing/mute problem
> (H2): there are no samples to route.** The engine `0x0808170c` runs once
> (that's why the SM1 handshake is single), mixes the empty stream, the copy
> skips, and the task blocks. The PRODUCER task (decoder), upstream of the
> stream, is stopped before frame 0.

Side data: `media_dispatch_chain 0x081f87f8` runs 248 times and `strcmp`
131,240 times in the run (MeCCA framework very active with string-keyed
lookups). The codec (ALAC region `0x0808`, table `0x0832`) **does** do setup,
so it is **not** a "codec not found" case like the fonts wall (§2): the codec
initializes but blocks before decoding.

Relevant RTXC mechanism (already documented in `ipod_classic.c:634-641`): tasks
do a `KS_wait` WITHOUT timeout on a DMAC0 channel's semaphore and are only woken
by the terminal-count ISR (IRQ 16). That mechanism **works** (storage mounts,
output drains). Pacing (AUDIOPACE) only affects the IIS0_TX line; the decoder's
mem-to-mem DMA is NOT paced. So the decode block is **not** a missing
IRQ/pacing — it's a software semaphore/event upstream.

### 11.10 The exact obstacle that remains and how to close it

The MeCCA decoder (a separate RTXC task, upstream of the stream) blocks in a
`KS_wait` before frame 0 and never fills `[r5+12]` (the mixer's source). Its
blocking PC/LR is still **UNKNOWN**: the mixer/render/copy chain is fully mapped
(consumer), but the PRODUCER task wasn't located statically (vtable dispatch +
task entries with no direct `bl`). To close it:
- **gdb already works during play** (it doesn't corrupt input if the breakpoint
  lands after the SELECT dispatch): set a breakpoint on RTXC's `KS_wait`
  primitive and, when the audio task blocks during setup, read `bt`/LR → the
  decode function and which semaphore it's waiting on. `KS_wait` still has to
  be located (map the RTXC task table / the audio task's TCB).
- New reusable tool: `gdbpoke.py --memreg IDX[+OFF]:LEN` (register-relative
  memory dump) — what made it possible to read the copy's source and
  descriptors without guessing addresses. `pctrace` gained `memwpc=` (store
  destination at a specific PC).

### 11.11 The live RTXC hunt: TWO hypotheses ruled out by measurement, and the wall reframed

Live tracing session with `gdb-multiarch` (it IS in the WSL, 15.1) + `pctrace`.
Play started via **曲をシャッフル (Shuffle Songs)**, the last item of the main menu
(Japanese in this firmware), disk `~/6g/Emulti.img`, `AUDIOPACE=1`. Everything below
**VERIFIED** except where marked.

**Technique that works (note it down):** `gdb.execute("continue")` from gdb's Python API
runs ASYNCHRONOUSLY against QEMU's gdbstub and fails ("Cannot execute while
running"); a **`.gdb` command file** with CLI `continue` (and `while`
loops) DOES block. All the live work was done with `-batch -x script.gdb`.
Recipes in `~/dbg/` (WSL): `enum_bt.py` (dump + backtrace of all 56 tasks in one
go, a single attach), `catch.gdb` (catches the engine and follows it until it blocks),
`catch4.gdb` (reads the state of the audio HW). **A single attach per capture** — the
attach/detach cycling is what induced the EIC spin (see below).

**A) The EIC spin (0x22002e24) is NOT the wall — RULED OUT by non-invasive measurement.**
The clock/power routine `0x22002dc0` (`iPodPowerProfile.txt`) reprograms a PLL
(`r0=CLK 0x3C500000`, `r1=EIC 0x39A00000` = `r0-0x2b00000`) and polls EIC bit0
(`0x22002e24`) and bit12 (`0x22002e48`) with IRQs masked; the unimplemented EIC reads
0 → eternal spin. **`pctrace pc=0x22002dc0`/`pc=0x22002e20` in a clean play (no
gdb): hits=0.** The routine is NEVER entered during playback. The spin that the
previous session captured was 100% an artifact of gdb cycling (an IRQ lost during a
detach leaves the clock routine half-done). The EIC is not part of the playback path (it is modelled for standby; see
`ipod_classic_eic.c`). The clean steady
state is WFI idle at `0x22003b0c–0x22003b30`, cpsr with IRQs ENABLED.

**B) The wall is NOT a missing peripheral nor one returning wrong values — RULED OUT.**
`-d unimp,guest_errors` during play: the ONLY unmodeled accesses are 43
config **writes** to GPIO `+0x200` (wheel/button IRQ setup, not reads
that condition anything). **Zero** accesses to AES / SHA / ADM(`0x39/0x3F`) / high SM1 window
/ io-floor. The decode does not reach any unmodeled block. `pctrace` of the chain:
`audio_engine 0x0808170c`, `mixer 0x0809a74c`, `copyPCM 0x08016e44` run **exactly
1 time**; `copy32 (real PCM) 0x08016eb4` = **0**. `KS_ISRSignal 0x22001ca4` = **7621
times** (the ISRs DO fire). SM1: `SM1TRACE` during play = **0 accesses after SELECT**
(SM1 is not polled during streaming); `catch4.gdb` at idle: SM1 `STAT(+0xa98)=0`,
`RUN(+0x400)=0` — not even "powered". SM1 is irrelevant on the play path.

**C) The low-level output WORKS — identical to Rockbox (VERIFIED by registers).**
`catch4.gdb` at play idle read the audio HW: **DMAC0 channel 1** active —
`SRC=0x22009c5c` (IRAM ping-pong), `DEST=0x3ca00010` (I2S FIFO), `LLI=0x22010020`
(continuous chain), `CTRL=0x84249800` (bit I=terminal-count IRQ), `CONF=0x8a81`
(E=1 enabled, `dest_id=0xA`=IIS0_TX, flow=1 mem→peri, ITC=1). I2S `TXCOM=6` (RUN),
`CLKDIV=0x110`. It is **exactly** the mechanism Rockbox uses (LLI chain +
terminal-count IRQ 16) and that already plays. DMAC1 unused. **The wall is NOT downstream
of the ping-pong.** The `SM1 POS` advanced to 0x27d800 (≈2.6M): the I2S drains silence
end to end from boot (which is why `SM1pos` in `StartPlayback` was already 1.97M ≈
44 s of boot silence).

**D) The audio engine runs in TASK 0x25 (VERIFIED by breakpoint).** `catch.gdb`
broke at `0x0808170c`: it runs in task `0x25` (MeCCA worker, entry `0x080833d0`),
called from the IRAM audio driver (`lr=0x22006304`, IRAM `0x22006xxx` = DRAM shadow
`0x07ffbxxx`; the "audio task 0x07ffb2d0" of §11.8 is IRAM `0x22006118`).
After its ONLY run, task `0x25` blocks on **mailbox/exchange 0x52** via service
**opcode 14** (wrapper `0x22004028`, a mailbox `KS_receive`; the handler
`0x22000950` indexes the mailbox table at **`0x0897783c` stride 32**, distinct from the
opcode-1 semaphore table `0x0897fb1c`). The copy `0x08016e44` runs 1 time and SKIPS
because `[r7+5]=0` (PCM not ready) — not because of the SM1 position.

**E) Runtime RTXC task map (56 TCBs, VERIFIED).** `enum_bt.py`: TCB array
`0x0897ff70` stride 52 (`+12` entry, `+16` savedSP, `+28` state, `+32` tasknum; the
TCB does not store a pointer to the name). States seen: `0x40`=semaphore wait (opcode 1,
resumePC `0x2200433c`), `0x08`/`0x20`/`0x04`/`0x01`=other waits (opcodes 14/4/…),
`0x00`=free/running. ~30 generic `0x080833d0` workers. Key pieces:
- **task 0x20** = StreamCache/media task (bt `0x08171500→0x081f39a4→0x081d0a2c→…→
  0x081fed34`), **idle waiting to RECEIVE on exchange 6** (opcode 4). It is an idle
  consumer, not "the blocked producer" that §11.10 believed.
- **task 0x25** = audio engine/driver, blocked on exchange 0x52 after 1 run.
- Many workers stopped at **`0x080c8998`** (return of `bl 0x8033d8c` = the `KS_wait`
  of the synchronous RPC `0x080c8968`): they make a request and wait for the reply. The RPC
  `0x080c8968` is generic (get-event `0x8033b70` → post `0x80cc0e4` → signal
  `0x8033af8` → wait reply): it runs ~3728 times in 18 s of play, it is NOT
  audio-specific.

**F) The disk is NOT the problem.** The track in `Emulti.img` is a **valid M4A/ALAC**
(`ftyp`@0x818004, `moov`, `mdat`@0x824824, fourcc `alac`) — a well-formed MPEG-4
container, not corrupt. The ALAC codec initializes (§11.9); the "ID3" found in the
scan is coincidental noise.

**REFRAMING THE WALL (measured, honest):** at the hang point the system is a
**MeCCA messaging deadlock**: ALL tasks are blocked on inter-task exchanges/
mailboxes, and NONE is polling a peripheral. Therefore **no
change to a peripheral's model can unblock it directly** — what is missing
is a *message* (a task-to-task `KS_send`/`KS_signal`) or a step of the SYNCHRONOUS
setup that should have dispatched the first decode and did not. The ISRs that do fire
(timer tick + DMA IRQ 16) wake the low-level output driver (it fills
silence) but do **not** trigger the decode task. The decode never produces frame 0.
The deadlock is HARD (§11.8: 0 new blocks after 367 ms), not a slow timeout.

**The next node, precisely:** trace the SYNCHRONOUS playback setup from
`StartPlayback 0x08150d84` (media player `0x0815xxxx` → opening the M4A → ALAC codec
init → **the point where it should queue/signal the first decode**) and find the
branch that does NOT dispatch that request. Non-determinism clue: the previous session
reached the SM1 handshake (33 accesses, §11.6) and this one did not (0 accesses) — the same play
gets stuck at different points → **there is a race/timing issue** (candidate: the moment
of the first IRQ 16 relative to the setup). Suggested method: breakpoint on the `KS_receive`
of the decode worker and on `0x080c8968`/`0x8033af8` filtering by the exchange the
media player touches; or compare the `pctrace range=` tree of a play that progresses vs.
one that gets stuck.

**Timing experiment TRIED and RULED OUT (this session):** hypothesis "the first
IRQ 16 of the audio DMA arrives too early and the decode setup loses/tramples
the signal". A knob `AUDIOPACE_START_MS` was temporarily added that delays the first
drain tick (and with it the first terminal-count IRQ 16) by N ms; tested with 1500 ms.
Result: **does not confirm** — it doesn't change the wall (`copy32=0` remains), and worse, delaying the
output ~1.5 s tends to **induce resets** (the retailOS seems to watch playback
progress and resets if it stalls). Consistent with the diagnosis: the decode does NOT
wait for IRQ 16 (the low-level output runs on its own, §C) but for an upstream task
MESSAGE. The knob was **reverted** (clean tree). The timing of IRQ 16 is not the lever;
the next effort goes to the synchronous decode setup, not to the DMA pacing.

**Hygiene:** zero changes to model sources (only gdb reads/breakpoints and
`pctrace`, already in the tree). Tree byte-identical to the baseline. Rockbox/navtest intact.
Session instrumentation in the WSL's `~/dbg/` (outside the repo).

### 11.12 The wall re-characterized by MEASUREMENT: it is NOT a hardware event — it is a pure rendezvous deadlock (and corrections to §11.6/§11.8/§11.F)

Measurement session dedicated to the wait graph and the decode setup. Clean play of
`~/6g/Emulti.img`, `AUDIOPACE=1`, triggered by Shuffle Songs. Everything below
**VERIFIED** by gdb read/breakpoint (non-invasive) or temporary model
instrumentation (reverted). Recipes in `~/dbg/` (`probe6.sh`=wgraph, `probe7.sh`=HW+timers,
`probe8.sh`=codec-setup, `probe9.sh`=ISR rates; `catch5.gdb`, `catch_setup.gdb`).

**A) The track is AAC-LC, NOT ALAC (corrects §11.F). And the codec IS configured (refutes the
hypothesis "AAC → default branch").** The 6 tracks of `Emulti.img` are `mp4a` in the `stsd`
(esds objectTypeIndication `0x40`, ASC `0x1210` = AAC-LC 44100/stereo), measured with
`ffprobe`+a dump of the `stsd`. The "alac" string seen by previous sessions is the
demuxer's fourcc table + bytes of `mdat`, not the sample entry. **Address CORRECTION:** the
getter `0x08015860`→IRAM `0x22006078` returns the state struct at **`0x220106dc`**
(not `0x22008c4c`, which is only an internal init latch); the codec type lives at
`0x22010734` (+0x58) and the gate at `0x22010723` (+0x47). With low-perturbation
breakpoints (`probe8.sh`) it was MEASURED live during setup: `CODEC_SETUP`
(IRAM `0x22006574`) runs with **type=0x01**, `GATE_SET` (`0x220065ac`) sets gate=1, and the
engine `0x0808170c` reads **gate=1, type=1** → takes **case 1** of the switch (`0x0808182c`),
which DOES wire up the pipeline. **Type 1 is a handled case; the engine is configured correctly.**
(The snapshot reads that gave type=0xff/gate=0 are *post-teardown*: the state is
reinitialized when playback fails — a mirage. Measure live, not at the hang.)

**B) The exact wait graph (regime A, stable deadlock, resets=0, `wgraph.py`).**
- **t2a** (AAC decoder, cluster `0x08123xxx`) BLOCKED receiving on **mailbox 0x09**
  (`0x08977964`); `M09.seq≈52` → StreamCache served it ~52 chunks. It is **starved**
  (waiting for input), not failing to decode.
- **t25** (audio engine) BLOCKED receiving on **mailbox 0x52** (`0x08978284`);
  `M52.seq≈18-20`. It ran its setup ONCE (type 1) and waits for commands.
- **t20** (StreamCache) BLOCKED receiving on **exchange 6** (`sema[6]=0x20`,
  `0x0897fb34`). It is the SERVER: owner of mailboxes 0x09/0x52/0x4e.
- t20 served ~52/18 times and then fell asleep on exchange 6 waiting for trigger #53. The
  hang point is **reproducible/deterministic** (M09≈52-53, M52≈18-20 in 4 runs).

**C) There is NO lost or mistimed hardware event — RULED OUT by direct measurement
(`catch5.gdb` at the hang).** VIC0 `RawIntr=0` (ZERO IRQs pending), `IntEnable=0x21a7010f`
(irq16 DMAC0 and irq29 ATA enabled). DMAC0 `RawIntTC=0`, **ch0 (storage) IDLE
(E=0)** — the disk has already been read, there is no hung DMA; ch1 (audio) is still E=1 going around
its LLI. ATA quiescent (transfer complete). **There is no pending interrupt that
the guest is not servicing.**

**D) The ISRs that matter DO fire, continuously, INSIDE the hang — RULES OUT "dead
tick"/"lost IRQ" (temporary `IRQDBG` instrumentation, `probe9.sh`).** The **1 kHz RTXC
tick** (timer B, 16-bit, ECLK/2, DATA0=6000, INT_EN, IRQ 8) fires **~1000/s** without
stopping during the deadlock. The **audio IRQ 16 (DMAC0 ch1)** fires **~50/s** (real
tempo, ~43 bursts/s of 2048 samples). Storage (ch0) is stopped (disk read).
That is: the RTXC scheduler receives its tick 1000 times per second and the output driver its
IRQ 50 times per second, **and still nobody signals exchange 6.**

**FINAL REFRAMING (measured, honest):** the wall does **NOT** fit any of the three cases
of "fix in the model via a peripheral event": there is no mistimed external event
(C: no pending IRQ), it is not that the tick/DMA is missing (D: both alive), and there is no hardware
latency to reproduce (the disk/ATA/SM1 are correct and idle). It is a **PURE inter-task
rendezvous deadlock**: the `KS_send`/`signal` to exchange 6 that would wake
StreamCache comes from a task on the **data path** that is itself blocked
(starved t2a ↔ t20 on ex6 ↔ t25 on mailbox 0x52). The first domino —the FIRST
PCM sample (`copy32 0x08016eb4` = 0 always)— is never produced, so the
producer↔consumer cycle never starts, and neither the 1 kHz tick nor the audio IRQ breaks it
(their handlers do not touch exchange 6 while the drained buffer is silence).

**What remains UNKNOWN (the exact obstacle):** WHO should signal exchange 6 on
cold start and why their task doesn't run. It is a protocol problem of the
MeCCA framework (pipeline bootstrap), not of a peripheral register/timing. Since the
regime-A/regime-B race is sensitive to guest speed (heavy instrumentation →
regime B), catching the `send`-to-exchange-6 live requires a VERY low-perturbation probe
(watchpoints on the kernel data page `0x0897xxxx`, which is hot, push
to regime B). Suggested route: locate the veneer of RTXC's `send-exchange` primitive (KS
opcode, table at `0x22003620`) and put a breakpoint on it conditioned on index 6 —
rare enough not to perturb— to read the sender's LR for the ~52 signals that DO
occur before the hang, and see why #53 stops.

**Hygiene:** `IRQDBG` instrumentation (counters in `hw/dma/pl080.c` and
`hw/arm/ipod_classic/ipod_classic_timer.c`) **reverted**; tree byte-identical to the baseline;
it rebuilds and links cleanly. Zero PCM injected, zero firmware patches. `pctrace` intact.
Rockbox/navtest untouched (the paths measured are only retailOS reads).

### 11.13 The CONSUMER side re-characterized: the engine NEVER receives a render trigger; the wall is a symmetric circular bootstrap of the first cycle

Session with non-invasive register reading now operational (`sendex=`/`regpc=`,
§ commit `0a0ea0365`). Clean play of `~/6g/Emulti.img`, `AUDIOPACE=1`, Shuffle
Songs. Everything **VERIFIED** by non-invasive gdb read/breakpoint or by
`pctrace pc=` counters (zero perturbation measured: `resets=0`). New recipes in `~/dbg/`
(outside the repo): `runt2a.sh`+`t2a_probe.py` (graph+unwound stack), `runcounts.sh`
(decode profile), `runmbox.sh` (sender attribution by LR/sp), `runtree.sh`
(decode tree via `range=`), `t2a_probe2/3.py`. Model tree **untouched**.

**A) The audio ENGINE (t25, consumer) NEVER receives a render trigger — VERIFIED.**
t25's wait is a `recv` on mailbox 0x52 WITH a 3 s watchdog: stack
`0808dfa8→22002f84→22006824→[recv 0x22006814]→0x080b3e78→0x805c518`, timeout
`r1=0xbb8=3000ms`. Probe `sendex=0x22003f7c:0:0x52` (gates on reg r0==0x52 at the
TB header of the *select-mailbox* primitive `0x22003f7c`, hardcoded with the
mailbox number in r0): **22 matches during play, ALL of them t25's own `recv`**
(`lr=0x805c578`, `r1=0xbb8`) firing **every ~3.00 s exactly** (timeout) and
waiting again. **ZERO sends.** Nobody ever sends a render command to the engine.
After 3 s the engine **tears down** the codec (`type[+0x58]→0xff`): that is the
"post-teardown mirage" of §11.12-A and the source of the non-determinism between
sessions (depending on whether you attach before or after the 3 s watchdog you see the clean
state or the torn-down one).

**B) The engine's ONLY run (setup) tries to render and comes out empty — VERIFIED.**
The engine `0x0808170c` (case 1, gate=1, type=1 from §11.12-A) calls the mixer
`0x0809a74c` **8 times** (`0x8081a9c..0x8081cb0`), but **all the `copy`s SKIP**
(`[r7+5]=0`, PCM not ready) → `copy32 0x08016eb4 = 0`. Its first render runs
**too early** (the decoder has not yet produced frame 0) and **there is no second
render** (no trigger). `copy32=0` is invariant across all runs.

**C) The DECODER (t2a) is a message actor; its data decode NEVER fires — VERIFIED.**
`pctrace` of the cluster: the *dispatcher* `0x08123dcc` switches on `[msg+4]`;
the **type-12** handler `0x8123970` decodes (`bl 0x8123bfc`) **only if**
`[msg+8]==obj+84` (the decode port) **and** `[[obj+52]+96]!=0`. Counters over a
full play: **`h12 (0x8123970) = 0`** (the "output buffer available /
render-pull" trigger NEVER arrives), `dstep (0x8123284) = 1`, `dwork (0x8123bfc) = 1`. And the
`range=` of that single decode run **does NOT touch AAC math**: the blocks
`0x08324xxx/0x08325xxx` that looked like "codec" are **I2C0 (`0x3C600000`)** talking to
the **CS42L55** chip (`0x08325634`: spin on `[I2C0+0x10]`, `[I2C0+0x20]|=0x3f00`),
NOT IMDCT/huffman. **Correction:** the AAC SW decode **never gets to run** (H1 of
§11.9 stands); what runs once is the codec chip config over I2C, which
**completes** (same path Rockbox uses). The decode gate `[[obj+52]+96]` was
measured **=1** in a snapshot (input ready) — the decoder is ready but without a trigger.

**D) Consolidated KS/messaging map (VERIFIED by disassembly).** Opcode table
of the KS dispatcher `@0x2200064c`: **op1**=WAIT-sema, **op2**=SIGNAL-tag,
**op4**=RECV-exch, **op5**=SEND-exch, **op14**=RECV-mbox. *Select-mailbox*
`0x22003f7c` (r0=mailbox number, it is a TB header → `sendex`-able). Veneers of the IRAM
render loop: `0x22003840→0x0808170c` (engine), `0x22003868→0x080b3e78` (its recv with
timeout). Decoder dispatcher `0x08123dcc` (type-12=`0x8123970` decode-gated,
type-19=`0x8123cfc` switch on codes `0x52800000+n`). The decoder's family of
mailbox-9 verbs (`0x8025ac4` send, `0x8025a04` recv, …) is hardcoded to mbox 9.

**E) THE WALL, re-characterized (measured): a SYMMETRIC CIRCULAR DEADLOCK of the first cycle.**
The two halves block each other:
- The engine renders real PCM **⟸** render trigger (mbox 0x52) **⟸** the output
  consumes real PCM (returns credit) **⟸** the engine renders real PCM.
- The decoder decodes frame 0 **⟸** render-pull/output-buffer (type-12) **⟸**
  the engine renders **⟸** the decoder decodes frame 0.

Neither side is **primed** to start without the other. The driver (StreamCache
t20) sends data to the decoder (mbox 9) and commands to the engine (mbox 0x52) **as
credit flows**; t20 is blocked on exchange 6 waiting for credit from its
upstream (t0e←t16), and that credit only comes back when the output consumes real PCM.
**The decoder is BLOCKED, not slow** — infinite time does not start it. `copy32=0`
end to end.

**F) Why NO model fix was found (re-confirmed, honest).** At the hang
**no task polls hardware** (wait graph: all on mbox/exchange). The
ONLY HW touches on the setup path — **SM1 position** (advances, correct) and the
**CS42L55 I2C config** (complete, identical to the path Rockbox uses and plays) —
are **correct**. Zero unmodeled peripherals (`-d unimp` = only 43 GPIO writes,
§11.11-B); IRQ16 (~50/s) and the 1 kHz tick fire continuously (§11.12-D). Therefore
**no value of a modeled peripheral nor HW event in the deadlock loop
can unblock it**. A HW value could only matter if it made **an UPSTREAM branch in the
priming diverge**; it was searched for (SM1, I2S, DMA, I2C, timers) and that value **was not
located** in 6 sessions — the priming that breaks the cycle lives inside
**MeCCA's asynchronous buffer/message protocol**, not in a register.

**HONEST VERDICT (this session): it does NOT play.** The wall is a **circular bootstrap
deadlock of the MeCCA pipeline**, confirmed by measurement to be free of any missing or wrong HW
value/event *within the loop*. This session did **not** find a localized
model fix (consistent with the 5 previous ones), but it **refined the mechanism**: the
first domino is the **CONSUMER** (the engine never fires; 3 s watchdog →
teardown), not just the decoder; and it is **symmetrically circular** (neither half
primed). To close it would require: **(a)** locating MeCCA's exact **priming** step
— who should issue the first render-pull (type-12) to the decoder and/or the first
render command (mbox 0x52) to the engine **without** waiting for credit — probably against
a **HW reference** or the state the real **bootloader** leaves (our
`-kernel` skips it); **(b)** if such priming turned out to depend on a value that the
firmware reads wrongly at boot, correct **that** value (a legitimate model nudge,
not yet found); **(c)** in the absence of (a)/(b), the wall is **intrinsic** to a
faithful emulation of this concurrent bootstrap and could only be opened by an **HLE of the
decoder/engine** or an **instruction timing** model (icount) that reorders
scheduling — with the auditor's caveat that determinism by itself is not enough.

**Hygiene:** zero changes to model sources; tree byte-identical except for this
wiki entry. `pctrace` intact (already in the tree since `0a0ea0365`).
Session instrumentation in the WSL's `~/dbg/`. Rockbox/navtest/5G untouched.

### 11.14 Priming, closed by measurement: the INPUT side is healthy; the wall is ONLY the OUTPUT trigger; and the SM1+0xa98 bit12 candidate REFUTED

Closing session for the priming node. The experiment that §11.12 left
proposed and that nobody had run (emitter attribution for **exchange 6**) was executed,
the engine's render loop was disassembled at the instruction level, and a new HW-value
candidate was **tested and ruled out by measurement**. Everything **VERIFIED** with
non-invasive `pctrace` (`sendex=`/`pc=`, disk `Emulti.img`, `AUDIOPACE=1`, Shuffle
Songs). New recipes in `~/dbg/`: `runex6.sh`, `runsm1v3.sh`. Tree byte-identical.

**A) Emitter attribution for exchange 6 (NEW — closes the unknown from §11.12/§11.13-E).**
Probe `sendex=0x22004138:3:0x6` (the KS *post* `0x22004138` carries `r3`=exchange
index, `r2`=opcode; VERIFIED by disassembling the decoder verb `0x8025ac4`:
`mov r3,#6; mov r2,#5; bl 0x8015740→0x22004138`). Result of one play:
**57 posts to exchange 6, ALL `r2=5` (op5 SEND-exch), ALL from the decoder's verb
family** `0x08025xxx` (by LR: `0x8025b18`×27, `0x8025a5c`×25,
`0x80258a4`×3, `0x8025960`×1, `0x8025904`×1). **Not a single post** comes from an ISR,
from an *upstream* disk task, or from the output consumption path.

⇒ **CORRECTS §11.13-E:** there is no "upstream credit (t0e←t16)" flowing through
exchange 6. Exchange 6 is ONLY the **decoder → StreamCache** channel (input
requests; StreamCache replies via mbox 9). The decoder↔StreamCache INPUT loop
is **autonomous and HEALTHY** (57 exchanges, matches
`M09.seq≈52`). It fills up to its cap and stops, waiting for the consumer
(the engine) to pull — which never happens. **The wall is NOT on the input side.**

**B) The wall is categorically the OUTPUT TRIGGER — re-confirmed.** In the same
play: `sendex=0x22003f7c:0:0x52` = **25 matches, all of them the engine's own `recv`**
(zero sends to mbox 0x52); `copy32 0x08016eb4 = 0`; `engine=1, mixer=1, copyfn=1,
disp=6, h12=0`. Identical to §11.13. **Nobody ever sends the render command (mbox
0x52) to the engine**, and the input side shows that **it is not for lack of data**.

**C) The engine's render loop, at the instruction level (VERIFIED by disassembly).**
Task 0x25 (audio driver, IRAM) calls the engine **once** during setup
(`0x22006300: bl 0x22003840→0x0808170c`) and then **waits**:
`0x22006814: recv(obj=0x220106d0, timeout=0xbb8=3000ms)` via `0x22003868→0x080b3e78`;
on timeout it returns −1 and the driver does **teardown**. The engine's mailbox is a field
of the struct `0x220106d0`; **nobody posts to it** (§B). The render trigger is a
**task-to-task message** (StreamCache t20 → engine t25), whose send branch in
StreamCache **is never reached** (§11.13-B: 0 `select`s of mbox 0x52 for sending).

**D) NEW HW-value candidate — TESTED and REFUTED by measurement.** The
setup body `0x0808af58` reads **SM1+0xa98** (getter `0x080a7638`, `r1=0` → `[obj+0xa98]`,
`obj` = `handleTable[0x08ade488]` = SM1 base) and **gates on bit 12 (0x1000)** a
block that writes **SM1+0x804=1** (`0x80a35a0`) and **SM1+0x858=1** (`0x809a72c`,
"enable" strobes of the audio engine). Our SM1 model only sets **bit 2**
(clock-ready) in +0xa98 → bit 12 always 0 → the enable block is skipped. A plausible
candidate that previous sessions (focused on bit 2) had not touched.
**Measurement (flag `SM1_ENGINE=1` forcing bit 12, reverted):**
`copy32 = 0` (no change) **and** `setupbody 0x0808af58 hits = 0` — the bit-12 reader
**is not on the play path** (it is boot-time init of the audio subsystem, not
per-playback priming); moreover, the two gated writes are **inert latches** in the
model (nothing in the render depends on SM1+0x804/+0x858). **It is not the fix.** Reverted;
tree byte-identical (`git diff` empty, rebuilds and links cleanly).

**VERDICT (this session): NO sound — STRUCTURAL wall, closed by measurement.** The
priming of the first MeCCA cycle **does not depend on any HW value/register/event nor
on state left by the bootloader**: it is an **inter-task message** (the mbox 0x52
render command from StreamCache to the engine, and symmetrically the type-12 render-pull to the
decoder) whose emitting branch is gated by the **circular** condition "output consumed
real PCM / there is a decoded frame" — and `copy32=0` keeps it false end to end.
The exchange 6 attribution (§A) eliminates the last "external credit" hypothesis:
the input is healthy; **what is missing is exclusively the cold output trigger**, and that
trigger reads nothing correctable. Consistent with the 6 previous sessions, and refined.

**The real route (documented, not a model fix):** an **HLE of the priming** that emits
**both** triggers cold —the first mbox 0x52 to the engine (`recv obj 0x220106d0`) **and** the
first type-12 to the decoder (`0x08123970`, gate `[[obj+52]+96]!=0` already =1)— to break
the symmetry; the PCM would then be produced by the guest's decoder. It is a shim of the
MeCCA protocol, **not** a HW value: that is why it does not fit as a "model fix". An
`icount` model (QEMU 9.x) could additionally explore the regime-A/B race (§11.11)
in case the real concurrent order differs, but determinism by itself does not guarantee
that the missing emitter appears.

**Hygiene:** SM1 bit12 experiment **reverted**; tree byte-identical to the baseline
(`git diff` empty) except for this wiki entry; rebuilds cleanly. `pctrace` intact.
Session recipes in `~/dbg/` (`runex6.sh`, `runsm1v3.sh`), outside the repo.
Rockbox/navtest/5G untouched (the measured paths are only retailOS reads).

### 11.15 The type-12 HLE shim: IMPLEMENTED and FIRED, but type-12 is NOT the wall (measured correction to §11.13/§11.14)

**Implementation** session for the HLE shim that the previous seven left
specified. The **type-12 render-pull** trigger to the decoder was built and
its effect measured with RELIABLE navigation (the missing piece). VA = off +
`0x07FF51B8`. Everything **VERIFIED** by `pctrace` (`pc=`/`sendex=` counters +
`memw`) and by the shim's own trace. Disk `Emulti.img`, `AUDIOPACE=1`.

**What was implemented (stays in the tree, GATED, default build byte-identical).**
`hw/arm/ipod_classic/ipod_classic_hle_audio.c` (+ `.h`), wired into the machine and
triggered by the I2S *run* bit. With `HLE_AUDIO_KICK` unset **it installs
nothing** (init returns before touching state; the I2S hook is a no-op). With
`HLE_AUDIO_KICK=1`: a virtual-clock timer watches the decoder's render-pull gate
(`decoder_obj` = heap `0x0ba9a8a4`, VALIDATED by its vtable `[obj+0]==
0x088d1948`; gate `[[obj+0x34]+0x60]`); when the gate is !=0 (primed) and the CPU
is in the **RTXC idle loop** (WFI, `0x22003b0c–0x22003b30`, so as not to clobber the
context of any live task), it redirects the idle context to call the
**guest dispatcher `0x08123dcc`(decoder_obj, msg)** with a 12-byte message
(`[msg+4]=12`, `[msg+8]=obj+0x54`) written below the idle stack, with LR returning
to idle. Zero PCM injected; the message is exactly the one the handler
`0x08123970` requires. (`HLE_AUDIO_KICK_FORCE=1` = debug knob that fires even
with gate=0, to exercise delivery; `HLE_AUDIO_KICK_TRACE=1` traces.)

**A) THE DECODER DOES GET PRIMED — and RELIABLE NAVIGATION was needed (corrects §11.11–14).**
Previous sessions measured with a flaky SELECT (moreover, the `grep StartPlayback`
matches the *watch* line that `pctrace` prints at startup → false positive → a
single SELECT press). That left them in **regime B (under-progressed)**:
`disp=2`, gate=0, the decoder does not even get configured. With RELIABLE SELECT (`iremote press
ret -u -w q,e --region right`, retries until the screen changes) play
**really starts and the decoder is configured ENTIRELY**: MEASURED over several runs
`disp=7`, `ex6≈61–134` (input loaded), and the render-pull gate
`[[obj+0x34]+0x60]` measures **`0x4` (!=0 = primed)**. Decoder priming **exists
and is reachable** — the §11.13 premise "the decoder is ready" is confirmed.

**B) THE FIRMWARE EMITS TYPE-12 ITSELF (refutes "nobody emits it" from §11.13/§11.14).**
As soon as play really progresses, MEASURED without the shim (HLE off, reliable nav):
`h12 (0x08123970) = 1`, `gateconsume (0x081239bc, the str that consumes the gate,
only reachable on the decode branch) = 1`, `dwork (0x08123bfc) = 3`. That is: the
retailOS **DOES dispatch the type-12 render-pull** and **enters the decode branch**.
The `h12=0` of the 7 sessions was an **artifact of regime B** (under
flaky navigation the pipeline does not get there). The shim, with gate=`0x4`, also
fires (trace "emitted type-12 ... from idle pc=0x22003b20"); but its type-12 is
**redundant** with the one the firmware already emits.

**C) THE REAL WALL: type-12 is INSUFFICIENT — no PCM comes out even though it decodes
(decisive finding).** With decode triggered (by the firmware or by the shim,
`h12=1`, `gateconsume=1`, `dwork=3`), MEASURED with `memw=0x08888000-0x0888c000` (the
PCM source the copy reads, §11.8): **8192 stores, ALL from the memset
(`0x22000198`/`0x220001a0`), ZERO from the decoder.** `copy32 (0x08016eb4) = 0`
invariant; `mixer (0x0809a74c) = 1` (only the bring-up render, which skips via
`[r7+5]=0`). **Type-12 does NOT produce PCM.** `dwork 0x08123bfc` does NOT decode
inline: it **posts** an async message (`0x8105c14` type `0x3a98` → `0x8105524`) to the
decode port `obj+0x54`; the 3 posts accumulate but **the downstream AAC decode
task never produces PCM**. The wall is **one level below
type-12**, not in type-12.

**HONEST VERDICT (this session): the retailOS does NOT play sound.** The captured
`hle_out.wav` is still the **UI click** (L==R 100%, ~20k non-zero samples in
bursts — not music). What was contributed, all MEASURED:
1. **Type-12 HLE shim implemented, gated, safe** (does not break the default
   build; observed `resets` = the retailOS's own watchdog reacting to the
   stalled playback, not the shim) and with its **delivery mechanism validated**
   (fires the dispatcher from idle without corrupting live tasks).
2. **The decoder DOES get primed** (gate `0x4`) with reliable navigation — priming was not
   the wall.
3. **Type-12 is NOT the wall**: the firmware emits it on its own (`h12=1`), decode
   enters its branch (`gateconsume=1`, `dwork=3`) and **still `copy32=0` and the
   mixer source only receives the memset**. Corrects the central hypothesis of the 7
   sessions ("type-12 is missing"): what was missing was **reliable navigation** to see it, and once
   seen, type-12 turns out to be **present but insufficient**.

**The exact wall that remains (for the next session):** the **async AAC decode
task** —receiver of the posts from `0x08123bfc` to port `obj+0x54`— **does not run
or does not write PCM**. It is a third level of rendezvous, downstream of the type-12 that
the 7 sessions believed to be the last. Routes: (a) locate that task (who receives on
`obj+0x54`, its TCB/state in the deadlock) and see what it blocks on; (b) the shim already
has the generic delivery mechanism (redirection from idle to a guest
function) — reuse it to prime that task, or the engine's render (mbox 0x52,
whose payload is still UNKNOWN, §mensaje). The guest's PCM does not appear because the
real producer (AAC decode) is stopped, not because type-12 is missing.

**Hygiene:** `HLE_AUDIO_KICK` off by default → build byte-identical to the baseline;
Rockbox/navtest/5G intact (the shim is inert without the env). Session recipes in
`~/dbg/` (`runhle_*.sh`, `cap_hle.py`), outside the repo.

### 11.16 `obj+0x54` is a TIMER (not an AAC task); the hunt for the "async task" was a phantom; the wall is the cold OUTPUT trigger — VERDICT closed by LIVE examination of the real decoder

Resolution session. Workflow goal: "hunt down the downstream AAC task that
receives the posts to `obj+0x54` and produces no PCM" (§11.15). It was **measured again with
reliable navigation** (4 decode-confirmed captures) and the `dwork` chain was
**disassembled in full**. Result: the workflow's premise is a **misidentification**.
Everything **VERIFIED** by direct disassembly (`~/6g/osos.dis`, VA = off +
`0x07FF51B8`) or non-invasive gdb reads of a play (disk `Emulti.img`,
`AUDIOPACE=1`, Shuffle Songs, reliable SELECT). New recipes in `~/dbg/`:
`runmeas.sh`, `runtrace.sh`, `runt2aexam.sh`, `t2a_examine.py`. Tree
byte-identical to the baseline (reads only; zero source changes).

**A) `obj+0x54` is a 15 s TIMER, not a decode channel — VERIFIED by disassembly (confirms Report-1; REFUTES §11.15-C and Report-2's "drainer actor" interpretation).**
`dwork 0x08123bfc` does: `r1=0x3a98; r0=obj+0x54; bl 0x08105c14; b 0x08105524`.
Disassembly of the two targets:
- `0x08105c14 → 0x08051b94`: `str r5,[r4,#4]` ⇒ **`port[+4] = 0x3a98`** (the
  INTERVAL, not a "message type").
- `0x08105524`: sets `port[+0x20]="run "` (`0x72756e20`) → `0x080578fc`, which:
  `NOW = 0x08033a40()`; `deadline = NOW + interval·125·8 = NOW + interval·1000`;
  `str deadline,[port+8]`; **inserts `port` into a linked list SORTED BY
  DEADLINE** (head in the control block `0x08911488`; insertion loop
  `0x0805795c` comparing deadlines with `0x0826480c`); `port[+0x1c]=1` (queued);
  and **signals the wheel's semaphore** (`0x0806a8ac`, sema `0x08970050`).
This is a **textbook timer-wheel insertion**. `0x3a98 = 15000`
= interval in ms = **15 s**. `dwork` **ARMS a 15 s watchdog**; it does not post
any decode message. The "task that receives on `obj+0x54`" is **t04**, the
**timer-wheel service** (entry `0x080833d0`, resumePC `0x22004394` op1b-WAITsema,
parked on sema `0x08970050`). **The async AAC task this workflow was created
to hunt DOES NOT EXIST** — `obj+0x54`/`0x3a98` is a false trail. (§11.15-C and
Report-2 misread the watchdog arming as an async post; Report-1
got it right.)

**B) The h12=0 (Report-1) vs h12=1 (§11.15) contradiction = the regime-A/B race PLUS the 15 s watchdog — RESOLVED by measurement.**
With reliable navigation, MEASURED (`runmeas.sh`, `pctrace pc=`/`sendex=`):
- **Run A (regime A):** `disp=7, h12=1, gateconsume=1, dwork=3, ex6=57`,
  `copy32=0`. The **time trace** reveals the decisive part: setup at `+74.7 s` (disp
  #1-2, dwork #1-2 arm watchdogs) and a first type-12 render-pull with **gate=0**
  (`h12else`), then a **STALL of EXACTLY 15.13 s**, and at `+89.87 s` **the watchdog
  expires** → burst (disp #3-6, `h12 #1` with the gate now set, `gateconsume`, dwork
  #3) → the engine runs its bring-up (`engine=1, mixer=1`) at `+90.2 s` → **`copy32=0`**.
- **Run B (regime B):** `disp=6, h12=0, dwork=2, ex6=87`, `copy32=0`, and the timer
  tag changes to `"stop"` (`0x73746f70`, teardown).
⇒ **`h12=1` IS reached (§11.15 was right), but ONLY after the 15 s
stall+watchdog**; Report-1's captures (`<15 s`, as Report-1 itself noted) measured
BEFORE expiry → `h12=0`. And **the result is INVARIANT in both
regimes: `copy32=0`, zero PCM.** The race does not change the outcome. The 15 s
watchdog (and its teardown) ARE the retailOS's own **stall-recovery** mechanism:
its existence and firing **prove that the pipeline does not start
cold** — the immediate first-frame push that the real HW provides is
absent, and not even the watchdog closes the loop.

**C) FIRST LIVE examination of the REAL AAC decoder (t2a) and the consumer/driver — PURE rendezvous deadlock, confirmed in the task itself (`t2a_examine.py`).**
Pre-watchdog snapshot (regime A, gate primed): `decoder_obj=0x0ba9a8a4`
(vtable `0x088d1948`), `ctx=[obj+0x34]=0x0ba90c50`, **gate `[ctx+0x60]=0x4`
(primed)**. State of the three tasks in the loop (all with worker entry `0x080833d0`):
- **t2a (AAC DECODER):** `st=08`, resumePC `0x22004028` = **op14-RECVmbox on
  mailbox 9 (INPUT)**. Return chain `0x08025b04 → 0x081232a4 (dstep) →
  0x08123da8 (type-19) → 0x08123e20`: it is **inside `dstep 0x08123284`**,
  having requested input via exchange 6, **blocked waiting for the chunk on
  mailbox 9**. It NEVER reaches per-frame decode.
- **t25 (audio ENGINE):** `st=08`, op14-RECVmbox on **mailbox 0x52 (render
  command)**, chain `0x0805c578 → 0x080b3e80 (recv with 3 s timeout) → 0x22006824`.
  **Nobody ever sends it the render command.**
- **t20 (StreamCache):** `st=40`, op4-RECVexch on **exchange 6** (serving the
  decoder's input requests), chain `0x08171500 → 0x081f39a4 → 0x081d0a2c`.
ALL THREE in inter-task rendezvous; **NONE polling hardware**; gate primed;
`copy32=0`. **Post-watchdog** snapshot: t2a and t25 go to `st=00` (**torn down**)
and mailboxes 9/0x52 to zero — the watchdog teardown. It is **exactly** the
deadlock of §11.12/§11.13, now confirmed IN the decoder task itself.

**D) Decode DOES do codec work, but NEVER writes PCM to its output buffer — VERIFIED (`runtrace.sh` range + §11.9).**
`range=0x08080000-0x080b0000` triggered at setup + watchdog: **672 distinct codec
blocks** run (region `0x080axxxx`: engine bring-up + `ParseASC
0x080a3cd0`, config). But the copy source `0x08889098` (region
`0x08888000-0x0888c000`) still receives **only the memset** (§11.9/§11.15) and
`copy32=0`. The engine renders its bring-up and **its copy SKIPS** (`[descr+5]=0`,
"PCM not ready"). The per-frame decode-to-PCM (which would write `0x08889098` and set
the frame-ready flag) **never fires**, because it is gated behind the
render round-trip that is deadlocked.

**E) MODEL FIX determination: NONE — closed by measurement.**
The missing element is **the cold render command (mailbox 0x52) to the
t25 engine**. It is an **inter-task message**, not a HW event: the three waits of the loop
are rendezvous (mailbox 9 / mailbox 0x52 / exchange 6), none is a
peripheral poll (confirmed LIVE in §C; consistent with `-d unimp`=only 43 GPIO writes
§11.11-B, IRQ16+tick alive §11.12-D, output identical to Rockbox §11.13-C). Therefore
**no peripheral, timer, HW value or initial state can satisfy
that wait** — consistent with the 8 sessions. The regime-A/B race is
timing-sensitive but **the outcome is invariant** (`copy32=0` in A and B), so
a timing change does not unblock it either.

**F) The WAV OUTPUT in regime A confirms it independently — it is ONLY the
UI click, zero music (`runwav.sh`, `AUDIOWAV`).** 72.6 s capture with decode
confirmed (`h12=1` at `+15.17 s` after the watchdog): **`nonzero=0.25 %` (99.75 %
silence), `L==R=100 %` (fake mono, `rms(L-R)=0.0`), 13 IDENTICAL bursts of 624
samples with peak 11444** (one per SELECT press during navigation). Real music
would be `nonzero≈100 %`, `L==R≪100 %`, `rms(L-R)≫0`. There is no content with which
to correlate the AAC. (Hygiene: the file `~/6g/EVIDENCE_realaudio.wav` from a
previous session measures EXACTLY that signature —11444/624/L==R—, so it is **the click
mislabeled as "realaudio"**, not proof of audio. The reliable discriminator is
`copy32>0`/the decoder's `memw`, not the WAV amplitude.)

**VERDICT (measured, honest): the retailOS does NOT play sound. An INTRINSIC circular
bootstrap deadlock of the MeCCA pipeline, with no model fix.** The producer
(t2a) and the consumer (t25) block each other cold: the engine renders PCM
⟸ render command (mailbox 0x52) ⟸ the output consumed real PCM ⟸ the engine renders;
and the decoder produces frame 0 ⟸ render round-trip ⟸ the engine renders. Neither
half is primed. The input is healthy (ex6=57–87, t20 serving), the decoder
primed (gate=0x4), and still `copy32=0` end to end. The firmware's own 15 s
watchdog is the proof that the cold start does not close.

**Corrections to the record contributed by this session (all VERIFIED):**
1. `obj+0x54` = **15 s watchdog** (timer-wheel disassembly), not a
   decode channel → **the "async AAC task" of §11.15 is a phantom**; this
   workflow's front rested on a misidentification.
2. `h12=1` **is reachable** (§11.15 was right) but **only via the 15 s watchdog**;
   Report-1's `h12=0` was measuring before expiry. `copy32=0` invariant.
3. **First live examination of t2a** (the real decoder): blocked on input
   (mailbox 9), never decodes; the wall is the **OUTPUT trigger** (mailbox
   0x52 to the engine), not `obj+0x54`.

**The real route (not a model fix, documented):** a **cold priming HLE** that
emits the **first render command (mailbox 0x52) to the t25 engine** to start
the consumer —which will pull the frame (type-12 render-pull) and make the
GUEST's decoder produce the PCM—. It is MORE than the §11.15 type-12 shim (which is now
shown to be insufficient: h12 only consumes the gate and **arms the watchdog**, it does not
decode): the ENGINE SIDE has to be primed, and the binding of the output buffer
(descriptor `0x0bb12d54`, flag `[descr+5]`) must come from the guest's own
render. Alternative: `icount` (QEMU 9.x) to reorder scheduling and explore
whether the real concurrent order differs — with the auditor's caveat that
determinism by itself does not make the missing mailbox 0x52 emitter appear.

**Hygiene:** zero changes to model sources; `git diff` empty; build
byte-identical. Only non-invasive gdb reads + `pctrace` (already in the tree).
Rockbox/navtest/5G untouched. Session recipes in `~/dbg/` (`runmeas.sh`,
`runtrace.sh`, `runt2aexam.sh`, `t2a_examine.py`), outside the repo.

### 11.17 The render DOORBELL shim (`HLE_RENDER_KICK`): IMPLEMENTED and its MECHANISM VALIDATED, but the engine is a work-driven CONSUMER — ringing its doorbell does NOT produce render without decoded PCM (refutes the "prime mailbox 0x52" hypothesis)

An **implementation** session for the ENGINE-SIDE priming that §11.16 left
specified (and that the two closing investigations — mailbox 0x52 format + the
cascade to PCM — detailed). The **render doorbell** trigger to engine t25 was
built and its effect measured with RELIABLE navigation. VA = off +
`0x07FF51B8`; IRAM at `0x22xxxxxx`. All **VERIFIED** by `pctrace` (`pc=`/`sendex=`
counters + `memw`), the shim's own trace, and a non-invasive gdb snapshot. Disk
`Emulti.img`, `AUDIOPACE=1`, reliable SELECT (`iremote press ret -u -w e --region
right -r 15`).

**A framing correction that the investigation confirms (read first).** The
"mailbox 0x52" is NOT a KS mailbox with a payload: engine t25 blocks in
`recv()` on a **payload-LESS fast-semaphore** `0x220106d0` (the "doorbell").
VERIFIED: t25's recv wrapper `0x22006814` loads `r0=0x220106d0, r1=0xbb8` (3 s)
and calls `0x22003868`. The engobj is `0x220106dc` (= ring+12;
`[engobj+0]=vtable 0x22008980` set by the ctor `0x220068e4`). The "render order"
is: arm the gate `[engobj+71]=1` (the engine exits via `beq 0x8082020` if it's
0, `0x808171c` `ldrb r0,[engobj+71]`) + ring the doorbell (V() of the
fast-sema). The firmware's own sender is the IRAM stub `0x2200683c` (arms
`[engobj+71]=1`, clears `[+68]/[+70]/[+72]`, increments `0x22008c1c`, signals)
— but its precondition `[engobj+70]!=0` is **false when cold** (`bxeq lr`
@`0x2200684c`) and it has **0 callers**: that's why NOBODY rings the doorbell
when cold. (Confirms §11.16-C: t25 waiting for the output trigger.)

**A) What was implemented (GATED, default build byte-identical).**
`hw/arm/ipod_classic/ipod_classic_hle_audio.c` (+`.h`), wired into the machine
and triggered by the I2S *run* bit. With `HLE_RENDER_KICK` unset it **installs
nothing** (init returns before touching state; the I2S hook is a no-op). With
`HLE_RENDER_KICK=1`: a virtual-clock timer, when t25 is queued on the doorbell
(`[0x220106d0+4]!=0`, waiter present) **and** the CPU is in the RTXC idle loop
(`pc∈[0x22003b0c,0x22003b34)` or `halted` in WFI), redirects the idle context
to: (1) write `[0x220106dc+71]=1` (the render gate, like the firmware's own
sender), and (2) call **the guest's own stub `0x22006870`** (`r0=0x220106d0;
r1=0; b 0x22003670→V() 0x080a1858`), with LR returning to idle. Zero PCM
injected; the EXACT doorbell the HW would ring is rung, and the dequeue+wake are
done by the guest's own KS under its locks. It reuses the delivery-from-idle
mechanism validated in §11.15 (commit `dd068aa1e`), pointed at the ENGINE
instead of the decoder.

**B) THE MECHANISM WORKS — VERIFIED.** Shim trace: "rang render doorbell #N to
engine t25 (ring=0x220106d0 waiter=0x0bb1306c [engobj+71]=1) from idle
pc=0x22003b20". The doorbell is rung from idle (pc=`0x22003b20`, exactly the pc
from the §11.16 snapshot), **9,791–11,244 times** per run, waking t25's recv
(waiter `0x0bb1306c` = node on t25's stack, the same one from the
investigation). No live task is disturbed (`resets`=1–2 = the retailOS's own
15 s watchdog, identical to baseline). The live snapshot confirms with the shim
active: `[engobj+71]=01` (armed by the shim), t25 `state=40` parked on the
doorbell (RET `…0805c558 080b3e80 22006824 22002f84`), decoder primed
(`gate[ctx+0x60]= 0x4`).

**C) DECISIVE FINDING: the doorbell does NOT produce render — the engine is a
WORK-driven CONSUMER, not doorbell-driven.** With t25's path after the wake
instrumented (MEASURED, run v2):
```
recv    @0x22006814  hits=11244     <- t25 wakes from the doorbell 11244 times
recvret @0x22006824  hits=11243
dpump   @0x22006118  hits=1         <- the data-pump (render) runs ONCE (bring-up)
dpeng   @0x22006300  hits=1  dpbail=0  dpalt=0
engine  @0x0808170c  hits=1     mixer @0x0809a74c  hits=1     copy32 @0x08016eb4  hits=0
```
t25's loop is `0x22002f5c`: `bl 0x22005e48` (@`0x22002f80`, ret `0x22002f84`)
**polls the SOURCE** — `0x22005e48` is a virtual dispatch `ldr r0,[r0]; ldr
r1,[r0]; ldr r1,[r1,#28]; bx r1` (calls method `+28` of the source object). If
the source has no data, it returns 0 and t25 **goes back to the recv**
(`0x22002f88 beq`). The doorbell wakes the consumer, but **it can't manufacture
work that doesn't exist**: the source (decoded PCM) is empty, so t25 does
wake→poll→0→re-park, 11,244 times, **without rendering a single frame more
than baseline**. `copy32=0` invariant; `memw=0x08888000-0x0888c000` = **8192
stores, ALL memset** (`0x22000198`/`0x220001a0`), ZERO from the decoder.
Identical to §11.16.

**D) The engobj is SEMI-configured — VERIFIED (snapshot).** `[engobj+88]=0xff`
(the render TYPE is still at the init value from the ctor `0x220068e4`; the
`switch` of the data-pump `0x220062a0` and of the engine operate on it — it was
never configured to a real type). `[engobj+70]=0` (the cold-start gate of the
firmware's own sender, never satisfied). `[engobj+68]=0`. The engine never
completed its render configuration; the doorbell alone doesn't complete it.

**E) THE CORRECTED CAUSAL MODEL (refutes the hypothesis of the task and of 8
sessions).** The chain the work order assumed —"the engine renders → sends the
type-12 to the decoder → the decoder produces PCM"— is **FALSE and now REFUTED
by measurement**: render is the **CONSUMER** side (`engine 0x0808170c → mixer
0x0809a74c → copy 0x08016eb4 → ping-pong`), it does NOT send the type-12 to the
decoder (see the "cascade" investigation); the type-12 to the decoder goes by a
separate route. Priming the engine's doorbell **starts the consumer, but the
consumer has nothing to consume** because the PRODUCER (decoder t2a) is blocked
on its INPUT (mailbox 9, RECVmbox inside `dstep 0x08123284`) and never produces
PCM (§11.16-C). It is the **circular bootstrap deadlock** of §11.16, now
confirmed DYNAMICALLY from the engine side: waking the consumer 11,244 times
doesn't generate a single extra render.

**VERDICT (measured, honest): the retailOS does NOT make sound.** The render
doorbell (mailbox 0x52) is **necessary but INSUFFICIENT**. How far the kick got,
with evidence:
1. **Did the engine render?** NO more than baseline: `engine=1` (only the
   watchdog bring-up), despite 11,244 doorbell rings. The engine wakes but its
   loop polls the source, finds it empty and re-parks.
2. **Did the decoder decode?** NO: `h12=1` only after the watchdog (as in
   baseline), `copy32=0`, `memw`=memset only. t2a is still blocked on input
   (mailbox 9).
3. **Did `copy32` rise above 0?** NO: `copy32=0` invariant (A and B). The
   captured WAV is ONLY the UI click (`nonzero=0.26 %`, `L==R` 100 %, peak
   11444) — not music, nothing to correlate with the AAC.

**What is MISSING (the real wall, one level below the doorbell):** for the
PRODUCER (decoder t2a) to consume its input (mailbox 9) and decode frame 0,
putting PCM into the copy's source (`0x08888xxx`). Only then will t25's loop
(already woken by the doorbell) find work and render. The engine's doorbell is
the second link; the first (decoder input) is still absent. Consistent with the
"intrinsic circular deadlock, no model fix" of §11.16 (the three waits are
inter-task rendezvous, none is an HW poll).

**Decision on the shim: REVERTED (silent), finding kept.** The shim is clean,
gated and default-inert (default build byte-identical; navtest 6G and Rockbox
unchanged with the env unset), and its **delivery mechanism is validated** —
but **it doesn't produce audio** (the doorbell wakes the consumer with no
work). Just like the type-12 shim of §11.15 (reverted in `6e9f4a9c7`), the
source is reverted and ONLY this measured documentation is kept. Reproducible:
it is `dd068aa1e` re-pointed at the doorbell (`pc=0x22006870`, preceded by
`[0x220106dc+71]=1`), gated by `HLE_RENDER_KICK`.

**New addresses VERIFIED this session (disasm + measurement):**
- t25 render loop `0x22002f5c`; work poll `bl 0x22005e48` @`0x22002f80` (ret
  `0x22002f84`); `0x22005e48` = virtual dispatch `bx [[[source]]+28]` (polls
  the source, returns 0 if empty).
- doorbell recv `0x22006814` = `recv(0x220106d0, 3000ms)` via `0x22003868`.
- data-pump `0x22006118`; call to the engine `bl 0x22003840`(→`0x0808170c`)
  @`0x22006300`; clears the gate `strb #0,[engobj+71]` @`0x22006308`.
- engine entry gate: `0x8081718 bl 0x8015860` (engobj getter) →
  `0x808171c ldrb r0,[engobj+71]` → `0x8081730 beq 0x8082020` (exits if 0).
- doorbell stub `0x22006870` (`r0=0x220106d0; r1=0; b 0x22003670`→V()
  `0x080a1858`, a veneer that doesn't touch LR); the firmware's own senders
  `0x220060e0` / `0x2200683c` (0 callers when cold).
- engobj ctor `0x220068e4`: `[engobj+0]=vtable 0x22008980`, `[engobj+88]=0xff`.

**Hygiene:** `HLE_RENDER_KICK` off by default → byte-identical build; shim
REVERTED after measuring (only this doc is committed). Rockbox/navtest 6G/5G
intact (the shim was inert without the env; the final tree is baseline).
Session recipes in `~/dbg/` (`run_rk.sh`, `run_rk2.sh`, `run_rksnap.sh`,
`rk_probe.py`, `rk_probe2.py`), outside the repo.

### 11.18 The DUAL KICK (`HLE_DUAL_KICK`): the GUEST's decode DOES run for the first time, but producer and consumer are UNLINKED when cold — symmetric deadlock confirmed at the instruction level, no kick fix

An **implementation** session for the dual kick that the two closing
investigations (decoder priming + order/timing) specified: prime the DECODER
(so it produces frame 0) **and** the ENGINE (so it consumes it), at the same
time / decoder-first, to break the symmetric deadlock. It was built, the
**decoder delivery point was corrected** at the instruction level, and it was
**measured** with RELIABLE navigation. VA = off + `0x07FF51B8`; IRAM at
`0x22xxxxxx`. All **VERIFIED** by direct disassembly
(`~/6g/osos.dis`/`iram.dis`), a non-invasive gdb snapshot of a reliably
navigated play, and `pctrace` counters (`pc=`/`memw=`/`memwpc=`). Disk
`Emulti.img`, `AUDIOPACE=1`, reliable SELECT. New recipes in `~/dbg/`
(`run_dk.sh`, `run_dksnap.sh`, `run_dk_snap2.sh`, `dk_probe.py`,
`dk_probe2.py`); shim in `~/dbg/hle_dual_kick_shim/`.

**VERIFIED correction of the decoder priming point (at the instruction
level).** The dispatcher `0x08123dcc` switches on `[msg+4]`: **12**→watchdog
`0x8123970` (§11.16, confirmed); **19**→`blx [[msg]+20](msg)` and then
`switch 0x8123cfc(obj, [retval+8])`; default→`0x80eaebc`. The
`switch 0x8123cfc` switches on `[[retval+8]+4]-0x52800000` (10 cases): **case
1** (`0x52800001`, `0x8123d64`) → `bl 0x81232d8` = **frame DECODE**; **case 7**
(`0x52800007`, via `0x81235d0`) → `bl dstep 0x08123284` → `bl 0x8025ac4`
(requests input via ex6 + **RECV mbox 9**, WHERE IT BLOCKS). The live decoder
is processing a case-7 (measured return chain
`0x08025b04→0x081232a4(dstep)→0x081236c8(case7)→0x08123da8(switch)→0x08123e20(disp)`).
The real DECODE hangs off case-1 `0x81232d8`: at `0x8123390` it does
`blx [[ctx]+104]`=`0x081c0a1c` (decode-method) → `0x081c1508` (frame-pull) →
`reader->vtable[16]`=`0x081e93d8` (pull) → loop over `0x081e9e70` (the per-block
codec math). Reachable only if `ctx=[obj+0x34]!=0`, `[obj+0x84]==0`,
`[obj+0x38]==0` — **all three hold live** (snapshot: `ctx=0x0ba90c50`,
`gate[ctx+0x60]=0x4`).

**The shim (GATED, default build byte-identical).** `HLE_DUAL_KICK=1`: a
virtual-clock timer, from the RTXC idle (`pc∈[0x22003b0c,0x22003b34)`, without
stepping on live tasks), **alternates decoder-first**: (1) DECODER = call the
guest's own case-1 `0x081232d8(r0=decoder_obj, r1=0)` — it decodes the frame
**and** publishes it (`ctx->vtable[100]=0x081c12ac` + message ops); `r1=0`
makes the post-decode `0x81235d0` see `r7==0` and SKIP the input dstep (which
would do RECV mbox 9), so it **doesn't block** — VERIFIED that
decode-method/frame-pull/pull/publish have no KS-wait; the `0x805c430` lock on
`ctx+80` is uncontended (t2a is parked in the input verb, it doesn't hold it).
(2) ENGINE = `[engobj+71]=1` + doorbell `0x22006870` (§11.17). The engine is
gated on the decoder already being constructed (never ring when cold during
boot). Zero PCM injected. Knobs: `_DEC_ONLY`, `_MOTOR_ONLY`, `_CFG` (forces
`+88=1/+70=1`), `_DEC_MAX`, `_TRACE`.

**A) POSITIVE FINDING: the guest's DECODE DOES RUN — first time in 11
sessions.** With the dual kick (decoder-first, case-1 decode+publish),
MEASURED: `decmeth 0x081c0a1c = 4192`, `fpull 0x081c1508 = 4192`,
`rpull 0x081e93d8 = 4193`, **`innerdec 0x081e9e70 = 1`**. That is: the kick
reaches and executes the **guest's AAC math** (the one §11.9-§11.16 measured as
"never runs": `range` only saw `ParseASC`=config). The decoder-first delivery
mechanism works and reaches the decode.

**B) BUT the decode runs only ONCE — the production enable gate is at 0.**
`innerdec=1` despite 4192 calls to case-1. `rpull 0x081e93d8` exits early
(`0x81e93f8 beq`) because its gate **`[[reader+0x30]+0x2b8]` = 0** (baseline
snapshot: `reader=0x0ba61f50`, `[reader+4]ring=ctx`,
`decode-gate[[+48]+0x2b8]=0x00000000`). That gate is the "you may produce a
frame" credit governed by **StreamCache**; at 0, the decoder can't produce
frame 1. It is starvation of an **output slot**, not of data.

**C) copy32 = 0: the published frame NEVER reaches the engine — producer and
consumer UNLINKED.** Even forcing the engine config (`_CFG`: `[engobj+88]=1`,
`[engobj+70]=1`), MEASURED: `copy32 0x08016eb4 = 0`; engine t25 wakes from the
doorbell, **polls its source `srcpoll 0x22005e48 = 19×` and ALWAYS finds it
EMPTY**; `dpump 0x22006118 = 1` (bring-up only), `dpgateclr 0x22006308 = 0`
(never closes a render cycle). The engine's copy descriptor
**`0x0bb12d54 = 5da35da3 5da35da3…` (UNINITIALIZED heap fill;
`[descr+5]=0x5d≠1`)** — the source↔copy binding that the render itself would
initialize does NOT exist when cold. And `memw 0x08888000-0x0888c000` =
**memset only** (`0x22000198`/`0x220001a0`), ZERO writes from the decoder to
the mixer's source. The frame the decode produced/published stayed in the
reader's internal buffer; **there's no wired path to the engine's (garbage)
source**. `CODEC_SETUP 0x22006574` ran (`hits=1`) but its render config is
absent at the stall (`[engobj+88]=0xff`, `+71=0`, `+70=0` = values from the
ctor `0x220068e4`; post-teardown mirage of §11.12-A/§11.13-A).

**D) The symmetric deadlock, now at the instruction level (VERIFIED).** Each
half is gated by the steady-state output of the OTHER:
- Producer: `decode` ⟸ gate `[[reader+0x30]+0x2b8]!=0` ⟸ StreamCache grants
  credit ⟸ output buffer freed ⟸ the engine renders (`copy32`) ⟸ PCM in the
  source ⟸ decode.
- Consumer: `copy32` ⟸ `[descr+5]==1` + descriptor `0x0bb12d54` initialized ⟸
  the engine's own render ⟸ source with PCM ⟸ the decode published ⟸ producer
  enabled.
The three waits in the loop are inter-task rendezvous (t2a RECV mbox 9 inside
the input verb; t25 RECV on the doorbell; t20 RECV ex6), **none is an HW poll**
(consistent with §11.12-D/§11.16). Priming ONE end from idle gives **one
transient step** (1 decode; 1 engine wake), never a self-sustaining cycle,
because the shared resources (production credit, initialized output
descriptor) are only produced by the running pipeline, not by a doorbell or a
decode call.

**VERDICT (measured, honest): the retailOS does NOT make sound with the dual
kick.** How far each side got, with evidence:
1. **Did the decoder decode?** YES — the guest's AAC math
   (`innerdec 0x081e9e70`) **executed for the first time** (all previous
   sessions: 0). But **only ONE frame**: the production gate
   `[[reader+0x30]+0x2b8]` is at 0 (StreamCache grants no credit).
2. **Did it write PCM into `0x08888xxx`?** NO — the mixer's source received
   only memset; the decode wrote into the reader's internal buffer and the
   publication stayed unlinked from the engine's (garbage) source.
3. **Did `copy32` rise above 0?** NO — invariant. The engine polls its empty
   source (19×) and doesn't render; its copy descriptor `0x0bb12d54` is
   uninitialized. Forcing `+88/+70` doesn't help (the problem is the source
   BINDING, not the type).
4. **Self-sustaining?** NO. WAV = UI click (`nonzero=0.22 %`, `L==R`, peak
   11444; §11.16-F), nothing to correlate with the AAC.

**This is the firm STRUCTURAL VERDICT:** the dual kick advances the state
**further than the 10 previous sessions** (the guest's decode runs), but the
**symmetric** bootstrap deadlock doesn't open: the priming gates of each half
(`[[reader+0x30]+0x2b8]` in the producer; `[descr+5]` + uninitialized
descriptor `0x0bb12d54` in the consumer) are fed by the steady state of the
OTHER half, and no kick (doorbell/decode-call/config) from idle manufactures
them. The only thing left would be a **FULL HLE of the data path**
decoder↔engine: synthesizing the frame flow and the buffer
credits/descriptors — which is **injecting PCM/faking state**, forbidden by the
rules (the PCM must be produced by the guest's decoder and flow through its own
protocol). There is no kick lever, no HW value, and no order/timing that closes
the loop.

**New addresses VERIFIED this session (disasm + live snapshot):**
- dispatcher `0x08123dcc` (type-12→watchdog `0x8123970`;
  type-19→`blx [[msg]+20]` + `switch 0x8123cfc`); `switch 0x8123cfc` switches
  on `[[retval+8]+4]-0x52800000`: **case 1 → `0x81232d8` (decode)**, case 7 →
  `0x81235d0` → dstep.
- case-1 frame decode `0x081232d8` (gate: `ctx=[obj+0x34]!=0`,
  `[obj+0x84]==0`, `[obj+0x38]==0`); decode-method
  `[[ctx]+104]=0x081c0a1c`; frame-pull `0x081c1508` (`reader=[ctx+0x48]`,
  pull=`reader->vtable[16]`); reader-pull `0x081e93d8` (gate
  `[[reader+0x30]+0x2b8]`, ring=`[reader+4]`); **inner-decode/AAC math
  `0x081e9e70`**; publish `ctx->vtable[100]=0x081c12ac` (non-blocking).
- input verb `0x8025ac4`: SELECT-mbox9 `0x8015730→0x22003f7c`, SEND-ex6
  `0x8015740→0x22004138`, **RECV-mbox9 `0x8015738→0x22004260`** (the block).
  The verb does NOT read the reply: the RECV is a pure handshake.
- live snapshot (stall, baseline): `decoder_obj=0x0ba9a8a4` vt=`0x088d1948`
  ctx=`0x0ba90c50` vt=`0x088daa4c`; reader vt=`0x088de828`;
  **`decode-gate[[reader+0x30] +0x2b8]=0`**; **engine descriptor
  `0x0bb12d54=0x5da35da3` (uninitialized)**; `engobj+88=0xff`.
- engine: `srcpoll 0x22005e48` (`bx [[[src]]+28]`), `dpump 0x22006118`,
  `dpgateclr 0x22006308`, `CODEC_SETUP 0x22006574` (runs `hits=1`, config torn
  down at the stall).

**Corrections to the record (VERIFIED):**
1. **REFINES "prime-decoder" (decoder-in report):** the decode is NOT
   unreachable — case-1 `0x81232d8` from idle **does execute the guest's AAC
   math** (`innerdec=1`). What's missing isn't the trigger but the
   **production credit** (`[[reader+0x30] +0x2b8]=0`) governed by
   StreamCache.
2. **REFINES "order/timing" (order report):** the engine does NOT only need
   the doorbell; its **source/descriptor `0x0bb12d54` is UNINITIALIZED** (not
   just `+88=0xff`), and forcing `+88=1/+70=1` doesn't pay off (`copy32=0`).
   The producer→consumer binding doesn't exist when cold; it isn't a field to
   force but a render binding that only the render creates.
3. **CODEC_SETUP `0x22006574` DOES run** (`hits=1`), refining the doubt in
   §11.17-D: its render config shows `0xff`/`0` at the stall because of
   **teardown**, not because of non-execution.

**Decision on the shim: REVERTED (silent), finding kept (rule §11.17).** The
shim is clean, gated and default-inert (default build byte-identical; navtest
6G/5G and Rockbox unchanged with the env unset), and its **dual-kick delivery
mechanism is validated** (decoder-first from idle, non-blocking
decode+publish) — but **it doesn't produce audio** (the symmetric loop doesn't
close). The source is reverted and ONLY this measured documentation is kept.
Reproducible: shim in `~/dbg/hle_dual_kick_shim/`, gated by `HLE_DUAL_KICK`.

**Hygiene:** `HLE_DUAL_KICK`/`HLE_RENDER_KICK` off by default → byte-identical
build; shim REVERTED after measuring (only this doc is committed).
Rockbox/navtest 6G/5G intact (the final tree is baseline; navtest 6G and 5G
actually run at close). Session recipes in `~/dbg/` (`run_dk.sh`,
`run_dksnap.sh`, `dk_probe.py`, `dk_probe2.py`) and shim in
`~/dbg/hle_dual_kick_shim/`, outside the repo.

### 11.19 The DATA PATH HLE (`HLE_AUDIO_DATAPATH`): the retailOS plays ITS real song through its own I2S — host-side decode, like the 5G's VideoCore

IMPLEMENTATION session of the route that §11.18 left specified. Since the MeCCA decode
pipeline is in the symmetric boot deadlock closed by measurement in
§11.12–11.18 (it does not self-prime under faithful emulation), the SAME song that the
guest reads from ITS disk is decoded **on the host** and the PCM is delivered to the buffer that the
guest's OWN DMA/I2S carries to the codec — exactly the HLE pattern of the
5G's VideoCore (`hw/arm/ipod_video/ipod_video_vc.c`). Gated by
`HLE_AUDIO_DATAPATH`; default build byte-identical (Rockbox and navtest intact).

**The shim (`hw/arm/ipod_classic/ipod_classic_hle_audio.c`, gated).**
1. **Parses the FAT of the guest's disk** (MBR → FAT32) and enumerates the audio
   files. It IDENTIFIES the track by the guest's OWN ATA reads: the DMA
   read that falls in the data range of a file (hook in `ipod_classic_ata.c`
   via `ide_get_sector`). That way the SAME song that the guest opened is decoded.
2. **Decodes that track on the host with ffmpeg** → 16-bit stereo 44100 PCM
   (the only host-side step; it reads the SAME raw image of the `-drive`; it runs in a separate
   thread so as not to freeze the vCPU).
3. **Feeds the PCM to channel 1 of DMAC0** (the one that drains the ping-pong into the I2S
   FIFO): in `i2s_drain`, before each paced burst (`pl080_pace_request`),
   it writes the next chunk to `ch->src`; the read cursor advances ONLY by
   what the burst consumed (`moved`=Δtx_samples), not by the polling ticks —
   that way the song follows the real clock (fill/commit).

**VERIFIED corrections to the previous investigation (important).**
- **The guest's song is NOT AAC.** The 6 M4A/AAC files that the INPUT investigation
  carved (raw offsets 8.5–55.7 MB) are **OUTSIDE the FAT partition** and
  are NOT what the guest plays. The real library lives in the FAT32 (starts
  at byte **92962816**), in `/iPod_Control/Music/Fxx/`: **4 MP3s** (48 kHz,
  `libgpodNNN.mp3`) **byte-identical** to each other = **a single song**
  ("Numb / Encore"). The guest outputs at 44100 (CLKDIV 272) → it is decoded at 44100.
- **Disk with 4096 B logical sectors (4Kn).** The MBR LBAs are in units of
  4096 (part1 LBA 22696 × 4096 = 92962816 = FAT boot sector); the scanner tries
  ×512 and ×4096 and validates the boot signature.
- **The guest DOES read the MP3 data when it really plays** (regime A), but
  its playback is NON-DETERMINISTIC (§11.11 A/B): sometimes it reads the file (→ ID by
  read), sometimes it gets stuck earlier (regime B). For robustness, a **fallback**
  decodes the ONLY song on the disk (when the library is unambiguous: the 4
  identical files) on detecting the start of playback — via the CLKCON
  edge (play's clock reconfig, which the boot does not do) or via a read of the
  FAT data area once past boot (I2S has been sounding for >~34 s, so it is the
  user opening a file, not the boot scan).

**Measurement (RELIABLE navigation, `AUDIOPACE=1`, `Emulti.img`).**
- ID + decode: e.g. `identified guest track N (file stream, disk offset ...)`;
  `decoded track N -> 36400496 PCM bytes (206.4 s @ 44100 stereo) -- feeding`.
- **The feed is CONTIGUOUS and at real-time rate** (drain trace VERIFIED): src alternates
  the ping-pong `0x22008c5c`/`0x22009c5c`, `count=moved=2048` per burst, cursor
  `0→4096→8192→…` monotonic.
- **WAV correlation (captured from the guest's I2S FIFO) vs the disk's MP3:
  Pearson=1.000** in steady state, with the offset advancing 1:1 with time
  (`wav+2s→song 1.62s`, `wav+4s→song 3.62s`); WAV peak 30420 ≈ MP3 peak 30653;
  real stereo (RMS(L−R)≫0). It is the SAME song, contiguous, in pitch. Correlator
  noise floor ~0.02–0.05 (baseline silence). ⇒ **The retailOS's real song
  PLAYS, through its own DMA→I2S→CS42L55.**

**The exact obstacle that REMAINS (honest).** The guest's render engine is still
in the deadlock: `copy32 (0x08016eb4)=0`; the engine `0x0808170c` runs ONCE at
~15 s (watchdog §11.16) and does not render. Consequences:
1. **The Now Playing screen does NOT advance / does not appear:** the guest's UI does not
   transition because its playback pipeline is stuck; the shim delivers the
   audio DOWNSTREAM of that engine. The shim breaks the silence, not the deadlock.
2. **Reset watchdog:** with nothing else, the retailOS resets (`SWRCON=0xaa5`, written
   at `0x08322b24`, then a nop spin at `0x08322b28`) ~15 s after `StartPlayback`
   (the baseline without HLE does too: it is the guest's deadlock). It cuts off the song.
   - **`HLE_AUDIO_SUSTAIN` (opt-in):** the clock model suppresses ONLY that
     watchdog reset (blank tag "----") while the HLE is feeding audio → the
     song plays IN FULL through the guest's DMA/I2S. Honest cost: the guest's
     CPU stays frozen in the `0x08322b28` spin (its attempt to play
     failed in the deadlock); the audio continues because the DMA is HW and the fill runs
     on the host timer. Without the flag, the guest follows its normal cycle (plays
     ~15 s and its watchdog resets).

Closing point 1 (UI + having the guest's engine render by itself) is the wall of
§11.12–11.18 (priming the MeCCA pipeline for `copy32>0`), not opened here.

**Honesty.** The PCM is the REAL song from the guest's disk (it correlates with
the same MP3 decoded separately); it goes out through the guest's OWN I2S
(DMAC0 ch1 → FIFO 0x3ca00010 → CS42L55), not through a sink bypass. The ONLY
host-side thing is WHERE the compressed audio is decoded — a data path HLE.
The guest drives the file open and the playback start; the shim
supplies the PCM that its stuck decoder does not produce, and (optionally) keeps its deadlock
watchdog from self-resetting. The Now Playing UI does NOT advance (obstacle noted).

**Hygiene / gating.** Everything under `HLE_AUDIO_DATAPATH` (`_TRACE`, `_TRACK`,
`_SUSTAIN`, `_DISK`); without the env the shim installs nothing and the machine is
byte-identical. Hooks: `ipod_classic.c` (init), `ipod_classic_i2s.c` (fill+commit,
clock edge), `ipod_classic_ata.c` (read offset for ID),
`ipod_classic_clk.c` (gated suppression of the watchdog reset). Regressions:
6G navtest and Rockbox (rbmusic) with the env unset — no changes.

### 11.20 Assault no. 12 (HLE_NP): own platform; decode kicked from idle does NOT deliver a frame; the wall is the decoder's CONTEXT+INPUT, not two fields

Session with an integrated module of our own, `hw/arm/ipod_classic/ipod_classic_hle_np.c`
(gated by `HLE_NP`, default build byte-identical), branch `dsp-deadlock-break`.
Goal authorized by the user: prime the control bindings (not PCM) so
that the guest's engine renders (`copy32>0`) and Now Playing advances. Everything
**VERIFIED** by non-invasive `pctrace`/`memw`/gdb + disasm (`~/6g/osos.dis`,
VA = off + 0x07FF51B8). Disk `Emulti.img`, `AUDIOPACE=1`, reliable SELECT.

**A) The copy function 0x08016e44 decoded (exact source and gate).**
`src = [r5+12] + (([r4+4]-r6-[r5])<<2)`; `gate = [r7+5]==1`. LIVE capture of its
args in the bring-up render: `r5=0x088890c0` → `[r5+12]=0x08889098` (source
buffer); `r7=0x0bb12eb4` → `[r7+5]=0` (gate closed); `r4=0x0bb12d54`. The copy
bails out on gate=0 / empty source. Source `0x08889098` = STATIC buffer in the
osos data section (< 0x08976f7c), not heap.

**B) The publish 0x081c12ac is PER MESSAGE, not per field (key point, corrects the previous
kicks framework).** It calls `reader.vtable[3]` (get-frame) and, if it returns !=0,
posts the frame downstream (`bl 0x832d9dc`); if 0, it does not post. **It does NOT write
the engine's `[r5+12]`/`[r7+5]`.** That is why ringing the doorbell (§11.17) or type-12
(§11.15) never wires up the binding: it comes via the MESSAGE that the engine must
receive and process, not via the doorbell.

**C) The production credit = `[[reader+0x30]+0x2b8]` (fn 0x81c0fd4, two instr:
`ldr r1,[r0+0x30]; ldr r0,[r1+0x2b8]`).** Live snapshot: reader=0x095562c8,
credit=0x02000000 (non-zero). The reader-pull 0x081e93d8 only does `cmp r0,#0; beq`
on it.

**D) MEASURED: the kick-from-idle does NOT deliver a frame.** With `HLE_NP` firing
case-1 `0x081232d8(decoder_obj,0)` from the RTXC idle in regime A (decoder
primed, `gate[ctx+0x60]=0x4`): breakpoint at the return of the publish's get-frame
(`0x081c12e8`) → **`r0=0` (no frame)**. `memw` of the static source
`0x08888000-0x0888c000` during play with the kick active: **zero non-
memset writers** (only `0x22000198/a0`). `copy32=0` invariant; WAV = only the UI click
(nonzero 0.3%, L==R 100%, peak 11444). The "innerdec 0x081e9e70" that §11.18 took
for "AAC math" is actually a **work queue manager** (0x8c bytes) —
it running does not imply producing PCM.

**E) VERDICT (measured, honest): the kick-from-idle is insufficient by design.**
The real decode requires the **t2a task context** (stack/state) and its
**input stream** (RECV mbox 9 ← StreamCache), both in the deadlock; running
the decode code from idle executes the instructions but **does not produce a
deliverable frame** via the message protocol. Priming the copy gate without PCM
in the source yields nothing. The only remaining native route is a **FULL HLE of the
decode** (synthesizing input+context+enqueueing via the MeCCA protocol), of
a size equivalent to reimplementing the codec integration — consistent with the
decision of §11.19 to decode host-side downstream.

**Reusable contributions (branch `dsp-deadlock-break`, module commit):** copy
decoded (A), publish=message (B), credit fn (C), and a gated `HLE_NP`
platform that fires the guest's decode at will for future experiments.
Recipes in `~/dbg/`: `run_np.sh`, `run_npsnap.sh`, `run_npmemw.sh`, `run_pub.sh`,
`np_snap.py`, `catch_copy.gdb`, `catch_publish.gdb`.

### 11.22 Assault #12 (cont.): why native injection does NOT converge -- the guest does not idle during the deadlock and t25 is torn down before the window

Exhaustive continuation of assault #12 with the `HLE_NP` platform. Measured at the
instruction level (pctrace `regpc`/`memw`, non-invasive gdb, iram/osos disasm). Each
injection layer tried runs into the next one; the summary of why:

**A) The copy 0x08016e44 (measured in the natural bring-up):** reads `count=[r4+16]`
bytes from `[r5+12]+([r4+4]<<2)`. Values: `[r5+12]=0x08889098`, `[r4+4]=0`,
`[r4+16]=0x28` (40 B), gate `[r7+5]`. The buffer 0x08889098 is a 40 B staging
area; the descriptor r5=0x088890c0 comes right behind it (base+0x28), so a
large window corrupts it (fixed: fill exactly `count`).

**B) Dynamic descriptors:** on the NATURAL path (t25) the copy uses the stable
HEAP descriptor `r4=0x0bb12d54 r7=0x0bb12eb4` (pokeable); calling the engine
0x0808170c DIRECTLY uses transient STACK descriptors (`0x0896xxxx`, they go down
0x3e0/render) that cannot be poked from idle. The natural path runs ONCE
(gated by the job counter `[r5+12]=[0x22008c1c]`, render-per-job 0x22005dec).

**C) The gate `[r7+5]` is derived by the engine from the stream fill state**
(0x088890c0: `[+0]=0 [+4]=0x08889088 [+8]=0xa [+c]buf=0x08889098 [+10]=0x004c1bf8
[+14]=0x580`), not from the buffer contents. Setting it requires the semantics of those
fields (another layer).

**D) THERE IS NO SAFE INJECTION WINDOW (the finding that closes the kick route):**
- During the deadlock the guest **does NOT idle**: the deferred PCs are live MeCCA
  loops (`0x0813dd3c`, `0x081611f8`, `0x081e47fc`, `0x080c8bb0`, `0x0815bb54`).
  Injecting from idle **always defers** (measured: deferred monotonic, bind=0).
- When the CPU reaches a safe point (the reset spin suppressed by
  `HLE_AUDIO_SUSTAIN`, 0x08322b28), **t25 has already been torn down** (3 s recv-timeout →
  `st=00`, `waiter=0`): the doorbell has no target.
- Without SUSTAIN, the guest resets at ~15 s. With SUSTAIN, it spins dead. In
  neither case is there an instant with (t25 alive + CPU at a safe point +
  decoded PCM ready) at the same time.

**VERDICT (measured): injecting state/kicks from the emulator CANNOT
open the native pipeline.** Not for lack of a specific value, but because (a) there is
no safe injection point synchronized with a live t25, and (b) the render is a
cascade of dynamic layers (copy←descriptor←engine-build←stream-state←job-count←
t25-loop←message) that would all have to be driven AT ONCE, in the correct task
context, with no instant in which to do it. It is the finest confirmation of the
"structural bootstrap deadlock" of §11.12-11.18: opening it is equivalent to
reimplementing MeCCA's render scheduler, not priming fields. The `HLE_NP` platform
(gated, default-inert) remains for future attempts.

### 11.23 WHERE WE ARE: usable HLE Now Playing player (overlay), with play/pause/seek/skip

After closing by measurement that MeCCA's NATIVE render pipeline is a structural
deadlock with no opening via injection (§11.12-11.22), and validating with a
multi-agent investigation what IS reachable, a **functional Now Playing player via HLE**
was implemented (not Apple's native UI). Committed on the branch
`dsp-deadlock-break`.

**What is real and what is HLE (honest).**
- **Audio: REAL.** The song from the guest's disk, decoded host-side, through
  its own DMA/I2S (CS42L55) → speakers (Pearson 1.000, §11.19).
- **Player screen: HLE (drawn by QEMU).** An overlay on the
  RGB565 framebuffer (`ipod_classic_np_overlay.c`), NOT the retailOS's Now Playing
  screen (which is never drawn: native render in deadlock, `copy32`=0).
- **Controls: real on top of the datapath.** The wheel buttons drive the
  HLE playback, not the native media player.

**The two key fixes that made it possible.**
1. **The guest no longer freezes after 15 s.** `ipod_classic_clk.c`: when
   suppressing the blank-tag "--------" watchdog reset, it now **redirects the
   CPU to return from the reset routine** (PC=LR, routine 0x08322b10 is a
   leaf called with bl) instead of leaving it in the dead spin 0x08322b28. Also,
   with `HLE_AUDIO_SUSTAIN` the blank reset is suppressed **also during menu
   navigation** (gate `ipod_classic_hle_audio_sustain_on()`), not only while
   playing, so that the guest does not reset at ~46 s before reaching Music.
   (Workflow discovery: the screen does NOT depend on `copy32` nor on the
   scaler; the only blocker for a stable screen was this auto-reset, and
   the previous SUSTAIN suppressed it TOO LATE, leaving the CPU frozen.)
2. **Control layer on top of the single cursor `hle.pcm_cursor`** (`hle_audio.c`):
   pause (freezes feed+drain in `i2s.c` → real silence, no hum),
   seek ±s (jumps the cursor), next/previous (worker restructured into a loop
   of decode requests with a generation counter), and position/
   duration/track getters. `sm1.c` +0xa48 reads the datapath cursor. Real metadata
   (title/artist/album) via `ffprobe`.

**Controls (keyboard):** `s`=play/pause, `d`/`a`=next/previous, `]`/`[`=
seek ±5 s, `w`=hide (back to the menu). The overlay auto-appears on decoding
a track and **does NOT hijack SELECT** (bug fixed: it made the menu "get stuck").

**Verified (screenshots + WAV):** overlay shows "Numb / Encore" — "Linkin Park",
"Track 1 of 4", bar + clock advancing (0:06→0:47→1:02); pause freezes the clock
("PAUSED") and the WAV stops growing (silence); seek jumps; next re-decodes
(track 1→2→3 of 4). All gated (`HLE_NP_OVERLAY` + `HLE_AUDIO_DATAPATH`/
`SUSTAIN`); default build byte-identical (no Rockbox/navtest regression).

**How to launch it:** `HLE_AUDIO_DATAPATH=1 HLE_AUDIO_DISK=<img> HLE_AUDIO_SUSTAIN=1
AUDIOPACE=1 AUDIOLIVE=1 HLE_NP_OVERLAY=1 qemu-system-arm -M iPod-Classic -kernel
osos_decrypted.bin -drive if=ide,format=raw,file=<img> -snapshot -audio pa`.

**What remains (native, no guarantee):** making Apple's REAL Now Playing screen
appear requires rebuilding MeCCA's streams/jobs/messages protocol
layer by layer (§11.22); the redirect already keeps the guest alive, but its native
time counter is still dead. Out of scope for this delivery.

### 11.24 Assault #13: the TIMING hypothesis refuted by measurement (icount sweep), and the render enqueue located as a DEAD path when cold

Resumption session on a new machine (clean clone + WSL Ubuntu-24.04, QEMU 8.2 +
`libpctrace.so` rebuilt, assets from the `assets-v1` Release). The baseline
**reproduces the deadlock identically** to the documented one (`copy32=0`, `h12=1`, `ex6=57`
matches, `mbox52` zero sends, `engine=1`) — the emulation is faithful across machines.
Two NEW experiments, both **VERIFIED** by non-invasive `pctrace`. Runners:
`tools/session-dbg/run_icount.sh`, `sweep_icount.sh`, `run_bind.sh`.

**A) The `-icount` sweep — the timing hypothesis, proposed 3× (§11.12/§11.14/
§11.16) and never run, is REFUTED by measurement.** The same navigated play
probe was run under `-icount shift={off,1,2,3,4}` (≈1000, 500, 250, 125, 62.5 MIPS;
the real ARM926 core is around ~200 MIPS ⇒ shift 2-3). The core is single (`max_cpus=1`,
`arm926`) and no device on the machine uses the host clock (i2s/timers/pl080 =
`QEMU_CLOCK_VIRTUAL`/`ptimer`), so under icount the machine is **deterministic**.
Result:

| shift | MIPS | copy32 | h12 | ex6 | mbox52 sends | engine |
|---|---|---|---|---|---|---|
| off | ~1000 | **0** | 1 | 57 | 0 | 1 |
| 1 | 500 | **0** | 1 | 57 | 0 | 1 |
| 2 | 250 | **0** | 1 | 57 | 0 | 1 |
| 3 | 125 | **0** | 1 | 56 | 0 | 1 |
| 4 | 62.5 | **0** | 1 | 56 | 0 | 1 |

`copy32=0` is **invariant** across the whole range. The wall is **NOT a scheduling
race** sensitive to guest speed: it is structural (confirms §11.13-E,
and proves the auditor of §11.13/§11.14 right — "determinism alone is not enough").
The A/B regime of §11.11 was an artifact of the instrumentation's WEIGHT, not of the CPU
rate. `icount` is **closed as a fix**; it now only serves as deterministic replay
for heavy probes.

**B) The render enqueue, located at the instruction level as a DEAD path
when cold (refines §11.18).** Decoded in `iram.dis`: the render job counter
`[0x22008c1c]` and the engine object `[0x220106d0]` are shared by a **family of
doorbells** of the engine, all of the form `[0x22008c1c]++; signal(engine)` via the
KS veneer `0x22003670→0x080a1858`:
- `0x220060e0` (`jobenq`, gated by `[engobj+0x44]`): **hits=0** — never runs.
- `0x2200683c…0x22006878` (the "render doorbell" `0x22006870` of §11.17): gated
  by **`[engobj+0x46]!=0`** (`cmp [r0+0x46],#0; bxeq lr`); §11.18-C measured
  `+0x46=0` when cold ⇒ **returns without enqueueing**.
- `0x22005e58` (`bindchk`: `if [descr+5]==1 return; else job_setup`): **hits=0** —
  the check that would bind source↔copy **is never invoked** (it is dispatched via
  vtable `bx r1`, not via `bl`, and its slot is not reached when cold).

Probe `run_bind.sh` (pc-watch + `memw=0x22008c1c`): the job counter receives **3
writes in total** — 2 from a `memset` (`0x080001a0`) and **only 1** from the engine's
single bring-up (`0x220067b8`) — and **ZERO from the `jobenq` incrementer
(`0x220060f8`)**. That is: **a render job is never enqueued**; the engine runs its
setup once (§11.13-B) and keeps waiting for the doorbell that its own gate
`[engobj+0x46]=0` prevents from being issued. It is the cleanest statement of the wall: it is not that
a value is missing, it is that **the render enqueue vtable slot is never dispatched
in the cold boot**, because its enablement (`+0x46`, and symmetrically the
binding of descriptor `0x0bb12d54`) is only set by the render cycle itself, which
never starts. Consistent with the symmetry of §11.18-D; forcing a single gate is not enough
(§11.18 already forced `+0x46=1`/`+88=1` → `copy32=0`).

**VERDICT (measured, this session):** assault #13 **closes the timing route** (icount
refuted) and **narrows the wall** down to the render enqueue vtable slot that is not
dispatched when cold. It does NOT open the native pipeline. The only remaining route is still
that of §11.14/§11.18: an **HLE of MeCCA priming** that dispatches the first render job
in the correct task context (set `+0x46`, enqueue via `0x220060e0`, bind
descriptor `0x0bb12d54` with `[descr+5]=1`) — open work, with no guarantee of
convergence. The physical iPod **contributes nothing** here (diskless, emCORE only; it does not run the
retailOS playing — wiki/10 §0/§5). Model tree byte-identical (only this
wiki entry + runners in `tools/session-dbg/`); baseline/Rockbox untouched.

### 11.25 Assault #13 (cont.): the MeCCA engine loop fully decoded; SM1 without an IRQ (a REAL fidelity gap); and why forcing each layer is not enough

A long resumption session on a new machine. It contains **an important correction of one of
our own claims** (§A), the **complete mechanism of the render loop** that thirteen
assaults had not decoded (§C), and a **genuine model fidelity gap**
(§D). The native wall does **NOT fall**, but the map changes substantially.

**A) CORRECTION: the `copy32=2` of §11.24 was NOT a render.** With an external PC
hook (gdb breakpoint at `0x08016e88`, CPU halted) `[r7+5]=1` was forced and the copy
**executed** — `copy32=2`, `copygo=1`, the first time in 13 assaults that the counter
moves. But measuring the destination pointer at the store (`regpc` at `0x08016eb4`):
**`r2 = 0x2238b800`**, and the S5L8702's IRAM runs from `0x22000000` to `0x22040000`. The
copy was writing **3.4 MB OUTSIDE RAM**, because the descriptor's `[r4+12]` field
is `0x0036b800` (garbage). **The gate was closed for a legitimate reason:** the
render context does not exist when cold. Forcing it produces an out-of-range write,
not audio. Lesson: a counter that moves is not a frame.

**A2) The exception storm is NOT caused by the hooks.** `CPU_exception_handler`
(the retailOS's semihosting): **1** on a clean baseline, **111 722** in every run with
`HLE_AUDIO_SUSTAIN`, including those that carry no hook and no `HLE_NP`. It is a
pre-existing cost of SUSTAIN (it suppresses a reset the firmware wants) and should be kept
in mind when interpreting any measurement made with that configuration.

**B) The injection window, RESOLVED (mechanism).** `HLE_NP_PASSIVE` (new,
gated): it primes the copy's source and the gates through **pure memory writes**,
without hijacking the PC or requiring the CPU to be idle. **`deferred=0`**, against 1000+ deferred
attempts and **zero** landed kicks with the method of §11.22-D. The "there is no injection
window" wall is overcome as a mechanism — what is not enough is what gets injected.
It was also measured that the host decode (`have_pcm`) is only ready ~48 s **after** the
single natural render, so the prefill is armed from play-start.

**C) THE ENGINE LOOP, FULLY DECODED** (disassembly, partly through multi-agent
analysis; it corrects the mental model of §11.13/§11.14):

- **"mbox 0x52" is NOT a mailbox.** It is a **condition variable** (`0x220106d0`) with
  a 3000 ms timeout (`0x22006814` → `0x080b3e78` → `0x0805c518`). Its "post"
  (`0x080a1858` with `r1=0` → `0x08051bac`) is a **data-less notify-one**, and it is a
  **no-op if nobody is queued**. That is why forcing the `recv` to return success
  (measured: 6 times) produced absolutely nothing.
- **The engine is the `"MeCCADecode"` thread** (IRAM `0x22002f5c`, created at `0x22005f48`,
  16 KB stack, prio 10). Siblings from the same constructor: `"FrameDone"`
  (`0x22004630`) and `"EndOfStream"` (`0x22004754`).
- **On waking it does NOT act on the signal:** it re-tests the **job counter
  `0x22008c1c`** (`0x22002fe0`: `ldr/cmp/ble`, signed) and if it is `<= 0` it goes
  straight back to its 3 s wait. Notifications are irrelevant without jobs.
- **With `jobcount > 0`** it calls the pump (`0x22005dec` → vtable slot +0x20 →
  `0x22006118`) and **decrements** the counter.
- **The loop is self-sustaining by design:** if the pump returns 0, the engine
  **re-queues** a job (`0x22005e38` → `0x220067e4`, `jobcount++`) — the counter is
  maintained and the render runs free. **A single successful pump would be enough.**
- **What breaks it:** the pump **aborts with −1 right on entry** if
  **`[engobj+0x44] != 0`** (`0x22006170`, writes state 5 to `0x22008c54`); a second
  early exit if `[engobj+0x4c] == 2`. A single −1 and the engine **does not re-queue**: the
  counter drops to 0 and the loop **parks forever**. That is the deadlock.
- **Measured and still unexplained:** the render called at `0x22006300` **never returns**
  (`0x22006304` hits=0, **also on a clean baseline**) — consistent with the
  `dpgateclr = 0` that §11.18-C measured without interpreting.

**D) A REAL FIDELITY GAP: SM1 had no interrupt, and IRQ 18 was not
wired.**

- The retailOS **installs a handler** (`0x08017378`) at **index 18** of the VIC
  table (chain `0x0813e688` → `0x080159c0` → IRAM `0x22004cc4`, which stores the
  handler at `table+8+id*4` and touches `VIC0+0x14`).
- That handler reads an **SM1** register (getter `0x080a7638`, the same one from the audio
  bring-up), tests **bit 2**, and **signals event `0x22008c58`** (`0x080173cc` →
  `0x080a1a24`) — exactly the event that the **pump predicate** (`0x0808af58`)
  waits **500 ms** for before giving up. (Note: the predicate's return value comes from that
  wait, **not** from the test of bit 12 of SM1+0xa98, which only gates some enable
  writes — that explains why the experiment of §11.14-D saw nothing.)
- **The guest has line 18 ENABLED:** `VIC0 IntEnable = 0x21a7010f` (§11.12-C)
  has **bit 18 set**, alongside 16/17 (DMAC), 8 (timer), 23 (wheel), 29 (ATA).
- **Our model could never raise it:** `ipod_classic_sm1.c` had no
  `qemu_irq` or `sysbus_init_irq`, and the machine never did `sysbus_connect_irq`. The
  IRQ map came from Rockbox (`s5l87xx.h`), whose table has a **GAP right at
  18** because Rockbox decodes on the ARM and **does not use SM1**.
- **Implemented:** `qemu_irq` in SM1 + `sysbus_connect_irq` to `S5L8702_SM1_IRQ = 18`.
  Gated firing: `SM1_IRQ=1` (pulses when the engine is `powered`), `SM1_IRQ=2`
  (unconditional, diagnostic).
- **Measured:** with `=1` **it never fires** — SM1's `powered` flag **is not set
  anywhere on the play path** (11 idle diagnostics in ~110 s), which is itself a
  finding still to be explained. With `=2` the early pulse **kills the guest** at
  ~125 ms (it jumps to a vector before the handler is installed). **The line is now
  wired; faithful firing remains to be characterized.**

**E) Priming the job counter: it makes the loop spin, but does not render.**
`HLE_NP_JOB=1` keeps `[0x22008c1c] >= 1` and `[engobj+0x44] = 0`. Measured
(`run_pump.sh job2/job3`, identical with and without SUSTAIN):
`motorpred=35 motorpump=22 dpump=22 jobrepost=11 pumpabort=11` **but `engine=0`,
`copy32=0`, `resets=11`**. That is: the engine **does** cycle (baseline: `dpump=1`,
parked) and **does re-queue on its own** (`jobrepost=11`, the self-sustaining behavior of
§C exists and is observed), but half the passes abort and the other half
**return 0 without reaching the render**; the guest ends up rebooting 11 times. Compared with the
baseline (`engine=1`) this is a **regression**: injecting jobs from outside,
asynchronously to the engine's mutex, makes the loop spin idle. **It does not open the pipeline.**

**Honest VERDICT of assault #13.** The native pipeline **remains closed** and
Apple's Now Playing screen **is not drawn**. What is established:

1. The **timing hypothesis is refuted** (§11.24-A, icount sweep).
2. The **render loop mechanism is decoded** (§C) — data-less condvar,
   job counter, self-sustaining re-queuing, abort flag `[engobj+0x44]`.
3. There is a **real fidelity gap, half wired** (§D): SM1 without an interrupt
   line and IRQ 18 unwired, with the guest waiting for it.
4. Each forced layer reveals that **the one below is not built either** (descriptor
   with a garbage destination, non-existent render context, `powered` that never goes up). The
   pattern suggests that **a render context construction step is missing**,
   upstream of everything forced, and that that —not a gate— is the target of assault #14.

**Proposed concrete next step:** find out **why SM1's `powered` is never
set on the play path** (does the audio bring-up not run all the way through?) and **why
the render `0x0808170c` does not return**. Both are closed questions, cheap to
measure, and either of them may be the missing context construction.

**Hygiene:** everything gated (`SM1_IRQ`, `HLE_NP_PASSIVE`, `HLE_NP_JOB`) or external
(gdb hooks in `tools/session-dbg/`); **default build byte-identical**; Rockbox and
navtest untouched. `pctrace` extended to dump `r4..r7` in `regpc`/`sendex`.
New runners: `run_icount.sh`, `sweep_icount.sh`, `run_bind.sh`, `run_np_prime.sh`,
`run_descr.sh`, `run_cascade.sh`, `run_pump.sh`, `copygate_hook.gdb`,
`cascade_hook.gdb`.

### 11.26 THE WALL FALLS: SM1's position was a RING OFFSET — the native render was NOT deadlocked, it was FAULTING

**The finding that closes fourteen assaults.** For fourteen sessions the native render
was characterized as a "structural rendezvous deadlock" of the MeCCA framework.
**That was incorrect.** The render was not waiting for anyone: **it was taking a data abort** on an
address that our own model was making it compute wrongly.

**A) The complete chain, located block by block** (`pctrace range=` + `trig=`,
which was the decisive tool: it traces the PC of every translated block within a
window):

```
pump 0x22006118 → render 0x0808170c → mixer 0x0809a74c → 0x08016f14
   → 0x08016f68 → copy 0x08016e44 → gate [r7+5] closed → 0x08016ecc
   → bl 0x08085ea0   ← EVERYTHING STOPS HERE
```

Each step was measured by the next block that does **not** get to execute: the trace
of the render ends at `0x08081a8c` (the block containing the `bl` to the mixer), the
mixer's ends at `0x0809a880` (the block containing `bl 0x08016f14`), and the
per-stream render's ends at `0x08016ecc` (the block containing `bl 0x08085ea0`).

**B) `0x08085ea0` is not a wait: it is a memcpy.**

```
8085ea0:  ldr r3,[r1,#16]   ; bytes
8085ea4:  ldr r1,[r1,#12]   ; destination base
8085ea8:  lsr r3,r3,#2      ; words
8085eac:  add r1,r1,r2      ; destination = base + offset
8085eb0:  cmp r3,#0
8085eb4:  ldrne r2,[r0],#4
8085ebc:  strne r2,[r1],#4
8085ec0:  bne 0x8085eb0
8085ec4:  bx lr             ← NEVER reached (measured, hits=0)
```

Live descriptor dump (gdb, `memcpy_probe.gdb`): `[descr+12]=0`,
`[descr+16]=0x28` → **40 bytes, 10 words**. It is the silence copy, tiny.
It is not an infinite loop.

**C) The destination, and the causal proof.** The measured destination was `0x222dc000` in one
run and `0x222fbc00` in another — **it grows with playback time**. With the
SM1 position forced to 0 (`SM1_POS=0`) the destination is **exactly `0x22020000`**.
That is:

```
destination = 0x22020000 (IRAM1 base) + SM1_position × 4
```

The audio output buffer **lives in IRAM1** (128 KiB), and the firmware computes its
write pointer as base + position in stereo frames.

**D) THE FAULT WAS OURS.** `sm1_pos_reg()` returned the position as
`i2s->tx_samples / 2`, a **cumulative counter that never wraps**. At ~1.9 s
of playback (32768 frames) the pointer went out of IRAM1; the 10 writes
fell into **unmapped memory**, took a **data abort**, and the copy did not return —
nor did the mixer, the render, or the pump. From outside, indistinguishable from a deadlock:
all tasks stopped, no IRQ pending, no hardware poll.

This explains why **nothing** attempted in fourteen assaults worked: the
`icount` sweep (§11.24-A), priming the gates, forcing the engine's `recv`, priming the
job counter — all of them attacked a symptom. And it also explains why
forcing the copy gate (§11.25-A) only produced an out-of-range write: the
pointer was already corrupt before getting there.

**E) THE FIX (hardware fidelity, without touching the firmware).** The position is
an **offset within the ring**, so it wraps:

```c
#define S5L8702_SM1_POS_RING_FRAMES  (0x20000 / 4)   /* IRAM1 = 128 KiB */
pos %= S5L8702_SM1_POS_RING_FRAMES;
```

Every write the firmware issues now falls inside IRAM1. `SM1_POS=<frames>`
allows overriding the wrap for experiments.

**F) MEASURED (baseline of 14 assaults → with the fix, default build):**

| counter | before | after |
|---|---|---|
| `rendret` (0x22006304, the render returns) | **0** | **2** |
| `dpump` (0x22006118) | 1 | **5** |
| `pumploop` (0x22006324) | 0 | **4** |
| `pumppred` (0x0808af58) | 0 | **4** |
| `mixer` (0x0809a74c) | 1 | **5** |
| `copyent` (0x08016e44) | 1 | **144** |
| `jobrepost` (0x220067e4) | 0 | **2** |
| exceptions | 1 | 1 (clean) |

**Apple's native render returns for the first time and the loop cycles**, with the engine
re-queuing jobs on its own — the self-sustaining behavior described in §11.25-C exists and
is now observed.

**G) What remains.** `copy32` is still **0**: the copy's `[r7+5]` gate never passes
because the decoder does not yet publish a frame (`decframe 0x081232d8` hits=0; of 87
entries into the copy only 12 reach the gate check, the rest take the
`[r4]!=0` branch). The next link is **who sends the decoder the decode
message** (code `0x52800001`, case 1 of the switch `0x08123cfc`) and why it does not
arrive. Given this session's precedent, it is worth looking for **another register whose
semantics we model wrongly**, not guest logic.

**H) The method that solved it, for future sessions.** The decisive thing was not a
hypothesis but a technique: **bounded block tracing** (`range=LO-HI` with
`trig=ADDR` to open the window) that shows the last block executed before
control disappears, applied recursively into each call.
Four runs were enough to go from "the render does not return" to the exact
instruction. Aggregate counters (`pc=`) say *whether* something runs; block tracing
says *where it stops running*.

**Hygiene:** the fix is on by default (Rockbox does not read SM1, so it is not affected; boot and
navigation do not enter the bring-up either). `SM1_POS` remains as an experiment knob.
New tools: `run_sm1en.sh`, `run_rtrace.sh`, `memcpy_probe.gdb`.

### 11.27 SERIOUS CORRECTION: the "audio decoder" of the `0x08123xxx` cluster is the COVER ART thread (`iMAImageCacheThread`)

A finding from a multi-agent analysis of the disassembly, **verified by hand**. It forces
a reinterpretation of a good part of §11.9–§11.22.

**A) The proof.** The singleton at `0x0891233c` on which that whole
cluster is built is created at `0x08123400`, which passes to thread creation (`0x0832dd88`) the
string at `0x081234f4`. Its words, in little-endian:

```
81234f4: 49414d69 → "iMAI"
81234f8: 6567616d → "mage"
81234fc: 68636143 → "Cach"
8123500: 72685465 → "eThr"
8123504: 00646165 → "ead\0"          ⇒ "iMAImageCacheThread"
```

The neighboring strings in the same code island corroborate it: `"iTunes Image DB.itdb"`
(`0x08122e9c`, `0x081d7cb0`), `"iTunes Image DB"` (`0x081f07e0`) and
`"ImagePresentationEngine"` (`0x081d8f0c`).

**B) What gets reattributed.** The following belong to the **cover art / iTunes Image
DB** subsystem, not to audio:
- the dispatcher `0x08123dcc` (and therefore the `disp` counters),
- the switch `0x08123cfc` and its family of codes `0x52800000..0x52800009`,
- case 1 (`0x08123d64` → `0x081232d8`), which is **"run the next image
  job"**, not "decode an audio frame",
- the type-12 handler `0x08123970` (`h12`) and `dwork` `0x08123bfc`.

The four senders that send code `0x52800001` are **pixel** paths:
`0x08122b28` and `0x08122be8` (RGB565 expansion: format constants `0x1888`/`0x565`,
masks `0x7e0`/`0xf800`) and `0x0813ada8` / `0x0813b090` (YUV 4:2:0: `w*h*3/2`). In
that class, `reason==1` means **"my destination surface is not ready yet,
re-queue me"** — a retry, not a start.

**C) Second reattribution.** `0x08025ac4` does **not** "request input via exchange 6 +
RECV mbox 9": it is a **trace emitter**. It acquires trace channel 9
(`mov r0,#9; bl 0x8015730`), emits a record (`bl 0x8015740`) and releases it.
Therefore the "57 posts to exchange 6" of §11.14-A were counting **trace
records**, not data flow, and the conclusion "the input side is healthy" does not
hold on the basis of that measurement.

**D) What does NOT change.** The fix of §11.26 (SM1's position as a ring
offset) is independent of all this: it was verified **causally** by the copy's destination
address (`SM1_POS=0` → destination exactly `0x22020000`) and by the measured effect
on the REAL render path (the memcpy goes from never returning to returning
101/102 times; `rendret` 0→2; `copy16`=2405 PCM stores). None of those
measurements depends on the image cluster.

**E) Practical consequence.** `decframe = 0` **never was** an audio indicator, and
neither were `h12` or `dwork`. The right question, now cleared up, is: **who fills the
mixer's source ring at `0x08889098`**, which the native copy reads from. That is the
real audio decoder, and it has not been located.

**F) Method lesson.** Two central premises of the project (the decoder
cluster and the exchange 6 input channel) turned out to be different
subsystems, and they survived fourteen assaults because nobody verified the **identity** of
the objects: decoding one string would have sufficed. Before building on a cluster,
confirm which thread/class it belongs to (thread name at creation,
neighboring strings, vtable).

### 11.28 Second fidelity fault: SM1's sub-block state was a sticky latch — and the render now builds its context

Direct continuation of §11.26. Found by **auditing the model against the
firmware that consumes it** (the same technique that produced §11.26), and confirmed by measurement
before and after the change.

**A) The fault.** We modeled `sub+0x14` (read by `0x080b8470`, masked `&7`)
as *"7 forever as soon as any command is written"*. It is a **state
machine**, and two places in the firmware read it expecting **opposite** values:

- **The power-up `0x080ba768` wants 7.** It writes cmd 4, polls up to ten times
  waiting for 7, writes cmd 16 (run) and **finally writes cmd 96 (stop)**.
- **The render `0x0808170c` wants 0:**

```
80820e8:  ldrb r0,[r7,#1]
80820f0:  bl   0x80b8470      ; state of sub-block 4
80820f4:  cmp  r0, #0
80820f8:  bne  0x80821a8      ; non-zero → SKIPS the build
```

The block that gets skipped, `0x08082100`–`0x080821a0`, is the **construction of the
engine context**: buffer copy, descriptor registration (`0x080b67a8`), chaining
(`0x080bfa58`) and address translation (`0x08075d80`).

Since the last command the power-up writes is 96 (stop), on silicon the state
returns to 0 and the render builds its context. Our latch answered 7 forever,
so **the context was never built**.

**B) Measured, before the change:** `statetest=1, ctxbuild=0, ctxskip=1` — the render reads
the state and skips its own build. It is exactly the *"missing context
construction step"* that §11.25 predicted, and it explains why each layer forced
downstream revealed that the one below was not built either.

**C) The fix.** 7 while the last command is 4 (power up) or 16 (run); 0 after
96 (stop) or if never commanded. Constants `S5L8702_SM1_SUB_CMD_UP/RUN/STOP`.

**D) Measured, after (run with confirmed playback, `StartPlayback=1`):**

| counter | 14-assault baseline | §11.26 | + §11.28 |
|---|---|---|---|
| `ctxbuild` (`0x08082100`) | 0 | 0 | **2** |
| `rendret` (`0x22006304`) | 0 | 2 | **2** |
| `engine` (`0x0808170c`) | 1 | 3 | **3** |
| `copyent` (`0x08016e44`) | 1 | 144 | **144** |
| `copy16` (`0x08016f08`, real PCM) | 0 | 2405 | **2405** |

**E) The render's destination, now correct.** With both fixes, the measured destination of the
copy is **`0x22026400`** — inside IRAM1 (`0x22020000`–`0x22040000`). Before
§11.26 it was `0x222fbc00`, 2.6 MB outside RAM. The native pipeline writes **where
it should**, with offsets that wrap in the ring.

**F) What remains, precisely isolated.** The mixer's source ring,
`0x08889098`, receives **only `memset`**: a memory watch on
`0x08889000-0x0888a000` gives **2048 writes from exactly 2 PCs**, both memset
routines in IRAM (`0x22000198`, `0x220001a0`). **Nobody writes decoded PCM
there.** That is why the native render, which now works, copies silence: the captured WAV
(91 s) contains only the UI clicks (identical bursts of 1248 samples, constant
peak 11444).

**G) Leads for the next assault.** The real names of the audio
components, extracted from the image:

| VA | name |
|---|---|
| `0x07ffb148` | `MeCCADecode` (engine thread, in the IRAM blob) |
| `0x0816c130` | `MeCCA_PCM_Output` |
| `0x081840ec` | `MeCCA_MediaPlayer` |
| `0x0818d5d4` | `MeCCA_VideoBufferMgr` |
| `0x0818d7dc` | `MeCCAVideoDecode` |
| `0x081c4460` | `MeCCAIOTask` |
| `0x08388c5c` | `MeCCARecordingTask` |
| `0x08859824` | `MeCCA_StreamCache` |

And a gift: the function **`0x0816bf48`** contains a **diagnostic dump that the
firmware itself ships** — it writes its PCM to `"MeCCA_PCM_Output.wav"` (`0x0816c080`
loads the name; the flag and the byte budget live in the struct `0x08948f24`,
byte 0 = enabled, +4 = bytes remaining; it writes `r6` with `r4*4` bytes via
`0x08121a00`). That function **receives the produced PCM**, so it is the exact point
where the retailOS's audio is born — and by enabling its flag the firmware would dump
the audio by itself, without any instrumentation of ours.

**Hygiene:** both fixes are on by default and only affect the retailOS audio
path (Rockbox does not read SM1). New probes in `run_pump.sh`
(`statetest`/`ctxbuild`/`ctxskip`/`pcmout`/`copy16`) and a port guard so that
repeated runs stop colliding silently.

### 11.29 SHA-1 was not modeled: the panic and the reboots go away — and the device becomes navigable for the first time

A session focused on **using** the player, not just measuring it. Three results: a
third fidelity failure found and fixed, the navigation mechanics characterized (it had
been silently invalidating tests), and the state of the audio narrowed down with clean
data.

**A) The reboot every ~46 s was not the battery: it was a firmware PANIC.**
Of the seven paths to the reset `0x08322b10` only `0x08021c00` fires, and what comes
immediately before it is:

```
8021bec:  ldr r1, =0x08375894
8021bf8:  svc 0x00123456      ; prints via semihosting
8021c00:  b   0x8322b10       ; and reboots
```

The string at `0x08375894` is literally **`"CPU_exception_handler"`**. But with
`-d int` **100 119 exceptions were measured, ALL IRQ**, none an abort: the CPU never
faults. It is the firmware's own panic path.

**Cause:** the **SHA-1 accelerator** (`0x38000000`) was an unimplemented window and
the retailOS really does use it. With `-d unimp`: it writes the config at `+0x00`, arms
memory mode at `+0x80`, sets the size at `+0x8c` and reads the digest from
`+0x20..+0x34` — receiving **zeros**. A hash that never matches ends in the panic.

**Fix:** the S5L8720 block already modeled in `hw/arm/ipod_touch_sha1.c` has
**exactly** that register map, so it works as is for the S5L8702. Reused, not
rewritten. **Measured: `resets=0 panics=0`** (before, 1-2 per run).

**B) The trade-off it imposes, measured (A/B on the SAME image).**

| | Library | Reboots |
|---|---|---|
| `SHA1_OFF=1` | **4 songs** | 1 |
| default | "No hay música" | **0** |

With the engine working the firmware **really validates** the hash of the image's
iTunesDB and rejects it; before, anything passed because the digest was zeros. The fix
is correct as hardware; what it uncovers is that the image's library carries a
signature this device does not reproduce (`SysInfo` has
`FirewireGuid: 0000000000000000`). `SHA1_OFF` is kept because being able to do the A/B
on the same image is what distinguishes "the model is wrong" from "the image is
wrong".

**C) Navigation, characterized — it was silently invalidating tests.**
1. A clean boot (with `-snapshot` nothing persists) enters the **language selector**,
   not the main menu. If it is not dismissed first, **every later press is choosing a
   language instead of a song** — that is how the menu ended up in Greek several times,
   and that explains navigations "that did nothing".
2. The retailOS clickwheel **discards ~5 of every 7 SELECT presses** (documented in
   `iremote.py`). A single press is **indistinguishable from a dead button**; you have
   to use the form with retries. With retries, SELECT enters.
3. `tools/session-dbg/run_play.sh` incorporates both things and drives the device all
   the way to playback, measuring the audio by the **RMS of the I2S WAV**, not by eye.

**D) The state of the audio, narrowed down.** With playback started
(`StartPlayback=1`):

```
dpump=3  engine=2  rendret=1  ctxbuild=1  mixer=3
copyent=87  copy16=1203        <- the NATIVE render writes PCM
framepull=1  aacmath=1  fpull2=0  credread=8
writers of the credit +0x2b8:  0x081cc1f4=1  0x081cc7a4=16  0x081d7548=0  0x081e360c=1
WAV 91 s: 0.19 % of samples nonzero, all identical bursts of 1248 with constant
peak 11444  ->  they are the UI CLICKS, not music.
```

**E) Two things ruled out with clean data.**
- **The DSP does not take part.** During playback there is **not a single access** to
  `0x39000000`/`0x3F000000`. The only unimplemented blocks touched are
  gpio/eic/miu/lcd/wdt. The old CalmADM2E theory is closed by measurement.
- **`0x0816bf48` is not the per-frame PCM push.** It is called **once** and with
  `r1 = 0x080338f4`, which is a **code** address: it is the registration of the audio
  sink. (`r0=0x0bb169dc r2=0x08a873c0 r3=0`.)

**F) An error of our own, corrected and verified.** The source ring being watched
(`0x08889098`) is the **boot** render's; in real playback the copy reads from
`0x0888a2xx-0x0888a6xx` (measured live with `regpc` at `0x08016edc`). The watch was
repeated over the correct window (`0x0888a000-0x0888c000`) **and the conclusion does
not change**: 4096 writes, **only** from the two IRAM memset routines
(`0x22000198`, `0x220001a0`), all at boot. **Nobody deposits decoded PCM**, and that
is why the native render —which already works— copies silence.

**VERDICT.** The device is today **navigable and stable** (no reboots), its **native
render pipeline works** end to end, and the library is read with `SHA1_OFF=1`. What
there is **not** is music: the audio decoder never gets to produce frames. With the
DSP ruled out, the image cluster ruled out and the sink identified, what remains is RE
of the decode loop — specifically why `0x081e93d8` extracts a single frame even though
the credit `+0x2b8` is written 18 times.

### 11.30 The decoder loop, narrowed down: the codec is asked for ZERO samples (and that is not the only cause)

Continuation of §11.29, now with the device stable and navigable. The last link —why
no music comes out— was chased down to a concrete number, and what is NOT the cause
was narrowed down.

**A) The frame extractor does have a loop and does pass its credit gate.**
`0x081e93d8`, after the gate, executes:

```
81e9428:  ldr r6, [obj+0x3c]        ; producer index
81e9434:  ldr r1, [obj+0x44]        ; samples requested
81e9438:  ldr r0, [reader+8]        ; output space
81e9440:  cmp r1, r0 ; movle r0, r1 ; r0 = min(...)
81e9448:  mov r3, r0                ; <- the COUNT the codec receives
81e9458:  bl  0x081e9e70            ; the codec math
81e945c:  sub r6, r6, #1
81e9468:  cmp [obj+0x40], r6 ; bne  ; iterates until it catches up with the consumer
```

Measured (`pullpass=1`): **it passes the credit**. That refutes the idea that the
credit `+0x2b8` was the wall, which was the hypothesis of §11.18/§11.25.

**B) The count is ZERO — measured live** (gdb at `0x081e9428`, with playback
started):

```
reader=0x093cbbd4  obj=0x0ba90c50
prod[+0x3c]=3  cons[+0x40]=2  depth=1     <- there IS queued work
[obj+0x44]=0   [reader+8]=0               <- but min(...) = 0
```

That is: **the codec is invoked asking it for zero samples**. That is why it "decodes
a frame" (`aacmath=1`) that produces nothing, and the mixer ring never fills.

**C) But forcing the count is NOT enough.** With a hook that writes 1024 into both
fields right before the loop (`count_probe.gdb`): `aacmath` stays at 1 and the audio
is still only UI clicks (170 s captured, 0.200 % of samples nonzero, all bursts of
1248/2496 with constant peak 11444). **There is another layer underneath**, so the
count is one more symptom, not the root cause.

**D) What is ruled out, with data.**
- **It is not the DSP.** Zero accesses to `0x39000000`/`0x3F000000` during playback.
- **It is not storage.** The disk is actively read: **30 666 writes to ATA registers
  from 29 different PCs** in one run, spread over the whole time, not just at boot.
- **It is not the credit** `+0x2b8` (§A).
- **It is not the ring that was being watched**: in real playback the copy reads from
  `0x0888a2xx-0x0888a6xx`, not from the boot render's `0x08889098`. Re-measured over
  the correct window: only the two IRAM memsets write.

**E) The engine's interrupt does close its cycle.** With `SM1_IRQ=1` (§11.29) the
firmware arms the interrupt on its own (`sm1arm=2`) and the render responds:
`rendret 1→4`, `dpump 5→8`, `copy16 2405→4808`. The output path works and feeds back
on itself; what does not arrive is content.

**STATUS.** The device is **navigable and stable**, its **native render pipeline
works and feeds back**, the library is read (`SHA1_OFF=1`), and storage serves data.
**There is no music**: between the compressed file and the mixer ring there is a link
that produces no samples, and it is no longer any of the usual suspects. The natural
next step is to follow the decoder's input —which object feeds `[obj+0x3c]` and where
it gets the compressed bytes from— instead of continuing to push from the output.


## 11.31 The iTunesDB signature: hypothesis raised and REFUTED by a control

It is worth writing down because it was plausible, it fit several measurements, and
the control experiment knocked it down cleanly.

**The hypothesis.** `run_play.sh` had carried since §11.28 the note that `SHA1_OFF=1`
was needed "because the engine, once modeled, rejects the hash of the image's
iTunesDB". Added to the fact that the disk's database came **unsigned**:

```
hashing_scheme @0x32 = 0
hash58        @0x58 = 0000...0000        <- 20 bytes of zeros
```

and that `iPod_Control/Device/SysInfo` declared `FirewireGuid: 0000000000000000`,
the natural reading was: the Classic 6G validates the hash58 signature derived from
the FireWire GUID, without a GUID there is no signature, without a signature retailOS
discards the library — and that was the "no songs" and, downstream, the codec's
`count = 0`.

**The "real" GUID that was deduced.** It is in the SysCfg block at the start of
`assets/classic6g/bootflash_nor_1MB.bin`, in 20-byte entries (4-byte tag + 16 of
data) with the tags stored reversed:

```
0x18 'SrNm' -> "8L745SDZYMV"
0x2c 'FwId' -> 01000000 | 17 8b 11 13 00 27 0a 00 | 00000000
0x40 'HwId' -> 85160282     0x58 'HwVr' -> 0x13 (19), the same one 0x0827a188 requires
0xa0 'Mod#' -> MB147
```

The 8 bytes of `FwId` in little-endian give **000A270013118B17**, and `00:0A:27` is
Apple's FireWire OUI. That was written into `SysInfo` and libgpod signed
(`hash58 = 116dcb48...`).

**The result: worse.** With the database signed, the player went to saying **"0
skladeb"** (0 songs) — and also with `SHA1_OFF=1`, meaning SHA-1 had nothing to do
with it.

**The control that settles it.** An intact copy was saved before touching anything,
and the device was booted with it:

| image | iTunesDB | GUID in SysInfo | songs on screen |
|---|---|---|---|
| original | **unsigned** | zeros | **4 skladeb** ✓ |
| rebuilt | signed, NOR GUID | 000A270013118B17 | 0 ✗ |

The original image **always read its library fine**. The signature was not the
problem: I introduced the problem myself when rewriting the database. And along the
way it is refuted that the missing NOR matters here — the firmware does not read
SysCfg (there is not a single `SCfg` or `FwId` string in the osos, nor a SysCfg block
anywhere on the disk).

**Also refuted, along the way:** that the firmware could not read the FAT because of
the sector size. The partition is in 4096 B units (22696 x 4096 lands exactly on the
FAT32 superblock) while QEMU's IDE core is 512 B, which raised the suspicion that the
firmware would look for the filesystem in the wrong place. But it reads 4 songs from
that FAT, so it finds it.

**What is established from §11.30, with the corrected reading:**

- **The output path works end to end.** DMAC0 ch1 moves `0x22008c5c` ->
  `0x3ca00010` (the I2S FIFO) with a single-node LLI that points to itself at
  `0x22010020`, 4 KB per lap, 16 bits, incrementing source, nonstop.
- **And real audio already comes out through it**: every capture carries ~15 000
  nonzero samples out of ~9 M, peak 11444, always in multiples of 1248 — they are the
  **wheel clicks**. Mixer -> render -> IRAM0 -> DMA -> FIFO -> WAV is proven.
- **The "nonzero samples" in the mixer ring** (`007fffff 006ba27e 005a827a
  004c1bf8`) are the decaying envelope of that click, not a song.

**THE REAL FAILURE, measured on the image that does have a library** (run `ctl1`):

```
StartPlayback=1  framepull=1  pullpass=1  pullbail=1  aacmath=1
copyent=201  copy16=3607  mixer=7  dpump=7  rendret=3
```

Playback **starts**, the frame pull **passes** the credit gate once, the codec runs
**once**... and that is the end of it. **It is a decoder that outputs a single frame
and stops**, not an unreadable library or a broken output.

## 11.32 The real knot: the library and SHA-1 are mutually exclusive

Four runs with the same image and a single variable changed leave the state
unambiguous:

| run | SHA-1 | iTunesDB | library on screen | playback | reboots |
|---|---|---|---|---|---|
| `ctl1` | OFF | original | **4 skladeb** | starts | - |
| `mj4`  | OFF | patched in place | **4 skladeb** | `StartPlayback=2` | 1 |
| `mj3`  | **ON** | patched in place | 0 songs | does not start | 0 |
| `sig1` | **ON** | rewritten and signed | 0 skladeb | does not start | 0 |

**With SHA-1 connected the firmware rejects the database; with SHA-1 off it accepts it
but the ~46 s panic comes back.** Run `rtc1` shows it at a glance: it starts entering
Music and a few seconds later it is back at the language selector.

That reinterprets §11.31 on an important point: **the original note in `run_play.sh`
was right**. The SHA-1 accelerator, once modeled, lets the firmware verify the
iTunesDB signature -- and this image's does not pass. It is not a failure of the
accelerator: it is a check that previously could not even run.

And it also reinterprets the "decoder that outputs one frame and stops": the runs
where that was seen had `SHA1_OFF=1`, that is, **the device was rebooting**.
`framepull=1` may simply be what it had time to do before the reset, not a codec
stall.

**What did NOT work for signing** (measured, not assumed):

- Rewriting the database with libgpod breaks it even with SHA-1 off: the original has
  EIGHT `mhsd` sections (types 1, 3, 2, 4, 8, 6, 10 and 5) and libgpod 0.8.3 does not
  keep them all. `mj1` -> "Pas de musique", and setting the hash to zeros does not fix
  it either (`nh1` -> "Keine Musik"). That is why `tools/ipod-library/patch_tracks.py`
  edits in place and rewrites nothing.
- Signing with the NOR GUID (`000A270013118B17`) is not enough either, and in fact
  the firmware **does not read SysCfg**: there is not a single `SCfg` or `FwId` string
  in the osos, nor a SysCfg block anywhere on the disk, nor a NOR region mapped in the
  machine.

**The path that remains is to measure the signature, not guess it.** SHA-1 is
modeled, so the engine itself sees which bytes the firmware gives it to digest and
which digest it produces; with that and the point where it compares, a signature that
passes can be built, without touching the firmware.

## 11.33 Two fidelity failures found along the way

**The RTC was not running.** With `PMUTRACE=1` you can see the firmware rereading in a
loop the PMU block `0x59..0x5f` -- seconds, minutes, hour, day of week, day, month,
year -- and the model answered **zero to everything**. A real seconds counter does not
stand still, and it showed on screen: Clock and Calendar displayed an absurd fixed
date. Now those registers are served in BCD from `qemu_get_timedate()`, the same
source that `hw/rtc/pl031.c` uses, so `-rtc` works here as in the rest of QEMU.

**The low battery screen.** retailOS raises "Batería baja -- Conecte la
alimentación" when entering playback, and that alone blocks the audio. It is measured
that **it does not come from the ADC channel this model answers**: with
`PMUBATT=1023` (full scale) the screen appears just the same. `PMUBATT` remains as an
environment variable precisely so it can keep being ruled out; the origin of the
verdict remains UNDETERMINED, although after fixing the RTC it did not appear again in
`rtc1`.

## 11.34 The test harness was lying, and why

Two failures of the instrumentation itself invalidated measurements and it is worth
not repeating them.

**Zombie instances.** The emulator is launched as `build/qmus_$TAG`, not as
`qemu-system-arm`, so the customary `pkill -x qemu-system-arm` **never killed
anything**. The old instances kept listening on `:4455`, and so `iremote.py` drove
and photographed **the previous machine** while the new one produced neither log nor
WAV. Fixed in `run_play.sh` and `run_nav.sh` with `pkill -9 -f 'build/qmus[_]'` --
with the bracket so it does not kill itself and with the path prefix so it does not
kill the script that invokes it.

**Premature screenshots.** The menu screenshot was taken while the database was still
loading, so the same intact image gave "4 skladeb" in one run and "Zádná" in another.
Any on-screen count is only valid after letting it load.

And from navigation, two measured things: the `q,e` wake pair **does not sum to zero
at the start of a list** (moving up from the first entry does nothing, so every retry
drags the cursor down -- that is how it ended up in Calendars instead of Songs); and
the Music submenu starts with **Cover Flow**, with Songs in **sixth** position, not
fourth.

## 11.35 The SHA-1 engine was hashing the EMPTY STRING (and was missing half the DRAM)

§11.32 left the knot posed as "the database does not pass the signature". **That was
false**, and the new trace `SHA1TRACE=1` shows it at a glance:

```
SHA1TRACE #1: buffered=64 bytes, trailer says 0, memory_mode=1 start=0x8ba718c0
  head: 00 00 00 00 00 00 00 00
  digest:da39a3ee5e6b4b0d3255bfef95601890afd80709
```

That digest is the SHA-1 **of the empty string**. Two chained failures, both ours:

**1) The message length.** The model was ported from the iPod Touch and deduced how
many bytes to hash by reading the last eight of the buffer, assuming the firmware
writes the SHA-1 padding with its bit-length trailer. The Classic's retailOS **does
not pad**: it delivers 64 and then 4096 bytes in memory mode and expects the hardware
to do it. Without a trailer, those eight bytes were zero, the length came out zero,
and the engine always answered the same thing. Now the length comes from **how the
message arrived**: in memory mode the guest has already stated it via `SHA_INSIZE`.
(A first attempt trusted the trailer "when it looked like a length" and read 34 of 64
bytes of data as if they were one — these messages look random, any octet passes for
one.)

**2) The uncached DRAM was not mapped.** The addresses are `0x8ba718c0` and
`0x8ba7192c`, that is `0x88000000 + 0x03a718c0`: the uncached view of `0x0ba718c0`.
With nothing mapped there, `cpu_physical_memory_read` returned **zeros**. The SoC
exposes the same DRAM twice and the code itself already took it for granted — the
DMAC0 note says the guest programs the channel with the cached alias while the CPU
runs on the other. Now the alias exists, and the engine reads real data
(`head: 67 23 fe 30 ...`).

**3) The 1 MB window is backed in both cases**, with the engine overlaid. Before it
was one or the other: with `SHA1_OFF` the whole megabyte was a stub and with the
engine wired only its `0x100` bytes of registers. The switch changed much more than
whether the digests were real, and that contaminates any A/B comparison.

**What this REFUTES.** The iTunesDB signature has nothing to do with this: the engine
does **two** hashes in the whole run, of 64 and 4096 bytes, and the content **is not
the database** (it changes between boots and carries no `mhbd` magic). All of
§11.31-11.32 pointed at a check that does not exist.

**The real state, measured:**

| SHA-1 | boot | library | playback |
|---|---|---|---|
| **active** | **clean**: no `CPU_exception_handler` and no reboot, `resets=0` | empty | does not start |
| `SHA1_OFF=1` | panic + reboot at ~46 s | 4 songs | starts, 1 frame |

With the engine fixed, **the faithful configuration (SHA-1 active) is the one that
boots clean**. But the library is still empty there, and no longer because of a
signature. Checked that it is not slowness either: leaving the device untouched, the
screenshots at 30, 120 and **240 seconds** are identical ("Ingen musik"). What the
firmware does with those two digests, and why the result leaves the library empty, is
what remains to be determined.

## 11.36 The real cut: the render copy runs off the end of IRAM1

With `SHA1_ZERO=1` (the engine present, but the digest answering zero as the
stub did) **playback starts**: `framepull=1`, `aacmath=1`, `pcmout=4`. So
what decides whether the library is accepted is the **value of the digest**, not whether the
device is there or not: with a real digest the firmware runs a check that
this image does not pass; with zeros it skips it. That leaves a single blocker for it to play: the
reset.

**And the reset has a single cause, measured.** QEMU's `-d int` log says that in
the whole run **exactly one** Data Abort is taken:

```
Taking exception 4 [Data Abort] on CPU 0
  ...with DFSR 0x8 DFAR 0x22040216
```

`0x22040216` is 0x216 bytes above the end of IRAM1 (which ends at
`0x22040000`). Captured at the abort vector, the culprit is unambiguous:

```
faulting insn at 0x08016f08
  0x8016f08:  strhne r3, [r2], #2
  r2=22040a16  r1=00000021  r4=0bb12d54  r5=0888ba74
```

It is **`copy16`**, the render's halfword copy loop, writing PCM into the
IRAM1 ring and running off the top.

**The overflow is BOUNDED.** By mapping RAM above IRAM1 (`IRAMSPILL`,
a measurement aid, not a fix) the run continues and **1178 writes** are counted
above the limit, all from that same PC: about 2.3 KB. It is not a runaway
leak; the destination pointer ends up a fixed amount too high. That is
the shape of a ring copy that should be split into two segments (tail up to
the end of the buffer and head from the start) and is done in one go.

**Why the order matters.** A note on method: the first write watch
on `0x22040000-0x22040800` gave **zero** and led to the conclusion that the abort was a
read. It was the window, which fell short: the pointer was already at `0x2204221x`.
Widened to `0x22060000`, the 1178 appeared.

**What remains.** Where that destination comes from: if the firmware computes it from a
register that this model answers wrongly (the natural candidate is the SM1 position, which
already produced a failure of this same family in §11.24), then it is our bug and it gets
fixed; if it computes it on its own, we need to see what makes it believe the ring is 2.3 KB
bigger than it is.

## 11.37 With the overflow survivable: the resets stop, the render doubles

Backing the region above IRAM1 (`IRAMSPILL=0x100000`) so that the
overflow of §11.36 does not abort, and with `SHA1_ZERO=1` so that the library loads:

| | before | now |
|---|---|---|
| `resets` | 1 | **0** |
| exceptions | 1 Data Abort | **none except IRQ** |
| `copy16` | 4 809 | **9 616** |
| `copyent` | 258 | **456** |
| `pcmout` | 4 | **8** |

That is: **that single abort was what cut the run short**, and without it the render
pipeline runs exactly twice as much. That confirms §11.36 from the other direction.

**But there is still no music.** The WAV of that run: 17 472 non-zero samples out of
10.8 M (0.161 %), peak 11444 -- the signature of the wheel clicks again. And
`framepull` is still at **1**.

**What that rules out.** The "decoder that outputs one frame and stops" **was not the
reset**. With no reset, no abort and the render running twice as much, the codec
still runs only once. They are two independent failures, and §11.32 had
lumped them into one.

What remains, therefore, is the decoder side: `credread=13` and `cred1=cred4=2` say that
the credit path is traversed, `pullpass=1` that the pull passed its gate once, and
`fpull2` (`0x081c1508`) is still at **0** -- that second pull point never
runs.

**And the credit is NOT zero.** Capturing the return of the read at `0x081e9bd0`, eleven
times in a row, always the same:

```
r0=0x00000004  r1=0x0ba95574  r2=0x2200ac64  r5=0x0ba90c98
```

Constant 4. So the theory of "it runs out of credit and waits" does not hold either:
the loop receives a stable value and even so **does not call `framepull` again**. The
`r2=0x2200ac64` is IRAM0, looking like a kernel object (semaphore or event), and the
value 4 looks more like a return code than a count.

**STATUS at the close of this session.** With `SHA1_ZERO=1` + `IRAMSPILL`: library
loaded (4 songs), playback started, **zero resets**, render running at
double... and only clicks in the WAV. The codec runs **once** and the flow goes into a
loop at `0x081e9bc0` that it does not leave. That, and not the reset, is what stands between
this emulator and the music.

## 11.38 The getter at 0x081e9bc0 is NOT the hang (refuted with the probe)

Reading the disassembly, `0x081e9bc0` turned out not to be a loop but a **getter** with
three distinguishable exits:

```
81e9bc0: bl 0x81c0fd4        credit -- measures 4, so this is NOT the exit
81e9bd8: ldr r1,[[r4]+0x48]  virtual slot
81e9be4: blx r1              -> r5 = next buffer
81e9bfc: ldrb r0,[r4,#0x30]
81e9c00: cmp r0,#3
81e9c04: beq 0x81e9c34       -> RETURNS NULL
81e9c08: cmp r5,#0
81e9c0c: beq 0x81e9bf0       -> NULL buffer: pumps 0x81e96d0 and retries
```

And `0x81e96d0` is a dispatcher on that same byte `[obj+0x30]`: states 0, 1 and 2
do work (`0x81e92cc`, `0x81e9580`, `0x81ea394`); with **3 it does nothing and returns**.
Hence the natural hypothesis: the object reaches state 3 ("finished") after one frame and
that is why there is no second one.

**Measured at `0x081e9bfc`, it is false.** The state **advances**:

```
slot#1  r5=0x0ba7564c  state=0        slot#5  r5=0x0ba7564c  state=1
slot#2  r5=0x0ba7564c  state=0        slot#6  r5=0x0ba7564c  state=1
slot#3  r5=0x0ba7564c  state=0        slot#7  r5=0x00000000  state=2  (retries)
slot#4  r5=0x00000000  state=1  (retries)   slot#8  r5=0x0ba7564c  state=2
```

0 -> 1 -> 2, with real buffers, and the NULLs are exactly the transitions where the
pumping does its work. It is the **startup sequence working**, not a hang.
It never reaches 3 in the measured window.

So "the codec runs once" is not here. It is still not located.

## 11.39 Full navigation test, with the library loaded

`run_nav.sh` with `SM1_IRQ=1 SHA1_ZERO=1 IRAMSPILL=0x100000`, presses verified
at every step (§11.34):

| step | result |
|---|---|
| language selector | passed |
| main menu | correct |
| enter Music | correct |
| go down to Songs (6th entry) | correct |
| **song list** | **Greatest Hits / Michael Jackson: Billie Jean 4:55, Beat It 4:58, Thriller 13:41, Smooth Criminal 9:25** |
| select song | enters the Now Playing animation |
| +8 s / +48 s | **stays on the cover**, does not reach the view with the progress bar |

Counters: `StartPlayback=1`, `framepull=3`, `pullpass=3`, **`pullbail=3`**,
`copy16=4808`, `resets=0`.

**Two things this establishes.** The first: the library, the titles, the artist,
the album and **the durations** are correct on screen, so
`tools/ipod-library/patch_tracks.py` does its job and the iTunesDB is read correctly. The
second: entering through Songs the frame pull runs **three** times instead of once
(through Shuffle it ran once), but **all three exit through `pullbail`** (`0x081e9470`), the
bail-out branch. Previously `pullbail` was not distinguished from `pullpass`; here they come
paired, 3 and 3.

That asymmetry -- one navigation path pulls more times than another, and all of them bail out --
is the concrete lead that remains: `0x081e9470` is the exit to follow.

## 11.40 The queue IS being fed -- and the window that denied it was misplaced

§11.39 left `framepull` passing its two gates and the loop at `0x081e9460`
running the codec once per unit of queue depth. And a note on method
that cost an entire measurement.

**The mistake.** `credread` was called with `0x0ba90c98`, and I read it as `obj+0x48` of an
object at `0x0ba90c50`. False: `0x0ba90c98` **is** the object (`[r4+4]`). Watching
`0x0ba90c88-0x0ba90c98` only **eight** writes appeared, all at boot,
from PCs with `bic #0xC0000003`, block splitting and statistics counters --
that is, **memory allocator headers**, not an audio queue. The conclusion
"the producer never enqueues" was an artifact of looking 0x40 bytes lower than
it should have been.

**With the correct window** (`0x0ba90cd0-0x0ba90ce0`, i.e. `obj+0x38..obj+0x48`) the
queue is alive throughout the whole playback:

```
0x081c0940 / 0x081c0944  ->  +0x38 / +0x3c   (producer side)  -- repeatedly
0x081e9f48 / 0x081e9f50  ->  +0x40 / +0x44   (after the codec)
0x081e9f68 / 0x081e9f88  ->  +0x40 / +0x44   (another path, later)
```

With timestamps at 65.32 s, 65.33 s, 80.76 s, 80.84 s... The producer runs, the consumer
updates, and even so no music comes out.

**Lesson on method, the third of this kind.** A misplaced watch window
does not give an error: it gives ZERO, and the zero looks like a finding. It already happened with
`0x22040000-0x22040800` (§11.36), where the silence led to the conclusion that the abort was a
read when the pointer was already at `0x2204221x`. Before believing a "nobody writes
here", the base address has to be confirmed with an independent read.

## 11.41 The cause of everything being extremely slow: an interrupt STORM

Following the chain from the codec upward, disassembly in hand:

- `framepull` (`0x081e93d8`) passes its two gates; its loop at `0x081e9460` runs
  the codec **once per unit of queue depth** ("while `[obj+0x40]` != r6",
  r6 from `[obj+0x3c]` counting down). Measured prod=3 cons=2 -> depth 1 -> one call.
  That explains `aacmath=1` **without needing any failure in the codec**.
- The depth is set by `0x81c08bc` from two fields: `[obj+0x2b4]` (position)
  and `[obj+0x2b8]` (limit). Measured: limit **4**, position **0**.
- The position is not dead: it advances 0 -> 1 -> 2 -> 3, one at a time
  (`adds r2,r1,r2` at `0x81cc404`, with a reset to zero if it goes negative). It just
  advances **three times in the whole run**.

**Everything is about four orders of magnitude too slow. And here is why:**

```
credithdlr (line 18 IRQ handler, 0x08017378)       = 1 196 485
creditsig  (where it signals the event, 0x080173cc) =       490
dpump      (render pump)                            =         8
SM1_IRQ: line 18 raised                             =         4
```

The model raises the line **four times** and the firmware enters the handler **1.2
million** times. QEMU's `-d int` log confirms it another way: ~1.05 million
IRQ exceptions. The line stays asserted for the 5 ms until the next tick
and the guest re-enters non-stop throughout that window, starving the audio
threads. That is why `dpump` runs 8 times while the handler runs a million.

**Partial fix, measured.** The acknowledge-on-read was limited to `+0xa98`
because the handler's getter (`0x080a7638`) reads that register. It is not enough: widened to
**any** read of the SM1 block, the entries drop from 1 196 485 to **789 182**
(-34 %). It is still a storm, so the line is being held or re-dispatched by something
that is not our raise -- the VIC model, or a second user of line
18. That is the live lead.

**What this reorders.** The "codec that runs once" is not a failure of the codec or of
the queue: it is the arithmetic consequence of a depth of 1, which in turn comes from
a position that barely advances because the system is starved. There is **one** problem,
not three.

## 11.42 The storm, solved: from 1 196 485 entries to 2 399

§11.41 left the storm measured but alive. The shape of the line was the problem, and
two experiments narrow it down:

**A pulse does not work.** With `SM1_IRQ_PULSE=1` (raise and lower at the same instant) the
handler is entered **zero** times and `creditsig` also falls to zero: the VIC wants
**level**, an edge is lost. A clean fact, and it rules out the obvious solution.

**What was excessive was the time.** The line was held until the next 5 ms
tick, and ~197 000 re-entries per raise are only possible because virtual time
barely advances while the CPU spins in the interrupt loop. A coarse
timer is exactly what should not hold an interrupt line. Bounding the
window with a dedicated one-shot:

| window | handler entries | `creditsig` | `dpump` | `copy16` |
|---|---|---|---|---|
| until the 5 ms tick | 1 196 485 | 490 | 8 | 4 808 |
| same + ack on any read | 789 182 | 493 | 8 | 4 808 |
| **250 us** | 43 750 | 482 | 8 | 4 808 |
| **20 us** | **2 399** | **2 399** | **16** | **9 616** |

**500 times fewer**, and the detail that matters: at 20 us `creditsig == credithdlr`,
i.e. **every interrupt does useful work**, versus 490 productive ones out of 1.2 million.
The render pipeline doubles again: `dpump` 8 -> 16, `rendret` 4 -> 8, `pcmout`
4 -> 8, `copy16` 4 808 -> 9 616.

`S5L8702_SM1_IRQ_ASSERT_US` is a **modeling choice**, marked as such: what
the real engine's line does is UNDETERMINED, and what the firmware touches to
acknowledge it has not been found -- the handler's getter (`0x080a7638`) does not seem
to reach this block, or else widening the acknowledge to any read would have ended
the storm instead of cutting it by a third.

**And there is still no music.** `aacmath` and `framepull` are still at **1**, and the WAV
still carries only clicks (18 720 non-zero samples, peak 11444). With the starvation removed,
the decode queue still has depth 1. The storm was a real cause of
slowness -- 500 times -- but it was not the only one separating this emulator from the music.

## 11.43 What remains, narrowed down: the queue still has depth 1

With the storm solved (§11.42) the system is no longer starved, and the question
narrows. Measured after the fix:

- `credithdlr` = `creditsig` = **2 399**: every engine interrupt signals its event.
- `dpump` = **16**, `copy16` = **9 616**: the render responds and has doubled.
- `aacmath` = `framepull` = **1**, WAV with 18 720 non-zero samples out of 12.9 M -- clicks.

And the quantity that governs everything is unchanged: sampling `0x081c08b4` throughout
the run (hits 1, 8, 32 -- the getter is only called a few dozen times),
`pos[+0x2b4]` = **0** and `limit[+0x2b8]` = **4**, unchanging. The position does advance
--measured at `0x81cc404`, one at a time-- but **three times in the whole run**.

**Correction of a label.** `framepull` (`0x081e93d8`) is **not** the decode
pump. It is called from `blx r2` at `0x081ea384` --a virtual call-- and
only when `byte[r4+0x1c]` **changes state**. That it is 1 is what is expected: it is an
edge notification. Chasing it as if it were a decode loop was another
assumption of mine baked into a name.

**The next thread, concrete:** 2 399 event signals produce 16 iterations of the
render pump. Between `creditsig` (`0x080173cc`, which signals event `0x22008c58`) and
`dpump` (`0x22006118`) there is an unexplained factor of 150. There, and in what actually
increments `[obj+0x2b4]`, lies what is missing.

## 11.44 Correction: "four line raises" was a bad count

Sections 11.41-11.43 cite "4 line raises" versus 1.2 million
handler entries, and from that comes the figure of ~197 000 re-entries per raise. **The
count was wrong.** The model's `printf` is filtered:

```c
if (s->irqs <= 4 || s->irqs % 2000 == 0)
```

so counting log lines does not count raises. The real numbers:

| run | last printed line | real raises | handler entries |
|---|---|---|---|
| `pi1` (before) | `raised #4` | **< 2000** | 1 196 485 |
| `bd2` (20 us) | `raised #2000` | **2000-4000** | 2 399 |

**The conclusion not only holds: it improves.** After the fix there is ~one
handler entry **per raise**, which is exactly what it should be. And it explains what previously
did not add up: if the model ticks every 5 ms and raises every fifth tick, in 100 s it should
raise ~4000 times -- but before the fix it only reached 4. **The storm froze
virtual time**, so the engine's own timer barely ran. Cause and
effect fed back into each other.

The I2S position confirms it another way: between the first raise and number
2000, `pos` goes from 3 251 200 to 5 453 824 stereo frames, i.e. **50 seconds of audio
clocked out** in that part of the run. The output path runs in real time.

**What still does not add up, and it is the live thread:** ~2400 interrupts signal their
event and the render pump (`dpump`) runs **16** times. The render moves 9 616
halfwords -- about 19 KB -- where 100 s of audio needs 17.6 MB. It is at
0.1 % of the required rate while the DMA drains silence in real time.

## 11.45 The whole chain, with a number on every link

With the storm solved and the counts corrected, the complete path is measured
end to end in a single run (`pc1`):

| link | measurement | healthy? |
|---|---|---|
| I2S / DMA outputting samples | 50 s of audio clocked out in the measured window | **yes, real time** |
| SM1 engine interrupt | 2000-4000 raises, ~1 handler entry each | **yes** (§11.42) |
| `creditsig` signaling event `0x22008c58` | **1 822** | **yes** |
| **`dpump`, the render pump (`0x22006118`)** | **16** | **NO -- this is where the cut is** |
| `posfn` (`0x081cc360`, advances the position) | 12, all from `0x081f6e94` | consequence |
| `posadd` (`0x081cc404`, the increment) | 6 | consequence |
| queue depth | 1 | consequence |
| codec (`aacmath`) | 1 | consequence |
| WAV | 18 720 non-zero samples out of 12.9 M -- clicks | consequence |

`posfn` has exactly **two** callers in the whole image, `0x081d7874` and
`0x081f6e94`; the first **never runs** and the second runs 12 times.

**The cut is a factor of ~114 between `creditsig` and `dpump`.** The engine signals its event
1 822 times and the pump waiting on it starts 16 times. Everything that comes after --
position, queue depth, codec calls, samples -- follows from that by
arithmetic. There is no longer any need to look in the codec, the queue, or the output: all three
are measured and are consequences.

**Where to look:** what `0x080a1a24` does with object `0x22008c58` (the signal), versus
what exactly the pump at `0x22006118` waits on. If they are different objects, or if the
semantics of the RTXC event is "one-shot" and gets lost while the waiter
is not blocked, that is where the explanation lies.

## 11.46 Line 18 is CONFIRMED by the firmware itself

`S5L8702_SM1_IRQ = 18` was chosen because Rockbox's table has a **gap** at 18,
and that was never proof of anything. Now it is. At render startup, in IRAM:

```
220069c4:  mov r0,#8       ; bl 0x22003870    creates the object
220069cc:  mov r1,#18      ; bl 0x22003888    IRQ number: 18
220069d4:  ldr r1,=0x08017378 ; bl 0x22003890 registers THAT handler
220069e4:  ldr r1,=0x08017378 ; bl 0x22003898 enables it
220069ec:  ldr r0,=0x22008c58 ; bl 0x220037f8 creates the event
```

The firmware registers `0x08017378` -- the handler we have been counting -- on
**IRQ 18**, and then creates the event `0x22008c58` that this handler signals.
The line choice was correct, and no longer by elimination.

So only one open question remains along the whole path: **who waits on that
event and why it starts 16 times when it is signaled 1 822 times** (§11.45). The object is
passed by pointer, so the waiter cannot be located by searching for the literal
`0x22008c58`; you have to follow the handle returned by `0x220037f8`.

## 11.47 Who waits on the event -- and bit 12, refuted

Following the literal `0x22008c58` outside IRAM, the waiter turns up at
`0x0808af58`:

```
808af6c:  bl 0x80a7638          reads the SAME status as the IRQ handler
808af74:  tst r0, #0x1000       checks BIT 12
808af78:  beq 0x808afa0         if not set: skips the processing block
808af7c:  ... bl 0x80a35a0 ; bl 0x809a72c(1)      <- per-frame work
808afb0:  ldr r0,=0x22008c58 ; mov r1,#500
808afb8:  bl 0x80bf2b8          waits on the event, timeout 500
```

Two consumers of the same status register with **different bits**: the line 18
handler looks at **bit 2**, this other one at **bit 12**. The model only answered
bit 2, so that per-frame work block never ran.

**It looked like the missing piece. Measured, it is not.** Turning bit 12 on while there is
a pending frame: `dpump` 16 -> **14**, `aacmath` stays at **1**. No improvement.
So bit 12 means something else, and it stays **off by default** -- inventing a
state that the part may never report is exactly what must not be done. It is
kept as a switch (`SM1_FRAME_BIT=1`) because the finding -- that this waiter
conditions on bit 12 while the handler conditions on bit 2 -- is real and
it is worth being able to test it again.

It is also clear that the wait has a **timeout of 500**, so it is not an eternal
block: the waiter wakes up on its own even if nobody signals. That rules out the cut
being a deadlock at that point.

## 11.48 Correction: the cut is not at the event, it is above it

§11.45 placed the cut at the ~114 factor between `creditsig` (1 822) and `dpump` (16), and
§11.47 went looking for the event's semantics. **Both pointed at the wrong
place.**

The waiter (`0x0808af58`) is invoked through a trampoline in IRAM
(`0x22003850`: `ldr pc,[pc,#-4]` followed by the address), so it can be counted:

```
waitcall  (trampoline 0x22003850) =    8
waitfn    (0x0808af58)            =    8
dowait    (0x0808afb0, the wait)  =    8
bit12set  (bit 12 branch)         =    0
creditsig (event signals)         = 1841
dpump                             =   16
```

**The waiter is only CALLED eight times.** It is not that the wait fails or that
signals get lost: the 1 841 signals were never going to be consumed because **nobody was
waiting for them**. Looking for the RTXC event's semantics would have been wasted time.

The cut is further up: **the loop of the task that should pump audio spins eight
times in the whole run**. Everything downstream -- the wait, the event, the
position, the queue depth, the codec, the samples -- is a consequence of that.

And this explains why none of the downstream improvements moved `aacmath`: the
storm (§11.42) let the system breathe and the render doubled, but it did not change
how many times that loop spins.

**Where to look now:** who calls the trampoline `0x22003850` and why its loop ends
after eight iterations. It is an RTXC task; the pattern to look for is a loop
exit condition, not a block -- the wait inside has a timeout of 500 and therefore
never hangs indefinitely (§11.47).

## 11.49 The wait does NOT fail: it always succeeds. The target moves up again

§11.48 left the waiter invoked only eight times, and the loop that calls it seemed
to explain it:

```
22006324:  bl 0x22003850      calls the waiter
22006328:  cmp r0, #0
2200632c:  beq 0x22006310     returned 0 = failure: sleep and retry
22006310:  mov r0,#10 ; bl ... sleep
2200631c:  cmp r7, #1
22006320:  bgt 0x220061fc     after TWO attempts, give up
```

From there came the natural hypothesis: the wait (timeout 500) expires, and the loop gives up
after two retries. **Measured, it is false.**

```
loopcall (0x22006324, the call)            = 8
waitok   (0x22006330, the SUCCESS branch)  = 8
```

`waitok == loopcall`: **the wait succeeds all eight times.** The retry branch
is never taken. And raising the engine's cadence to `SM1_IRQ_EVERY=1` --
`creditsig` from 1 841 to **8 180** -- moves neither `dpump` nor `aacmath`. The cadence is not
the driver of anything here.

**So the target moves up again.** The loop does not give up: simply, **the function
that contains it only executes eight times**. `dpump` (`0x22006118`) runs 16 times and
inside it this wait happens 8 times, always successfully. The live question is who
calls `0x22006118` and why only sixteen times.

**Method warning, the same one again.** I read the retry `beq` as the usual path
without checking it. Counting the success branch cost one run and would have avoided
the entire timeout theory. Before reasoning about which branch a jump takes, you have to
count it.

## 11.50 The audio task is scheduled every ~20 SECONDS

`0x22006118` does not appear as a literal or as a `bl` target anywhere: it is
called through a pointer built at runtime, so the disassembly doesn't
find it. Captured live with `regpc`, the caller is **unique**:

```
lr=0x22002ff8   r0=0x220106dc  r1=0x22006118  r5=0x22008c10
```

`r1` is the pump's own address passed as an argument: `0x22002ff8` is an RTXC
**task dispatcher**. And the timings are what matter:

```
+80.597 s  +80.760 s  +80.804 s  +80.814 s  +80.864 s  +80.874 s  +80.925 s  +80.935 s
+98.428 s  ...                                          <- 17.5 s gap
+119.650 s ...                                          <- 21 s gap
```

Bursts of pairs ~10 ms apart, and between bursts **17 to 21 seconds**. The
task that should pump audio is scheduled about three times per run instead of about
forty times per second. That ~1000 factor is of the same order as the audio
deficit (19 KB moved where 17.6 MB would be needed), so it is the explanation, not one more
symptom.

**Ruled out along the way: audio pacing.** `AUDIOPACE` gates DMAC0's request
line with a timer at the sample rate, so it was suspected of
strangling the chain. Measured the other way round: **without** it everything gets worse (`dpump` 16 -> 4,
`copy16` 9 616 -> 2 404). It helps, and it must stay on.

**The thread, now narrowed down to a scheduling question:** why the dispatcher
`0x22002ff8` lets 20 seconds pass between activations of this task. The natural
candidates are the kernel tick -- the timer's 1 kHz channel (ECLK/2,
DATA0 = 6000) -- and any `sleep` the task requests in ticks: if one of our ticks
lasts a thousand times what it should, a 10 ms nap becomes 20 seconds.

## 11.51 The timer is fine: the tick hypothesis, refuted

§11.50 left as the main candidate a kernel tick that is too slow -- if a tick
lasted a thousand times what it should, the audio task's 10 ms nap would become
the 20 seconds measured. `TIMERTRACE=1` settles it:

```
chan A   freq=6000000 Hz  data0=6000  pre=0   -> period 1.000 ms
chan B   freq=6000000 Hz  data0=6000  pre=0   -> period 1.000 ms
chan E   freq=1000000 Hz  (free-running, 32 bits) -> the microsecond counter
"selects PCLK" warnings: 0
```

The kernel tick runs at **exactly 1 kHz**, which is what it should, and the
microsecond counter at 1 MHz. And **no** channel selects PCLK, so the
ECLK-for-PCLK substitution documented by `chan_freq` -- the unmodeled part -- never
applies on this path. The timer model is not the problem.

**Tally of what has been ruled out, all by measurement:** the codec, the decode
queue, the output path, the iTunesDB signature, status bit 12, the credit, the
event semantics, the wait timeout, the SM1 engine cadence, audio
pacing and now the system clock.

**What is left standing:** the audio task is activated in bursts 17-21
seconds apart (§11.50), and the measured instants -- 80.9 s, 98.4 s, 119.6 s -- fall within
the harness's navigation script, which sleeps 6, 15 and 15 seconds between key presses.
That suggests the task is being woken **when something in the interface touches it**, not
on an audio clock of its own. Checking it is the next measurement: a run that
gets to playback and then doesn't touch any key, to see whether the bursts keep
appearing or stop.

## 11.52 CONFIRMED: the audio task has no clock of its own

The suspicion of §11.50 -- that the bursts coincided with the harness's key presses --
is checked directly with `run_quiet.sh`: get to playback and then **not
send a single message through the monitor** for two minutes (not even a
screenshot, which is a round trip to the monitor).

```
                        with interface activity    after 120 s WITHOUT TOUCHING ANYTHING
dpump                            16                            8
copy16                        9 616                        4 808
aacmath                           1                            1
```

**Two minutes of silence produce not a single extra activation.** The render pump does
not clock itself: every time it runs it is because something in the interface woke it. In a
real player that task has to spin about forty times per second with the
device sitting still in your pocket.

**What this means.** The audio is not waiting for data, nor credit, nor an
engine interrupt -- all of that is measured and healthy (§11.45, §11.42). It is waiting
for **a wake-up that only arrives as a side effect of interface activity**. The
clock of its own that playback should have does not exist in this emulation, or does not reach
that task.

**Where to continue, with what is already known:** the dispatcher is `0x22002ff8` and it passes
`0x22006118` in r1 with the object `0x220106dc` in r0. We need to see which RTXC queue or
timer feeds that dispatcher, and why it only moves when the
guest services the wheel or the LCD. The system timer is NOT it (§11.51: exact 1 kHz
tick), so the candidate is the task queuing mechanism itself.

## 11.53 Nor is it the DMA: the ISR runs 7 185 times

Having confirmed that the audio task does not clock itself (§11.52), the natural candidate was
DMAC0's end-of-transfer interrupt: the audio channel uses an LLI that
points to itself, and if the PL080 model did not fire the interrupt on every reload, the
task would never receive its "buffer consumed".

**Measured, it arrives in abundance.** The ISR registered on IRQ 16 (`0x22002810`):

```
dmairq    (0x22002810, DMAC0 ISR)      = 7 185
creditsig (SM1 engine event)           =   394
dpump     (render pump)                =     8
```

Seven thousand DMA interrupts and almost four hundred engine signals, and the pump
runs eight times. **Neither of the two periodic sources wakes that task.**

**The inventory of clock sources is exhausted:** the system timer is
correct (§11.51), the SM1 engine interrupt arrives and is serviced (§11.42), the
DMA interrupt arrives 7 185 times, and the I2S pacing helps rather than
hinders (§11.50). The task simply does not hang off any of them in this
emulation.

**What remains to be done, and did not fit in this session:** follow the dispatcher
`0x22002ff8` backwards -- which RTXC queue it reads, who fills it -- instead of continuing to
test interrupt sources one by one. The four that exist are already
ruled out by measurement, so the failure is in the queuing, not in the signal.

## 11.54 The whole chain, from the speaker to the job queue

Status at the close of the session, with each link measured:

```
[TOP]     who queues jobs           0x220067b8 / 0x220067fc   4 times each  <-- HERE
              |  (count++ at 0x22008c1c, then signal to 0x220106d0)
          job queue                 0x22008c1c                8 items
              |
          dispatcher                0x22002fe0..0x2200302c    "while count>0" loop
              |  (bl 0x22005dec, which jumps to the pump)
          render pump               0x22006118                8 iterations
              |
          event wait                0x0808af58                8 calls, 8 SUCCESSES
              |
          position / depth          [obj+0x2b4]=0 [obj+0x2b8]=4   depth 1
              |
          codec                     0x081e9e70                1 time
              |
[BOTTOM]  WAV                       only clicks
```

The dispatcher is a `while (count > 0) { procesar; count-- }` loop over
`[r5+0x0c]` with `r5 = 0x22008c10`, i.e. the count at **`0x22008c1c`** -- the same
counter a previous session had already pointed out. Watching that word yields
exactly four bursts, two enqueues and two dequeues each: **eight items,
eight pump iterations**. It adds up perfectly and nothing is left to explain downstream.

**Everything below is measured and healthy or ruled out:** the codec, the decode
queue, the event, the wait, the output path, the DMA (its ISR runs 7 185
times), the SM1 engine, the system timer and the I2S pacing. **The failure
is at the very top: nobody queues audio work except four times.**

**Where to get in next time:** `0x220067b8` is in a routine that first clears
a row of fields (`[r4+0x2c]`..`[r4+0x80]`, `[r4+0x8c]`) -- it has the shape of
a start/reset, not of periodic pumping. `0x220067fc` also increments a counter
of its own at `[r0+0x68]`. Both end up signaling `0x220106d0`. What needs to be found out
is what should call them forty times per second during playback, and doesn't.

## 11.55 One more link: the one queuing is another task, and it does not spin either

The function that queues is `0x22006574` (prologue `push {r4,r5,r6,r7,r8,lr}`, epilogue at
`0x220067cc`). It does not appear as a `bl` target in the disassembly -- another call through
a pointer -- so captured live:

```
lr=0x2200591c  r0=0x220106dc  r1=0x093b34c0  r3=0,1,2,3,5,6,7  sp=0x093b33d0
   8 calls, all from the same site
```

- `r0 = 0x220106dc` is **the same audio object** that the render pump receives.
- `r1` is a message, and `r3` its sequence number: 0, 1, 2, 3, **5**, 6, 7 -- number 4
  is missing, so one message was posted by another route or was lost.
- `sp = 0x093b33d0` is in SDRAM: **it is another task**, not the dispatcher (which runs on
  an IRAM stack).

In other words, whoever produces audio work is itself a task that only runs
eight times. The pattern repeats one level higher and the session ends here.

**Summary of the state for picking it up again.** The complete path, with a number measured at each
link, is in §11.54. The entry point is `0x22005918` -- the site that calls the
enqueuer -- and the question is still the same at every level: **what should be
spinning forty times per second during playback and isn't**. All the
hardware clock sources are ruled out by measurement (§11.51, §11.53), so
what is missing is on the software side: a task, or a chain of tasks, that is not
being rescheduled.

## 11.56 The chain, complete from the WAV to the firmware

Traced bottom-up, with a number measured at each link and the caller captured
live where the disassembly was not enough (several calls are through pointers):

```
0x081fef28   send site in the firmware              3 times (+1 at startup 0x081febe4)
     |       r0=0x0bae8ad8  r2=0x7fffffff
0x081d0944   builds the message (372-byte frame)    4 times
     |       bl 0x8015b18 -> veneer to IRAM
0x22005674   posts to the queue                     4 times
     |       bl 0x22005d04 from 0x22005918
0x22006574   enqueuer                               8 times
     |       count++ at 0x22008c1c, signal to 0x220106d0
0x22008c1c   job queue                              8 items
     |
0x22002fe0   dispatcher  while(count>0){...count--}
     |
0x22006118   render pump                            8 iterations
     |
0x0808af58   wait on event 0x22008c58               8 calls, 8 SUCCESSES
     |
[obj+0x2b4]=0 [obj+0x2b8]=4                         queue depth 1
     |
0x081e9e70   codec                                  1 time
     |
WAV                                                 only clicks
```

**Every link below the send is correct and consistent**: the arithmetic adds up
at each step (4 sends -> 8 items -> 8 iterations -> depth 1 -> 1 call to the
codec), and the hardware clock sources are ruled out one by one by measurement
(§11.51 timer, §11.53 DMA, §11.42 SM1 engine, §11.50 I2S pacing).

**The failure is at the top: `0x081fef28` sends three jobs in the whole run** where
a player needs about forty per second. `r2 = 0x7fffffff` on every call
looks like "no limit/no deadline". The next step is to understand what governs that
site -- what condition or what event should make it repeat -- and at that point you are in
player code, not kernel plumbing.

**Note on `IRAMSPILL`.** All these measurements were taken with that aid enabled, which
backs memory above IRAM1 so that the `copy16` overflow (§11.36) does not
abort. **It is not a fix** and must not stay: without it the run gets cut off at
~46 s and none of this can be measured. The overflow remains an open bug
in its own right.

## 11.57 Session conclusion: there is no broken link, the loop does not start

`0x081fef28` turns out to be `blx r3` with `r3 = [[r4]+0xc8]`: a **virtual** call, the
third in a sequence (+0xd4, +0xcc, +0xc8) shaped like "process a buffer". And the
function containing it runs, like everything else, three or four times.

That is the pattern that closes the diagnosis. Walking the chain of §11.56 from top to
bottom, **each level runs a handful of times and the arithmetic between levels always
adds up**: 3-4 sends, 4 messages, 8 items, 8 pump iterations, depth 1, one
call to the codec. There is no place where N goes in and much less than N comes out.

**That is: there is no broken link. What does not happen is the playback loop
starting over.** It starts, processes a few buffers and stays still. Looking for the
fault by walking down the chain --which is what this session has been spent on-- was doomed:
each level faithfully reflected the one above.

**What this suggests for next time.** Instead of continuing up through callers,
the useful question is what ends or suspends playback right after it starts. A
candidate with prior evidence: the player showed the **low battery** screen
(§11.33) and the "Now Playing" screen stays on the cover animation without reaching the
view with the progress bar (§11.39). A "stopped" state would explain both the
handful of buffers and the frozen UI, and it is checked by looking at the player's state
on screen or in its object, not by chasing more counters.

**State of the tree at the close.** Six verified fixes, on the branch's `master`:
IRQ storm (§11.42), SHA-1 engine (§11.35), uncached DRAM alias (§11.35),
1 MB window of the SHA-1 block, PMU RTC (§11.33) and the `copy16` data abort
located (§11.36, not fixed: `IRAMSPILL` only makes it survivable for measuring).
New instruments: `SHA1TRACE`, `SHA1_ZERO`, `PMUTRACE`, `PMUBATT`, `TIMERTRACE`,
`QEMU_DEBUG`, `IRAMSPILL`, `SM1_IRQ_EVERY`, `SM1_IRQ_PULSE`, `SM1_FRAME_BIT`, and the
probes `abort_probe`, `limit_probe`, `posinc_probe`, `slot_probe`, `dma_probe`,
`dma2_probe`, plus `run_nav.sh`, `run_quiet.sh` and `run_settle.sh`.

## 11.58 CORRECTION of §11.57: the output loop DOES turn, 7 500 times

§11.57 concluded "the loop does not start". Measured directly with a new trace in
the PL080 (`DMATRACE=1`, which counts what is programmed on each channel and how many
terminal-counts each one raises), that is **false for the output side**:

```
dmac0 ch1 ENABLE conf=0x00008a81 flow=1 srcid=0 dstid=10
          src=0x22008c5c  dst=0x3ca00010  lli=0x22010020  xsize=2048
dmac0 ch1 TC #7500 srcid=0 dstid=10
```

`dstid=10` is `IIS0_TX`; `0x3ca00010` is the I2S FIFO. The channel drains a
**4 KB double buffer in IRAM0** (0x22008c5c / 0x22009c5c) into the FIFO and
raises more than **7 500** terminal-counts per run. That is ~15 million
samples delivered, which is exactly the size of the WAV (32 MB). The consumer
is healthy and runs at the right tempo.

**Why the earlier dismissal of the DMA was invalid.** §11.53 declared the DMA fine
because "its ISR runs 7 185 times". A global handler count does not say which
channel the interrupts come from: they could all have been from the display or the ATA. Now it is
known that they come from the audio channel, but that had to be measured, not assumed. The
lesson repeats itself in this investigation: **an aggregate counter does not answer a
question about a specific component.**

**And a detail that reorders the problem.** In the `dma1` run playback did not
even start (`StartPlayback hits=0`, `dpump=0`, `aacmath=0`) and even
so the channel delivered 7 500 buffers. That is, **that channel is a silence pump
that runs from boot**, regardless of whether there is playback or not.

**Where the fault lies, then.** Not in the loop not turning, but in **nobody
filling with audio the IRAM0 buffers that the DMA drains**. And the exact place was already
noted in the header of `run_play.sh`: in the codec loop

```
81e9434: ldr r1, [r0, #0x44]     ; r0 = obj2
81e9438: ldr r0, [r4, #8]
81e9440: cmp r1, r0
81e9444: movle r0, r1            ; r0 = min(...)
81e9448: mov r3, r0              ; 3rd argument = sample count
81e9458: bl 0x81e9e70            ; the codec
```

the codec **is called, but with `r3 = min([obj2+0x44], [r4+8])`, and both fields
are measured at ZERO**. It is asked to produce zero samples. So the question is not
who consumes or when, but **who should fill those two counters**.

## 11.59 REFUTED: the MP3 format is not the cause

The four files the database points to were written by `patch_tracks.py` with a
plain `shutil.copyfile`: it does not check that the source format matches the
destination's extension, nor does it touch the record's type field. And the downloaded
originals carry a **682 KB ID3v2.4** tag up front with embedded cover art.
Two plausible reasons for the decoder to output nothing.

Test: the four rewritten with `ffmpeg -map_metadata -1 -id3v2_version 0`,
MP3 CBR 128 kbps 44.1 kHz stereo, **with no tag at all** — they start directly at
`fffb9064`, a bare MPEG-1 Layer III frame. Installed at the same paths,
with the sizes and durations patched in the database.

Result: playback starts (`StartPlayback`, `aacmath`, `framepull`,
`copy16` all active) and the WAV still contains **only clicks**: 33 696 non-zero
samples out of 26 122 218, peak 11444, and all the per-segment counts are exact
multiples of 1248. That 1248 and that peak are the signature of the wheel click. **The
file format is not the cause.**

## 11.60 Two test bench traps that cost three runs

**`IRAMSPILL` is a number of BYTES, not a boolean.** `IRAMSPILL=1` maps a single
byte and the run resets just the same, with the same `copy16` data abort. The
useful value is `0x10000`. The boot message says so —"IRAMSPILL: 1 bytes mapped"—
but it goes unnoticed amid the noise.

**The verification region matters when pressing SELECT.** `iremote.py press -u`
repeats the press until the screen changes by more than a threshold. Entering a
submenu only redraws the LEFT LIST: the right panel keeps showing
the same icon and "4 Songs". Verifying with `--region right` the diff comes out 0, it
retries fifteen times, and the extra presses end up in Cover Flow. For
list transitions `--region left` must be used.

**And the reminder that costs the most.** With the SHA-1 accelerator active the
library comes out EMPTY ("No Music" / "No Songs") — it is the knot of §11.35, not a
regression. Every run that must play needs `SHA1_ZERO=1`. Three runs
in a row were interpreted as "playback does not start" when what was happening
is that there was no library to play.

The menu, verified on screen and in English: **Music, Videos, Photos, Podcasts,
Extras, Settings, Shuffle Songs** — Music is already highlighted on accepting the language,
no scrolling needed. Inside: **Cover Flow, Playlists, Artists, Albums,
Compilations, Songs**, then Genres, Composers, Audiobooks — Songs is five down.

## 11.61 Label CORRECTION: `0x081e9e70` is not a codec

All the earlier work calls it `aacmath` and treats it as the decoder, and
from that comes the sentence "the codec is called but with a zero count". Having read the disassembly,
that does not hold:

```
81e9e70: ...
81e9e94: bl 0x81c0c50          ; gets an object
81e9ed8: bl 0x80f8ba8          ; <-- "next" of a list
81e9ec4: cmp r0, r6            ; each element's [+4] against the two values
81e9ecc: cmp r0, r7
81e9ef0: bl 0x80f8ab4          ; acts on the one that matches
```

`0x80f8ba8` is a linked list iterator with no ambiguity at all: cursor at
`[obj+0x30]`, head at `[obj+0x28]`, it advances through the `next` pointer and returns
`cursor+8`. And `0x80f8ab4` walks an array of function pointers
(`[r5+4] + i*4`, calling `0x83613a8`). That is **dispatching notifications to
subscribers**, not arithmetic on PCM.

So `aacmath=1` never meant "the decoder produced a frame", and
`min([obj2+0x44],[r4+8]) = 0` does not mean "it is asked to produce zero samples":
they are the ends of a range used to choose whom to notify. The
conclusion that hung from that reading is withdrawn.

**The lesson, which now makes two in this investigation:** a label assigned by
conjecture (`aacmath`, `pullbail`) propagates through dozens of measurements and
contaminates them all. Before building on a name, read the ten instructions.

## 11.62 CORRECTION of §11.58: the buffers ARE filled, ~4 000 times

§11.58 concluded "nobody fills with audio the IRAM0 buffers that the DMA drains".
Measured with a `memw` on the head of the buffer (`0x22008c5c-0x22008cdc`), that is also
false:

```
--- mem-write watch 0x22008c5c-0x22008cdc: 127264 stores from 8 distinct PC(s) ---
    writer pc=0x22000294  stores=63328
    writer pc=0x22000298  stores=63328
    writer pc=0x080fb4ec  stores=256
    writer pc=0x080fb50c  stores=256
    (and four more, with 16 to 32 writes)
```

The watched window is **128 bytes**, i.e. 32 words, so 127 264 writes
are about **4 000 complete fills** of the 4 KB buffer — the same order as the
DMA terminal-counts. A copy loop resident in IRAM (two store
instructions, `0x22000294` and `0x22000298`, with identical counts) keeps the buffer
fed at exactly the rate at which it is emptied.

**The real disproportion, in the same run:**

| place | times |
|---|---|
| fill in IRAM (`0x22000294/8`) | ~4 000 fills |
| `copy16` | 4 808 |
| `dpump` (render pump) | **8** |
| `mixer` | **8** |
| `rendret` | 4 |
| `pcmout` | 4 |

The consumer and its filler run at tempo. **The render runs eight times in an entire
150-second run.** So the filler copies over and over a source that
nobody refreshes, and that is why it delivers silence: it is not the writer that is missing, what is missing is the
render producing.

With this, the three earlier formulations are withdrawn — "the loop does not start"
(§11.57), "nobody fills the buffers" (§11.58) and "the codec is asked for zero
samples" (§11.61). The question that remains is narrow and checkable: **the SM1 raises
~2 400 interrupts and the handler signals its event as many times; why does the render
pump only wake up eight times?**

## 11.63 What writes the audio buffer is a `memset` to ZERO

The 127 264 writes of §11.62 come from `0x22000294` and `0x22000298`. In
`assets/classic6g/iram.dis`:

```
2200027c: mov r2, #0                      <-- the fill value is ZERO
22000280: subs r1, r1, #32
22000288: mov lr, r2
2200028c: mov r3, r2
22000290: mov ip, r2
22000294: stmiacs r0!, {r2, r3, ip, lr}   <-- 16 bytes of zeros
22000298: stmiacs r0!, {r2, r3, ip, lr}
2200029c: subscs r1, r1, #32
220002a0: bcs 0x22000294
```

It is an IRAM-resident `memset(dst, 0, n)`. **Nobody is copying audio into the buffer: the
firmware is CLEARING it**, about 4 000 times per run, once per DMA
iteration.

That is not a failure, it is the output driver's **underrun** path doing
exactly the right thing: there is no data to send, so it outputs silence. And it confirms from
another angle that the whole output side is healthy. Also verified on the live
machine:

```
22008c5c: 0x00000000 0x00000000 0x00000000 0x00000000    (the buffer, while playing)
22010020: 0x22008c5c 0x3ca00010 0x22010020 0x84249800    (the DMA descriptor)
```

The descriptor points to itself, so the DMA rereads the same buffer
forever; `ctrl=0x84249800` decodes as 2048 16-bit transfers, source
incrementing, destination fixed at the FIFO, interrupt on completion. All correct.

**The diagnosis, now for real, with the three pieces measured:** the consumer asks at
tempo, the driver answers with silence because it has nothing, and the producer
(`dpump`, eight times) does not produce. And the render pump is only invoked from the
work queue dispatcher, which received eight items — which is the number
that has been carried along from the firmware's send site. The whole chain is
coherent; what is missing is whatever should repeat the send.

## 11.64 The gate that switches off the render: `0x080a3c00` and a status bit

With the three variables set (`SHA1_ZERO=1`, `IRAMSPILL=0x10000`, **`SM1_IRQ=1`**)
the wake-up chain works:

```
credithdlr  2646     the line 18 handler is entered
creditsig   2646     and signals event 0x22008c58 as many times
evwait         8     but the render only waits on it EIGHT times
dpump         16     copy16 9616     WAV: only clicks
```

**`SM1_IRQ=1` gates the very creation of the timer** (`ipod_classic_sm1.c`,
`s->irq_enabled`). Without it there is no tick, no assertion and `raises=0`. The message
that does appear — "guest armed the engine interrupt; VIC line 18 live" — is the
FIRMWARE arming it, not the model raising it: reading it as confirmation cost five
runs. The good signal is "engine interrupt armed on VIC0 line 18" at boot and
then "line 18 raised #N".

**And now the loop, taken apart.** The render loop in IRAM:

```
22006324: bl 0x22003850   -> veneer to 0x0808af58 (the event wait)
22006328: cmp r0, #0
2200632c: beq 0x22006310  -> sleeps 10 and retries... only TWICE
22006338: str fp, [sp,#24]        <- sets the length to ZERO
2200633c: bl 0x22003858   -> veneer to 0x080a3c00
22006340: ldr r1, [sp,#24]
22006344: cmp r1, #0
22006348: bne 0x22006370          <- brought data: live path
22006358: bne 0x220061fc          <- brought none: EXIT
```

The wait succeeds. What decides is `0x080a3c00`, which computes the pointer and length
of the PCM buffer and writes them into the two output parameters — **but only if it passes a
guard**:

```
80a3c18: ldr  r1, [r4,#4]     ; r4 = 0x0896a684 (global), r1 = descriptor
80a3c1c: ldrh r2, [r1]        ; 16-bit status word
80a3c20: cmp  r0, #6          ; r0 = (state==6) ? 2 : 1
80a3c2c: tst  r2, r0
80a3c30: beq  0x80a3cbc       ; <-- bit absent: exits WITHOUT writing anything
80a3c34: ldrh r0, [r1,#2]     ; second guard: sub-block index == 0
80a3c4c: cmpeq r3, #1         ;   and [entry+10] == 1
80a3c50: bne  0x80a3cbc       ; <-- also exits
80a3c6c: str  r1, [r5]        ; buffer pointer
80a3c70: str  r0, [r6]        ; LENGTH
```

If the bit is not there, the length stays at the zero the caller put, and the render
gives up. **That 16-bit status word is a hardware mirror**, and the block
that feeds it is exactly the one we are modeling. If the model does not set that bit, the
fault is ours and can be fixed. The measurement that decides it —how many times each
exit is taken— is running.

## 11.65 LOCATED: the `0x080a3c00` guard fails 100 % of the time

Measured with live playback (`SHA1_ZERO=1 IRAMSPILL=0x10000 SM1_IRQ=1`, and
checking that the counters were not zero before believing anything):

```
fetch      4      called four times
fetchok    0      NEVER exits through the live path
fetchbail  4      ALWAYS exits through the guard
```

And the descriptor, read from the running machine:

```
0896a684: 0x00000000 0x2203ff28 0x2203ff00 0x22038000
0896a694: 0x22038100 0x00000000 0x00000000 0x00000001

2203ff28: 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000
```

`[0x0896a684+4] = 0x2203ff28`, and **its 16-bit status word is ZERO**. The
guard is `tst r2, r0` with `r0` being 1 or 2, so with `r2 = 0` it never passes:
`0x080a3c00` returns without writing the length, the caller reads it as zero and the
render gives up after its two retries. **Every time.**

That is the complete chain of the silence, with no gaps:

```
the DMA drains 4 KB of IRAM0 and asks for more        18 500 times
the driver answers with a memset to ZERO               ~4 000 times   (correct underrun)
the SM1 signals "frame ready"                           2 646 times
the render wakes up, asks for a PCM buffer...
  ...0x080a3c00 looks at [0x2203ff28] = 0, and gives nothing    100 % failure
the render gives up after 2 retries
```

**One question remains, and it is the one that decides whose fault it is:** is that word
zero because nobody ever writes it (in which case something the model should
produce is missing), or because something overwrites it? The candidate for the latter has motive and
opportunity: `copy16` (0x08016f08) writes its PCM to `[r4+12] + offset` and runs past
`0x22040000` — so it **passes over `0x2203ff28` on the way**. And the
size of the ring that governs that offset is an INFERENCE of the model
(`S5L8702_SM1_POS_RING_FRAMES = 0x20000/4`, "IRAM1 is 128 KiB"), not a measurement —
whereas the top of IRAM1 turns out to contain the audio control structures
(table at 0x22038000, buffer at 0x22038100, descriptor at 0x2203ff28). A
128 KB ring from 0x22020000 covers all three.

## 11.66 CONFIRMED: the render overwrites its own control structure

Write watch on the descriptor, with live playback:

```
--- mem-write watch 0x2203ff28-0x2203ff40: 24 stores from 1 distinct PC(s) ---
    writer pc=0x08016f08  stores=24
```

**A single writer, and it is `copy16`** — the loop that copies the decoded PCM into the
render ring. It writes audio samples over `0x2203ff28`, status
word included. That is: the render destroys the structure that the next iteration
needs in order to know where to write, and that is why `0x080a3c00` fails 100 % of the
time (§11.65).

**The arithmetic adds up to the byte.** `copy16` writes to `[r4+12] + offset` with
`[r4+12] = 0x22020000` and `offset = position * 4`, and the model wraps the position at
`S5L8702_SM1_POS_RING_FRAMES = 0x20000/4` = 32768 frames:

```
maximum destination = 0x22020000 + 0x20000 + (chunk of ~2.4 KB) = ~0x22040960
abort observed in s11.36:                                          0x22040a16
```

That wrap **is an inference of the model**, written in the header's own
comment: *"IRAM1 is 128 KiB, so the position wraps every 32768 frames"*. It was never
measured. And what we now know is that the top of IRAM1 **is not ring**: it contains the
audio control structures —table at `0x22038000`, buffer at `0x22038100`,
descriptor at `0x2203ff28`—. A 128 KB ring from `0x22020000` covers all
three.

With that, the `copy16` fault (§11.36) stops being a separate mystery: **it is the same
fault**. It is not that the copy goes haywire, it is that we are telling it the ring is
twice as big as it is. And `IRAMSPILL` could never fix it: it avoids the
abort at the end, not the trampling of the descriptor that happens 2.3 KB earlier.

If the ring goes from `0x22020000` to `0x22038000`, that is `0x18000` bytes = **24576
frames**. There is an environment override (`SM1_POS=N`), so it can be checked without
recompiling.

## 11.67 Where the `copy16` destination comes from, without inferences

§11.66 attributed the trampling to the ring size, relying on the destination being
`0x22020000 + position*4`. That came from the header comment, not from reading the
code. Read:

```
8016e58: mov r2, sp          ; output parameter
8016e60: bl  0x08091710      ; the SM1 position getter
...
8016e94: ldr r2, [sp]        ; reads what the getter wrote
8016e9c: ldr r3, [r4,#12]    ; base
8016ea4: add r2, r3, r2      ; destination = base + value, a sum of BYTES, unscaled
```

And the getter does not return the raw position:

```
8091734: add r1, r3, r1, lsl #2   ; block base + channel*0x14
8091738: ldr r3, [r1,#0xa48]      ; OUR position register
809173c: ldr r0, [r0,#4]          ; a base maintained by the FIRMWARE
8091740: add r0, r0, r3           ; base + position
8091744: str r0, [r2]             ; <- this is the offset copy16 uses
```

So **destination = `[r4+12]` + `[table+4]` + position**, with an accumulator of the
firmware's own in between. Two consequences:

1. The arithmetic of §11.66 (maximum destination ~0x22040960 against the abort at
   0x22040a16) **does not prove the attribution**: with that indirection the fit may
   be a coincidence. The measured part of §11.66 —that `copy16` is the ONLY writer of
   the descriptor— still stands; the explanation of why it reaches that far does not.
2. Lowering the wrap to 24576 frames reduced the tramplings from 24 to 12 but **did
   not eliminate them**, and `fetchok` stayed at 0. Consistent with the above: our
   register contributes, but it does not rule on its own.

The measurement needed is the one that is running: capture `r2` (destination) and
`r3` (base) at the head of the loop `0x08016efc`, which is a branch target and
therefore gives reliable register reads. With that we know the real range the copy
sweeps and whether the wrap governs it, instead of deducing it.

## 11.68 The trampling was a consequence, not a cause: NOBODY writes the status word

With `SM1_POS=16384` (64 KB ring from 0x22020000):

```
copy16 destinations observed    0x2203cef4 .. 0x2203d65a    (measured, not deduced)
tramplings of the descriptor    0                            <-- eliminated
fetchok                         0                            <-- SAME as before
WAV                             only clicks, peak 11444
```

Removing the trampling **fixes nothing**. And since `copy16` was the ONLY writer of
`0x2203ff28` (§11.66), now there are none left: in the whole run, boot included,
**nobody writes that status word**. It was zero from the start; `copy16` trampled it
*in addition*, but it was not the cause of the zero.

The explanation of §11.66 is retracted. What survives of that section is the measured
fact —`copy16` wrote there— and a real improvement: with the ring corrected the render
stops corrupting the control area and the data abort of §11.36 goes away. That fixes
a failure, but not the silence.

**What this establishes, which is the important part:** the audio control block at
the top of IRAM1 **is never initialized**. The pointer that registers it is set
(`[0x0896a684+4] = 0x2203ff28`), so someone set up the structure and something should
fill it. The two possibilities:

- **(a)** firmware code that never gets to run, gated by something the model answers
  wrongly — that would be our failure, indirect;
- **(b)** the block is written by the SM1 engine itself via DMA, and our model writes
  nothing to memory — that would be our gap, direct.

(b) fits the shape of the structure: a word of "slot ready" bits, a sub-block index
and a table of slots at `0x22038000` with length and stride. It is exactly what an
audio engine reports to the CPU. Distinguishing them is the measurement that is
running: watching who writes the global that registers the block names the setup
code.

## 11.69 Who sets up the block, and a blind spot of the model

**The global IS maintained.** Watch on `0x0896a684-0x0896a6a4`: it is written by
eleven sites in the audio engine (`0x08081754`, `0x08081764`, `0x08081984`,
`0x08081aa0`, `0x08081b6c`, `0x080820e0`, `0x0808214c`, `0x08082174`, `0x08082184`,
`0x08082190`, `0x08085ef8`), at `+0`, `+1`, `+2`, `+0x14` and `+0x18`. So the control
structure is alive and the firmware maintains it; the only thing nobody writes is the
block it points to.

**And that block falls where we write.** `S5L8702_BOOTINFO_BASE = 0x2203ff00`, and the
machine's boot does `cpu_physical_memory_write(0x2203ff00, zero, 0x100)`. The audio
descriptor is at `0x2203ff28` — inside those 256 bytes. The header comment says
*"away from anything the firmware uses"*, marked ASSUMED; it is not. It is a single
init write, so it does not trample anything later, but it explains the zero being
read: we set it to zero and nobody touched it afterwards.

**The firmware does NOT give IRAM1 addresses to the SM1.** Searched for in
`SM1TRACE`: zero matches for `2203ff` and for `22038`. But it does give it work, and a
lot of it:

```
+0xc48 (COMMIT)   519 writes
+0x400 (RUN)      518
+0x110, +0xb10, +0xa90, +0xa8c, +0x99c...   the start-up
```

519 "commits" to the engine, and the model accepts them without doing anything.

**The blind spot.** The block's window measures 1 MB and only its first 4 KB are
modeled. Everything above was silently discarded **before** the trace, so not even
`SM1TRACE=1` could show it: if the firmware uploads descriptors or DSP code there, the
whole investigation has been falling into a hole without any measurement noticing.
Added a counter (same logarithmic scale, off without the variable) that reports how
many writes go up there and in what range. Running.

## 11.70 The blind spot, measured: a second register page, not a load

With the new counter (`SM1HI=1`, a variable of its own because `SM1TRACE` generates a
log capable of taking down the entire filesystem — it happened):

```
+0x1000 = 0 / 1 / 3      from IRAM code (0x2200239c)
+0x1008 = 1              more than 1024 times, from 0x080915f0
range seen: +0x01000 .. +0x0100c     (11 lines in the whole run)
```

There is no bulk load of code or descriptors: what was being discarded is **a small
second control page**, `+0x1000..+0x100c`. The dispatcher that writes it (0x080915a4)
translates a bitmask into enable/disable pairs:

```
bit 8 -> +0x1004 = 3, +0x1008 = 1, +0x100c = 1
bit 0 -> +0x1008 = 1
bit 1 -> +0x1008 = r2
bit 2 -> +0x100c = 1
bit 3 -> +0x100c = r2
bit 9 -> +0x1004 = 0, +0x1008 = 0
```

They are all **stores**; in the whole driver area (`0x8091xxx`) there is not a single
read of that page, so returning zero fools nobody. Also ruled out is the neighboring
read of `+0x404` (0x08091590 masks it with bit 8 and our zero is exactly the success
answer).

It is a real gap in the model and it is worth plugging, but **it is not the one
silencing the music**.

## 11.71 CORRECTION: the ring change does not fix the data abort (it does not reproduce)

§11.68 said that with the ring corrected "the data abort of §11.36 goes away". Put to
the test in the only honest way —**without `IRAMSPILL`**, which is the crutch that
was hiding it— and as an A/B:

```
A: default ring    (32768 frames), no IRAMSPILL -> reboots=0  exceptions=0
B: corrected ring  (16384 frames), no IRAMSPILL -> reboots=0  exceptions=0
```

**Neither of the two aborts.** With `SM1_IRQ=1` the abort does not even reproduce, so
the ring change cannot be credited with fixing it: there was nothing to fix in this
configuration. (The reboot of §11.36 was observed with `SM1_IRQ` off; with the
interrupt chain alive the render takes another path.)

What **is** measured about the ring change, and remains true:

| ring | `copy16` writes over the descriptor |
|---|---|
| 32768 frames (default) | 24 |
| 24576 frames | 12 |
| 16384 frames | **0** |

It stops corrupting the control area, which is correct in itself. But it does not
change `fetchok` (still 0) or the WAV (still clicks). **It is not the fix for the
silence, and it must not be presented as such.**

And there is a rereading worth considering before pulling further on this thread:
that `0x080a3c00` gives no buffer may be **correct behavior** —there is nothing
decoded to deliver— rather than a broken guard. In that reading the failure is not in
who writes the status word but in the decoder never producing, and `fetchbail` is a
symptom, not a cause. `framepull` and `pullpass` are 1 in these runs, which fits.

## 11.72 The file IS read from the disk

`ATATRACE` only printed register offsets, so throughout the investigation it was
never possible to see **which sectors** are read. Added `ATALBA` (=1 logarithmic
scale, =2 every read with a virtual timestamp), which uses the `ide_get_sector()` that
was already at hand in `prepare_buf`.

The first song on the list (Billie Jean) is
`iPod_Control/Music/F47/libgpod282598.mp3`, and `filefrag` places it in a single
extent, 4 KB blocks 20707..21862. With the partition offset (92962816) that is
absolute bytes **177 778 688 .. 182 513 664**. And among the measured reads:

```
ATALBA rd #512  lba=348368  (byte 178 364 416)   <-- INSIDE that range
total read in the run: 36 756 KB
```

**The firmware reads the file.** That rules out the simplest reading of §11.71 —"the
decoder never produces because it is never given anything"— at least in its strong
form: the MP3's bytes arrive from the disk.

What remains is to separate *when*: whether the block is read all at once when the
track is opened or in a trickle while it plays. That is what `ATALBA=2` measures,
whose result is cross-referenced with the song's range and with the virtual clock.

## 11.73 The file is read IN FULL, and in two bursts

With `ATALBA=2` (every read, with virtual clock), cross-referenced with the song's
range:

```
368 reads inside the song after pressing play
first: +136 844 374us  byte 177 778 688   <- the first byte of the file
last:  +153 057 975us  byte 182 513 664   <- the last
distribution: two bursts, ~136.9-137.3 s and ~152.9-153.1 s
total read in the run: 51 804 KB
```

The file is read **from start to finish**, not in a trickle. That is what a real iPod
does: fill a large buffer so the disk can be stopped. So the decoder has the song
**complete in memory** and even so not a single sample comes out.

With this the failure is confined to the decoding step, and the suspicion finally has
a concrete name in the tree: in the table of unimplemented blocks in `ipod_classic.c`
there are **`s5l8702.adm` and `s5l8702.adm-sfr`**. ADM is the **CalmADM2E**, the SoC's
audio DSP — a stub that returns zeros and discards whatever is written to it.

If the firmware hands it the decoding work there, everything measured fits without
forcing anything: correct input, correct output, control structures in IRAM1 empty
because whoever fills them does not exist, and perfectly orderly silence. The
measurement that confirms or knocks it down —how much traffic that block receives,
and whether code is dumped into it— is running with `-d unimp`.

## 11.74 The DSP REFUTED: the firmware does not touch the ADM even once

Run with `-d unimp`, which logs every access to an unimplemented block:

```
distribution by block, whole run (274 accesses in total):
    226  s5l8702.gpio
     42  s5l8702.eic
      2  s5l8702.miu
      2  s5l8702.lcd-win
      2  s5l8702.lcd
  adm lines: 0
```

**Zero.** Neither `s5l8702.adm` nor `s5l8702.adm-sfr` receives a single access, either
at boot or during playback. The hypothesis of §11.73 is refuted, and with it the idea
—carried along since wiki/14— that retailOS decodes on the CalmADM2E. **In these runs
the DSP takes no part in anything.**

With that, decoding has to be on the CPU, and what is measured says it does not
happen: `framepull` once, `pullpass` once. And the rest of the system is healthy:
input (the entire file in memory, §11.73), output (18 500 buffers delivered, §11.58)
and the wake-up chain (2 646 signals, §11.64).

That makes fourteen hypotheses refuted by measurement in this investigation. The last
two —file format (§11.59) and DSP (here)— were among the most plausible, and neither
held up. What remains to be answered is narrow: **who should call the frame pull more
than once, and what is stopping it.**

## 11.75 The frame pull is called ONCE, 16 ms after boot

Register capture at the entry of the pull (`0x081e93d8`, a function entry, so the
read is reliable):

```
pullwho hit#1  +16877us  lr=0x081ea388  r0=0x093effbc r1=0 r5=1 r6=r7=0xffffffff
distinct callers: 1
credread 8   pullpass 1   pullret 1   fetchok 0   StartPlayback 2   dpump 8
```

**+16 877 µs is 16 milliseconds: that is boot, not playback**, which starts after 130
seconds. So the decoding path **is not entered even once while the song plays**.
Everything that was measured downstream of there —and the conclusions that hung on
"the codec runs once"— described a boot event.

`credread=8` does not contradict it: it is a shared helper, also called from other
places, and eight matches `dpump`.

**Who calls it.** `lr = 0x081ea388` is the return of a `blx r2` at `0x081ea384`,
inside a notifier that starts at `0x081ea2c0` and has three exits:

```
81ea2cc: cmn r6,#1 / cmnne r7,#1     is the requested range (-1,-1)?
81ea2e0: ldrne r1,[r4,#0x20]         if not: capacity
81ea2ec: cmpne r0,r1                 requested span == capacity?
81ea2fc: popne {..,pc}               <-- EXIT A: returns without notifying
81ea344: ldrb r0,[r4,#0x1c]          saved state
81ea348: cmp r0,r5                   != the requested one?
81ea34c: bne 0x81ea370               --> NOTIFIES
81ea350: cmp r9,#0                   if they are equal, hysteresis by sign
81ea384: blx r2                      <-- EXIT B: notifies (and from here the pull)
81ea388: ...                         <-- EXIT C: no edge, does not notify
```

The only measured execution carried `r6=r7=-1` (range "all") and state 1, and it
fired. What needs to be known now is how many times this notifier is invoked **during
playback** and through which of the three exits it leaves: if it is not invoked, the
failure is further up; if it is invoked and drops out through exit A, the requested
span does not match the capacity and that is buffer arithmetic that can be followed.
Running.

## 11.76 The notifier is not called while it plays either

Capture at the notifier entry (`0x081ea2c0`):

```
6 calls in the WHOLE run, all with lr=0x081c17f0 (a single caller)
   hits 1-3:  +15.4 / +16.3 / +17.9 ms     boot
   hits 4-6:  +16.17 s                     still well before playback
notifyfire 1   notifyskip 6   pull 0   dpump 8
r0=0x09555db0  r3=1 (requested state)  r4=0x0ba90c98  (the queue object)
```

(The 6 "skips" with 1 "fire" do not contradict each other: `0x081ea388` is also the
natural fall-through after the `blx`, so every call goes through there.)

**During playback, between 130 and 190 seconds, the notifier is not invoked a single
time.** So the failure is not in its three exits: it is in whoever should call it.

That caller is unique, `0x081c17f0`, inside a function that starts at `0x081c1794` and
that **begins with a gate**:

```
81c1794: push {r1, r2, r3, r4, r5, lr}
81c1798: mov  r4, r0
81c179c: ldrb r0, [r0, #0x70]      <-- a status byte of the object
81c17ac: cmp  r0, #0
81c17b4: bne  0x81c1808            <-- if it is NOT zero, the whole notification is skipped
81c17c0: ldr  r0, [r4, #0x48]      (live path: virtual dispatch via [+0x18])
```

If that byte stays nonzero, the request to the decoder never goes out, however many
times it is called. The measurement in progress counts the calls during playback and
through which of the two exits they leave; `r0` at entry also gives the exact address
of the byte, so the next step —who writes it— is a one-line watch.

## 11.77 Three consecutive levels run ONLY at boot: the wrong conduit was being traced

```
0x081c1794 (request)     3 calls, at 15.0 / 16.0 / 17.6 ms
0x081ea2c0 (notifier)    6 calls, all under 17 s
0x081e93d8 (pull)        1 call,  at 16.9 ms
during playback (130-190 s): ZERO in all three
```

The `[object+0x70]` gate of §11.76 is not the culprit either: `reqgated` and `reqgo`
both come out at 3, which is the natural passage through the label, not a rejection.

Three consecutive levels that only execute at boot are not three failures: they are
the sign that **this chain is not the playback path**. The addresses come from
earlier sessions that measured them under boot conditions —and a good part of those
measurements were moreover taken without `SM1_IRQ=1`, that is with the wake-up chain
dead (§11.64)—, so they describe the initial priming, not what happens when play is
pressed.

It is worth saying plainly: part of the inherited map points to the wrong place, and
continuing to climb through callers inside it only produces more sleeping levels.

**Change of method.** Instead of assuming which code runs, measure it: sample the PC
through the monitor while the song plays and make a histogram. That says, with no
prior hypotheses, what the device is executing during the 60 seconds in which it
should be decoding, and in which region that code lives (IRAM0, IRAM1 or DRAM).

## 11.78 THE MISSING FACT: the CPU is ASLEEP 88 % of the time

PC sampling through the monitor, 300 samples while the song "plays":

```
0x22003b20   264  (88.0%)  IRAM0
0x00004da8    10   (3.3%)  vector/low
0x22003e8c     4   (1.3%)
... the rest, tails of 1-5 samples
distribution: IRAM0 275, low addresses 25, DRAM 0
```

And `0x22003b20` is, in `iram.dis`:

```
22003b0c: nop / nop / nop / nop
22003b1c: mcr 15, 0, r0, cr7, cr0, {4}    <-- WAIT FOR INTERRUPT
22003b20: nop                             <-- where 88% of the samples land
22003b24: nop / nop / nop
22003b30: b 0x22003b0c
```

It is the **kernel's idle loop**, and the PC is on the instruction following the WFI.

**The device is not stuck: it is asleep.** That reframes the entire investigation.
There is no loop eating the CPU, no deadlock, no failing wait: there is no work. Zero
samples in DRAM means that during those 60 seconds **no application code runs at
all**, and therefore no decoder either — any decoder, anywhere on the map.

It fits everything measured and explains at a stroke why every level that was traced
gave "eight times" or "once": it is not that eight events get lost, it is that the
system wakes up eight times and goes back to sleep.

And it also fits §11.77: there was no wrong conduit to trace, because **during
playback practically nothing runs**.

**The right question, at last.** Not "which link is broken" nor "who writes such and
such word", but: *the SM1 raises 2 646 interrupts and its handler signals the event
as many times; why does that not wake any task?* The signal arrives and the system
keeps sleeping. That is a question of RTXC kernel scheduling, not of audio.

## 11.79 The structure's pointers are static firmware data

Watch on the global's pointer fields (`0x0896a688-0x0896a698`):

```
+0us   pc=0x22000198  addr=0x0896a688
+28us  pc=0x22000198  addr=0x0896a68c
+29us  pc=0x220001a0  addr=0x0896a690
+30us  pc=0x220001a0  addr=0x0896a694
(and no more in the whole run)
```

They are written at microsecond zero, from IRAM0 code (`0x22000198`, the copy
routine that the `memset` of §11.63 also comes from). That is, **they are static
image data**, copied during boot: the address `0x2203ff28` is not
computed by anyone at runtime, it is fixed in the firmware. That is where its authors
put the audio control structure, at the top of IRAM1.

Timeline of the failure in the same run:

```
fetchbail #1 .. #4   +142.2 s .. +142.4 s      (on pressing play)
fetchbail #5 .. #6   +163.6 s
fetchbail #7 .. #8   +169.7 s
fetchok              0
```

Eight attempts, eight failures, none succeeds.

## 11.80 PROBE (not a fix): setting the bit by hand

The guard requires a bit —1 or 2— in the 16-bit word at `0x2203ff28`, which is
zero and which nobody ever writes. Writing it by hand via gdb with the machine running
answers a question that no reading of the disassembly can answer: **is
that guard the only blocker, or are there more behind it?**

If after setting it `fetchok` stops being zero and the WAV changes, the mechanism is
confirmed and we know what the model has to produce. If nothing changes, the guard
is a symptom and we have to look further up —which, with the CPU asleep 88 % of the time
(§11.78), is the most likely outcome—.

Stated explicitly: **this is diagnostic instrumentation**, writing
guest memory from outside. It is not a fix, it cannot stay, and it does not count
as "the player works".

## 11.81 PROBE RESULT: the guard was not the blocker

```
before:    0x2203ff28 = 0x0000    fetchbail=4  fetchok=0
written:   0x2203ff28 = 0x0003    (the bit the guard requires, set by gdb)
45 s later: fetchbail=4  fetchok=0  dpump=0
WAV: still clicks, peak 11444
```

`fetchbail` **does not rise** and `dpump` is **zero**: after setting the bit, the buffer
lookup is not called again even once, because the render does not run
again. The `0x080a3c00` guard was not the blocker — it is that **nobody reschedules
the render to re-evaluate it**. And gdb, on attaching, found the CPU at `0x22003b20`:
the WFI again.

The whole line of §11.64-§11.65 is withdrawn: locating the guard was fine as an
observation, but the conclusion "that is where the player shuts off" was false. It shuts off
earlier, and that is why the guard is only evaluated eight times.

With this and with §11.78, the diagnosis converges on a single sentence: **the system
processes a few buffers on pressing play and then goes to sleep, and nothing wakes it
again to work.** The 2 646 SM1 interrupts come in, their handler
signals an event, and the CPU goes back to the WFI: there is no task waiting on that event because the
one that should have been waiting on it has already finished.

**What has to be looked at, and has not been.** All the sampling and almost all the
measurements have been taken *well into* playback, when everything is already asleep.
The failure happens **at the instant of pressing play** —the first `fetchbail` is the
first thing that happens—, so what matters is what code runs in that window and what
it leaves undone. That is what the ongoing sampling measures.

## 11.82 Not even on pressing play does it get to work

PC sampling in the playback start window (400 samples,
pressing play 2 s after the start of the sampling):

```
idle (0x22003b20): 358 of 400  = 89.5%
non-idle PCs: tails of 1-13 samples in IRAM0 vectors and kernel
DRAM: ZERO samples
```

§11.78 measured 88 % idle well into playback, and the doubt remained whether the
start of the song did bring a burst of work. It does not: **the device is
just as asleep at the instant of pressing play**. And still not a single
sample appears in DRAM, which is where the application code lives.

(Note on method: sampling via the monitor is intrusive —each `info registers`
stops the machine for an instant— so these percentages are valid for comparing with
each other, not as an absolute measure of load. The conclusion they support is qualitative and
robust: there is no burst of work where there should be one.)

There remains an elementary check that has not been done in the whole investigation, and that
may reorder everything: **what the screen shows after pressing play.** In
this session's screenshots it stays on the song list and does not move on to
"Now Playing". If the player does not even get to open, then the eight
measured buffers are not "the start of a playback that shuts off" but something else
—the interface sounds, for example— and the whole chain that has been traced
describes something that was never playback.

## 11.83 THE REAL SYMPTOM: the player opens and closes by itself

Screenshots after pressing play, in the same run:

```
+5 s   the cover animation, spinning -> THE PLAYER IS OPENING
+20 s  back on the song list
+45 s  back on the song list
+70 s  back on the song list
```

And the diff of the press is 42 416 pixels: the screen changes a lot and **comes back**.
It is not that "Now Playing" stays stuck in the animation, as noted in §11.39:
**the player starts and aborts**, returning control to the list.

That is the symptom we should have had from the start, and it reorders the reading of
everything else: the eight buffers, the eight `dpump`, the four `fetchbail` are not "a
playback that shuts off little by little" but **the open attempt and its
cancellation**. After that there is nothing to reschedule, and that is why the CPU stays
in the WFI (§11.78).

Also ruled out, along the same line, that the track record is wrong: dumping
the `mhit`, the type field at `+0x18` contains the fourcc `'MP3 '`, and size,
duration, bitrate (128) and sample rate (44100 in 16.16 = 0xac440000) match the
files on disk.

**For the next session, the right question is a different one:** not "why does no audio come out"
but **"why does the player abort on opening"**. It is attacked from the UI
inward —what error the track open returns— and not from the DMA outward,
which is the way in every time so far and where everything is healthy.

## 11.84 Stale record geometry ruled out, and a check on method

`patch_tracks.py` only updates `size`, `tracklen`, `bitrate` and `samplerate`, so
any other field of the `mhit` still describes the file that libgpod imported the
first time. A natural candidate for a player that opens and aborts. Dumped all
the non-zero fields of the header (584 bytes):

```
+0x070/+0x0a8  0x1ef4fab4 17e97dfa   the 64-bit dbid, repeated (normal)
+0x12c         0x007df303            <-- the suspect
+0x134/+0x138  0x80808080 / 0x8080   "unknown" markers
```

**Refuted without spending a run:** the four tracks carry **the same**
`0x007df303`, and four songs of different durations cannot share a
sample count. It is a constant that libgpod writes, not audio geometry. And
`0x80808080` are "no data" markers: setting them to zero would have been inventing a
value, not correcting one. The database was restored from the previous copy.

(Before this it had already been ruled out that the record had the wrong type: `+0x18` carries the
fourcc `'MP3 '` and the rest matches, §11.83.)

## 11.85 The missing check: do the bytes LAND?

Knowing that the correct sector is read (§11.72) **does not say that the data reaches
guest memory**. If the ATA DMA destination is wrong, the parser sees garbage and
the player aborts — which is exactly the symptom of §11.83, and would be our
fault.

A dump via the monitor does not work to check it: taken 25 s later, the memory
at `0x0bee0720` contained `mhdp`, a piece of the artwork database. The buffers are
reused within seconds, so the late read proves nothing — neither for nor
against.

The correct way is to do it inside the model: store the destination of each
transfer and, on the next one (when the previous one has finished), read its
first words and publish them together with the sector they came from. The song starts at
LBA 347224 and its files start with `ff fb 90 64`, so the check is
exact: either that header appears, or the data does not arrive where it should.

## 11.86 VERIFIED: the MP3 bytes land correctly

First attempt, misleading: all the landings came out `00 00 00 00`. The cause was a
bug in the probe itself — `s->dma_cur` has already advanced `len` bytes when it is read,
so it was looking at the END of the block. Corrected to `dma_cur - len`:

```
ATALND lba=347224 dst=0x0bee0720 -> ff fb 90 64 00 0f f0 00   <-- MP3 frame header
ATALND lba=347344 dst=0x0be53240 -> ff fb 90 64 97 01 05 6b
ATALND lba=181568 dst=0x0bee0720 -> eb 58 90 6d 6b 66 73 2e   <-- control: FAT32 boot
224 landings inside the song
```

The control is as important as the measurement: `eb 58 90` + "mkfs." is a legitimate FAT32 boot
sector in the first sector of the partition, which confirms that the
probe looks where it should. **The input path is verified end to end:
correct sector, correct destination, correct bytes.**

The pattern deserves noting, because it has bitten three times today: a `memw` window
that was too small (§11.36), a dump of an already reused buffer (§11.85) and this
offset. All three return ZEROS, and a zero reads just like a finding.
**Every new probe needs a known control before its result is believed.**

## 11.87 Status at the close of this stretch

All subsystems verified, none fails:

| piece | evidence |
|---|---|
| disk read | the whole file, from the first byte to the last (§11.73) |
| **data in memory** | **`ff fb 90 64` at the correct destination (§11.86)** |
| DMA -> I2S output | 18 500 buffers delivered (§11.58) |
| output driver | `memset` to zero on underrun, correct (§11.63) |
| SM1 interrupt chain | 2 646 signals (§11.64) |
| track metadata | fourcc `'MP3 '`, size and duration match (§11.83) |
| CPU | **~89 % asleep in the WFI, zero samples in DRAM (§11.78, §11.82)** |

And the symptom: **the player opens —cover animation— and goes back to the list**
(§11.83).

With the input verified and the output verified, there is no subsystem left to
blame: **the decision to abort is the player's own**. The next session has
to go in that way, from the UI inward, and not via the DMA outward as has been
done every time.

## 11.88 CORRECTION to §11.78/§11.82: "zero code in DRAM" does not follow from the sampling

Sampling via the monitor takes about **16 samples per second**. If the work that
the device does on opening a track lasts tens of milliseconds of guest
time, the mathematical expectation of capturing it is **less than one sample**. That
DRAM does not appear in 400 samples does not prove that no application code runs: it only
proves that the CPU is idle **most of the time**, which is normal for an
event-driven device.

What §11.78 and §11.82 do support: the CPU spends ~88-89 % of the time in the idle loop's
WFI, and there is no sustained burst of work where there should be one (an MP3 at
128 kbps takes an appreciable, continuous fraction of an ARM of this class). What they do not
support, and is withdrawn, is the sentence "no application code runs at
all".

To answer that, one needs to **trace**, not sample.

## 11.89 `StartPlayback` is another wrong label

```
pbin (0x08150d84):  2 entries, at +22us and +16.19 s      <-- never on pressing play
pbfail (0x08151028): 0     pbgo (0x08150e00): 2
```

The function has an explicit failure exit —it releases the object with the same routine
as the prologue and returns 0— and **does not take it**. But it does not run when
playing either: its two entries are at 22 microseconds and at 16 seconds.

That makes four names from the inherited map that are not what they say: `aacmath` (which
is a notification dispatch, §11.61), `pullbail` (which is the epilogue), the
`pull`/notifier/request chain (which is boot code, §11.77) and now
`StartPlayback`. **The inherited address map has to be treated as suspect
until each name is rechecked with a measurement.**

## 11.90 THE FINDING THAT INVALIDATES THE APPROACH: everything happens in second 0

`cap=8` in the harness records only the **first eight hits of each site**, so
all the times read in this investigation are those from the start, not from the
bulk. With the cap raised to 20 000 and a per-second histogram made:

```
copy16     4808  ->  ALL in second 0
dpump         8  ->  all in second 0
mixer         8  ->  all in second 0
pcmout        4  ->  all in second 0
fetchbail     4  ->  all in second 0
creditsig   325  ->  290 in second 0, 32 in second 10, 3 in second 11
```

Play is pressed at **~140 seconds**. **None of these sites runs on
play: they all happen in the first second of boot.**

This closes the circle of §11.77, §11.81 and §11.89, and the conclusion is
stronger than it seemed: **all the audio machinery that this investigation has
been measuring —the render, the mixer, the PCM copy, the pump, the buffer
lookup— is the boot's silence push.** The "eight buffers", the "eight
dpump", the "four fetchbail" that have been chased for months do not describe a
broken playback: they describe initialization, which works.

And therefore: **on pressing play the audio pipeline is not entered at all.**
The player opens, aborts (§11.83) and does not even get to touch the render. That
explains without residue why the CPU stays in the WFI and why every level that was
traced gave "eight times".

**Consequence for future work.** The inherited counters (`dpump`, `mixer`,
`copy16`, `pcmout`, `rendret`, `aacmath`, `framepull`) **are no use for measuring
playback**. Any earlier conclusion that uses them as evidence of what happens
on play has to be reread as evidence of what happens at boot. And every
new measurement has to carry a **timestamp and a high cap**, or it lies again in the same way.

## 11.91 CORRECTION to §11.90: it generalizes from a single run

§11.90 concluded that "everything happens in second 0" from run `uc1`. Placed
next to another run from the same session, it does not hold as stated:

```
uc1:  copy16 4808, dpump 8, mixer 8, fetchbail 4   ->  ALL in second 0
pt1:  copy16 8, dpump 8, mixer 8, fetchbail 8      ->  first at 142.16 s
```

The plugin's timestamps are **wall clock**, relative to the first
logged line (`t0` is fixed at the first log of any kind), so both
readings are valid and say different things: in `uc1` the audio pipeline ran at
boot and not on play; in `pt1` it ran on play, at 142 seconds.

**What holds** and is still important:

- `cap=8` hid the time distribution in all the earlier runs, and without
  a timestamp the counters do not distinguish boot from playback. That part
  of §11.90 is correct and stands as a permanent warning.
- The "eight buffers" are not a stable measurement of anything: **the same counter is 8 at
  boot or 8 on play depending on the run**, and comparing them across runs without
  looking at the clock is what has sustained conclusions for months.

**What does NOT hold**, and is withdrawn: the sentence "on pressing play the audio
pipeline is not entered at all". In `pt1` it is entered. The correct statement is that
**sometimes it is entered and sometimes not**, and that run-to-run variability is in itself
a fact that has to be explained before going on.

## 11.92 A playback-time anchor, and the mirror-image wake-tap trap

**The anchor.** All the named sites of the inherited map also fire at
boot, so none of them serves to open a trace window at the moment of
playing — and that is why every attempt filled up with boot noise (§11.90). What
is exclusive to playback is a disk read **inside the song's file**. Added to the ATA model
(`ATALBA_LO`/`ATALBA_HI` bound the range in
bytes) and measured:

```
ATASONG #1..#256  byte=177778688..  pc~0x0805a70c
distinct PCs: 1
```

A single firmware PC requests all the song's data. It is playback-time code
by construction, and it is the entry point that was missing to attack
"from the UI inward".

**The trap, a mirror image of the one already documented.** `iremote.py`'s wake tap
—needed because a cold SELECT is ignored— is the pair `e,q`, which nets zero in
the middle of a list. But **at the end of a list `e` is a no-op and `q` goes up one: net
−1**. Trying to play via "Shuffle Songs" (the last entry of the menu), the
tap moved the highlight to "Settings" and the press opened the settings; the previous
screenshot showed "Shuffle Songs / 4 Songs" correctly highlighted, so without looking at
both screenshots the result would have passed for "Shuffle Songs does not play".

```
start of list:  q is a no-op  ->  use e,q  (net 0);  q,e net +1
end of list:    e is a no-op  ->  use q,e  (net 0);  e,q net -1
```

And a navigation note that is robust: to reach the last entry of a
menu, scroll down more than enough and let the list hit its end, instead of counting positions. Counting
six scrolls landed on "Settings"; hitting the end of the list always gets it right.

## 11.93 THE PLAYER'S CALL CHAIN, captured at playback time

With the anchor of §11.92 (a disk read inside the song's file) one can also
**sweep the guest's stack** at that instant and keep the
values that fall within the firmware's code range. The SP is constant across all the
reads (`0x0bee0374`), i.e. the same task and the same stack, so the reading is
stable:

```
0x0805a70c   disk read (ATA driver)
0x08326518   <- lr, return of the `bl 0x805a6f4` at 0x08326514 (file layer)
0x0827a6fc
0x08246948
0x08246678
0x082465f0   return of a `blx r1` (virtual dispatch)
0x0813b20c   return of a `blx r1` at 0x0813b208
0x0808dfa8   return of a `blx r2` at 0x0808dfa4
0x0813b220
0x0808df88
```

**It is the first real foothold inside the player**, with authentic stack frames
instead of inherited labels —which have turned out wrong four out of
four times (§11.89)—. And it explains why no static search found them:
almost all the frames are returns from **virtual dispatches** (`blx` to table
pointers), which do not appear as a `bl` to a fixed address.

**It was also confirmed that the abort is general.** Playing via "Shuffle Songs"
instead of via the song list —a completely different entry path— the
result is identical: `copy16=8`, `dpump=8`, and after 33 seconds back on the
menu. It is not a problem of how the track is chosen.

**Where to go next.** The upper frames (`0x0808df88`, `0x0813b220` and their callers)
are the code that governs playback. That is where to look for the condition that
decides to give up, with the same discipline that was needed here: measure with a
timestamp, a high cap and a known control next to each probe.

## 11.94 The Xing header hypothesis REFUTED

The song file is read **445 times** for 4.7 MB, when with 32-61 KB blocks
about 118 would suffice: it is reread in full three or four times. That is not loading it
into memory, it is **retrying**. And a classic reason for a player to reread an
MP3 in full is not finding the **Xing/Info** header with the stream length — which is
exactly what was removed when re-encoding with `-write_xing 0` in §11.59, in the belief that "no
tags at all" was the cleanest configuration.

With all four re-encoded with `-write_xing 1` and ID3v2.3 tags (as iTunes
writes them), measured:

```
fetchok 0   fetchbail 8   copy16 8   dpump 8
WAV: clicks only, 27 456 non-zero samples out of 16 799 744, peak 11444
```

**Identical.** The hypothesis is refuted, and with it the reading that the rereading
pattern comes from metadata missing from the file.

**Method warning, the same one again.** The same run reported "0 reads inside
the song", which looks like a huge finding. It is not: rewriting the files
changed their size (~500 bytes) and their location on disk, so the
`ATALBA_LO/HI` range —computed with `filefrag` on the PREVIOUS files— stopped
covering them. **Every time the files are touched the range has to be recomputed**, or the
probe measures a place where there is no longer anything.

## 11.95 POSITIVE CONTROL: the output path CAN carry music

A control was missing that separates two things that had been getting confused. That the DMA
delivers 18 500 buffers and that the wheel clicks reach the WAV proves that the path
carries **short PCM** (a click is 1 248 samples); it does not prove that it sustains music
for minutes. That had never been verified.

The `HLE_AUDIO_DATAPATH` shim decodes on the host and feeds the PCM into the same
DMA channel as the firmware. **It is not a solution** —it is not the retailOS player—
but as a control it answers exactly that:

```
with host-side decoding:      14 987 242 samples, 5 506 568 non-zero (36.74%), peak 32768
with the retailOS player:     16 799 744 samples,    27 456 non-zero ( 0.16%), peak 11444
```

**Real music versus clicks.** The output path —DMA, I2S, pacing, capture— is
**demonstrated** end to end with sustained audio, not inferred.

With this the problem is narrowed down with no possible ambiguity:

| side | status |
|---|---|
| input (disk -> memory) | **demonstrated**: the whole file, and `ff fb 90 64` at the correct destination (§11.86) |
| output (memory -> speaker) | **demonstrated**: 36.74 % non-zero samples with real audio (here) |
| decoding in the firmware | **is the only thing that fails** |

**Consequence for future work:** do not investigate the output path again.
It is proven. All effort goes to why the player aborts when it opens
(§11.83), with the call chain of §11.93 as the entry point.

## 11.96 The "structural lock" of `[engobj+0x44]` REFUTED

`ipod_classic_hle_np.c` documents the mechanism that was taken as the root cause for
thirteen rounds: the render pump aborts with −1 before doing anything when
`[engobj+0x44] != 0`, and *a single* −1 makes the engine stop re-posting, the
job counter decay to 0 and the loop park forever. It fits
perfectly with the eight-buffer signature that appears in every measurement.

**Both branches of the gate** measured separately (not the check, which is what
a `pc=` at `0x22006170` counts and would have given a misleading "8"):

```
22006170: ldrb r0,[r4,#0x44]     the flag
2200617c: beq  0x22006194        <- ZERO: live path
22006180: mov  r0,#5             <- NON-zero: aborts with -1

aborts    (0x22006180) = 0
continues (0x22006194) = 8
```

**The abort branch is not taken even once.** The gate passes all eight times. The
structural lock described no longer occurs —it was probably resolved by the earlier fixes
(VIC line 18, SHA-1 engine, RTC)— and that note in the tree describes an
earlier state of the emulator.

The flag **is written** during playback (`0x22006888` at 148.6 s, and
several more at 163.7 s), but with a value that lets the gate pass.

Hypothesis sixteen, refuted. And a method note that almost slipped through again: putting
the probe on the check instruction counts every evaluation, not every abort.
**You have to instrument the branches, not the condition.**

## 11.97 The pump's real path, and the CAUSE narrowed down: the descriptor block is never set up

**The pump's internal path**, traced with `range=0x22006100-0x22006600` and
triggered at its own entry (small range, one window per invocation):

```
0x22006118 entry -> 0x2200615c -> 0x22006194 (the [engobj+0x44] gate PASSES)
-> 0x220061d0 -> 0x220061e0 -> 0x220061f0 -> 0x22006204 -> 0x22006234
-> 0x2200624c -> 0x22006260 -> 0x22006294 -> 0x220062e4 -> 0x22006304
-> 0x22006324 (event wait, successful) -> 0x2200633c (the buffer lookup)
-> 0x22006340 -> 0x2200634c -> 0x220061fc   <-- EXITS
```

It exits via `bne 0x220061fc`, which is exactly the "returned length is
zero" branch. This **reopens** what §11.81 had closed: that experiment set the
guard bit 25 seconds AFTER pressing play, when the pump was no longer being invoked. The
measurement was correct; the experiment was badly timed and the conclusion hanging
from it, wrong.

**Repeated properly** —holding the bit from before pressing and throughout the whole
window— it still fails:

```
fetchok 0   live (0x22006370) 0   exits (0x220061fc) 8
```

Because the lookup has TWO guards, and the second requires
`[0x22038000 + index*4 + 10] == 1` in the slot table. And that table is also
all zeros.

**The measurement that closes it.** A write watch on `0x22038000-0x22038040`
during a complete run with playback:

```
total writes: 0
```

**Nobody ever writes the slot table** — neither at boot nor during playback.
Together with the descriptor at `0x2203ff28`, whose only writer was `copy16` clobbering it
(§11.66), the conclusion is established from two independent angles:

> **The audio descriptor block at the top of IRAM1 is never initialized.**
> That is why the lookup fails 100 % of the time, the pump exits without producing, the
> engine stops re-posting and the loop parks. That is the cause, and everything else that has
> been measured over months is its consequences.

And since the DSP is refuted (§11.74) and no DMA channel writes there (§11.58), what
is missing is **firmware code that never gets to run**.

**The next step, concretely.** The global that holds those pointers (`0x0896a684`)
is referenced from six places in the firmware:

```
0x080821b4   0x08085f38   0x0808afc8   0x080a3ccc (the lookup)   0x080b5320   0x08272f34
```

One of them should set up the table. Putting a `pc=` on each and seeing which one does **not**
run during playback names the missing initializer, and from there the condition that
gates it — which is where the model's fault will be.

## 11.98 The semantics of the slot table, and who consumes it

The six places that reference the global `0x0896a684` probed, with
timestamps:

```
ctx    (0x08082100)  n=4  +137.07 s    runs during playback
s2     (0x08085ef8)  n=3  +137.21 s    runs
evwait (0x0808af58)  n=4  +137.07 s    runs
fetch  (0x080a3c00)  n=4  +137.17 s    runs, and fails
s5     (0x080b5300)  n=0               NEVER
```

The one that never runs turned out, when read, to be the **"slot consumed"** routine
(`0x080b52f0`):

```
80b52f4: ldr  r4, [pc,#36]        ; the audio global
80b52fc: ldr  r0, [r4,#20]        ; slot index
80b5300: ldr  r1, [r4,#12]        ; table base = 0x22038000
80b5304: add  r3, r1, r0, lsl #2
80b5308: strh r2, [r3,#10]        ; sets [table + index*4 + 10] to ZERO
80b5314: bl   0x8014848           ; wraps the index
80b5318: str  r1, [r4,#20]        ; and advances it
```

It writes **zero** to exactly the field that the lookup's second guard requires to
be **one**. With that the semantics are fixed unambiguously:

> `[table + index*4 + 10]` is the **"slot ready"** flag: **1 = there is data**,
> and `0x080b52f0` clears it when consuming it. `[global+0x14]` is the current index.

That it never runs is **consistent**, not a bug: there is nothing to consume. And it
shifts the question to the **producer**: who should set that field to 1 and fill the
buffer at `0x22038100`.

Of the six places, three run during playback (`ctx`, `s2`, `evwait`) and none is the
producer: `s2` is engine control (it writes the second register page via
`0x080915a4`), `evwait` is the event wait with its 500 ms timeout, and `ctx` is
the context constructor. So **the producer is outside those six**, or it is
one of them via a path that has not been followed yet.

Searching for `strh` with offset 10 does not discriminate —it is too common an offset, more
than twenty generic candidates—, so the useful path is the reverse: start from who
writes the **buffer** `0x22038100` (a watch like the ones that have already worked three times
in this session) and work up from there.

## 11.99 ROOT CAUSE ESTABLISHED: the producer of the slot mechanism does not exist

Three independent watches, on complete runs with real playback:

```
slot table         0x22038000-0x22038040  ->  0 writes
PCM buffer         0x22038100-0x22038180  ->  0 writes
status word        0x2203ff28-0x2203ff40  ->  0 writes (only copy16 clobbering it, s11.66)
```

**Nobody ever writes anything into the slot mechanism.** The structure exists —its
pointers are static data copied at microsecond zero (§11.79)— but the producer
side is simply absent.

With that the chain is closed end to end, each link with its measurement:

```
the slot producer                  NEVER RUNS           <-- THE CAUSE
the lookup 0x080a3c00              fails 100% (4-8)         correct consequence
the render pump exits via 0x220061fc                        correct consequence
the engine stops re-posting, the counter decays             correct consequence
the output driver memsets silence (~4000)                   CORRECT behavior
the DMA delivers 18 500 buffers of zeros to the I2S         CORRECT behavior
the WAV contains only the wheel clicks                      consequence
```

**Everything that has been chased downstream for months is consequences.** The
consumer works and does the right thing when it finds no data; the driver does the
right thing by outputting silence; the DMA does the right thing by delivering it. The fault is that
nothing produces.

And this also closes, with evidence, why the player opens and aborts
(§11.83): it opens, asks for a buffer, there is none, and it closes.

**What remains to be known, in one sentence:** which firmware task should fill those
slots and why it does not start. It is not a problem of the audio path —input and output
are DEMONSTRATED (§11.86, §11.95)— but that a producer task never comes to
exist or is never scheduled.

## 11.100 The two PCM regions are DISJOINT

Putting together two measurements from this session that had not been placed side by side:

```
copy16 writes PCM into IRAM1      0x2203cef4 .. 0x2203d65a   (measured, s11.68)
the audio DMA reads from IRAM0    0x22008c5c                 (measured, s11.58)
the only writer of 0x22008c5c is the memset to ZERO          (measured, s11.63)
the slot buffer is at             0x22038100                 (measured, s11.65)
```

**Three different regions and none connected to the next.** The render produces
PCM into a ring in IRAM1; the DMA plays from an IRAM0 buffer that only receives
zeros; and the slot mechanism, which is what the lookup consults, lives in a
third area that nobody touches.

That fits §11.99 and makes it more precise: it is not just that "the producer does not
write". It is that **the link that should copy from the render ring to the buffer
the DMA plays does not exist in any run**, and that is why the output driver
finds the buffer empty and does the only correct thing it can do: fill it with
silence.

It is worth saying what this does NOT prove. That the three regions are disconnected is a
measured fact; that the missing connection is "a copy from IRAM1 to IRAM0" is a
reasonable reading but **not measured** — it could be that the firmware must reprogram the
DMA to read from the ring, or that the slot mechanism is the right path and the
`copy16` ring belongs to something else (the boot silence push, for
example, which is when `copy16` accumulates its 4 808 passes).

**The measurement that would decide it:** check whether in any run the DMA descriptor
(`0x22010020`) ever points to IRAM1 instead of `0x22008c5c`. A watch on those
16 bytes answers it in one run, and distinguishes "a copy is missing" from "a DMA
reprogram is missing".

## 11.101 The §11.100 ambiguity RESOLVED: the COPY is missing, not a DMA reprogram

Watch on the audio DMA descriptor (`0x22010020`, 16 bytes) during a
run with playback:

```
+0us      written by the boot copy routines (0x08000294/98)
+7 ms     rewritten from IRAM (0x22000294/98)
+20.38 s  rewritten by 0x22004b4c..0x22004b60  <- the ping-pong rotation
400 writes in total
the descriptor at the end:  src=0x22009c5c  dst=0x3ca00010  next=0x22010020
```

**The source is ALWAYS IRAM0** —`0x22008c5c` or its partner `0x22009c5c`—, never IRAM1.
The firmware alternates between the two ping-pong buffers and that is the end of the story.

That resolves the ambiguity §11.100 left open: **no DMA reprogram is missing;
what is missing is whoever copies the decoded PCM into those IRAM0 buffers.** The two
possible readings are reduced to one, with measurement.

The code that rotates the descriptor is at `0x22004b10..0x22004b64` and it is the LLI
constructor:

```
22004b14: ldr r0, [r0, r4, lsl #2]   ; buffer[index] from an array of pointers
22004b4c: str lr, [r3, #12]          ; ctrl
22004b54: str lr, [r3, #4]           ; dst  (the I2S FIFO)
22004b5c: str lr, [r3]               ; src  (the chosen buffer)
22004b60: str r3, [r3, #8]           ; next -> the same descriptor
```

That is: the driver picks `buffer[index]`, sets it as the source and queues it. What
**nobody** does is fill it with audio beforehand — only the silence `memset` (§11.63)
touches it.

**The exact point to continue from**, now without ambiguity: who should write
`0x22008c5c` / `0x22009c5c` with PCM before the rotation queues them. That is the
absent producer of §11.99, now located at a concrete address and with the
code that consumes it in plain view.

## 11.102 Failed attempt to capture the `copy16` base, and what it did yield

Idea: at the loop head (`0x08016efc`) `r3` contains the destination base
`[r4+12]`, but **only on the first iteration** — afterwards it is overwritten by the sample read. Proposed
filter: keep the lines with maximum `r1` (remaining halfwords), which
should be the copy starts.

**It did not work.** With `regcap=3000` the captures are a CONTIGUOUS window in the middle of a
burst, not the starts, so the maximum `r1` observed was 0xff and the corresponding `r3`
values are audio samples (`0x00fffffe`, `0x0000003d`, `0x00ffef9e`),
not addresses. The base is still unmeasured.

To get it one would need to capture at `0x08016edc` (which is a branch target) and
read `[r4+12]` from memory, something `regpc` does not do: it gives registers, not memory. The
practical route is a gdb breakpoint with a `print` of that word, or a probe in the
model that publishes the memory as `ATASONG`/`ATALND` already do.

**What the run did yield:** the range of `copy16` destinations goes from `0x2203d502` to
`0x2203fa5a`. It covers `0x2203ff28`, which confirms for the third time the clobbering of the
descriptor with the default wrap (§11.66), and confirms that the region the
render writes is disjoint from the buffers the DMA plays (§11.100).

## 11.103 `[r4+12]` is not a base: it is an offset

Read with a gdb breakpoint at `0x08016edc` (which can read memory, something
`regpc` cannot):

```
COPYBASE base=0x000119b4  count=20    r4=0x0bb12d54
COPYBASE base=0x000117ac  count=520   r4=0x0bb12d54
COPYBASE base=0x00011660  count=328   r4=0x0bb12d54
COPYBASE base=0x00010000  count=16    r4=0x0bb12d54
COPYBASE base=0x000100ac  count=112   r4=0x0bb12d54
```

The values of `[r4+12]` are between `0x10000` and `0x119b4`: **they are not addresses,
they are offsets**. And `[r4+16]` (the counter) goes from 4 to 520 bytes per copy, not
kilobytes.

That corrects the reading of §11.67. The real sum is:

```
destination = [r4+12] (small offset)
            + [table+4] (a BASE maintained by the firmware)
            + [bloque+0xa48] (OUR SM1 position register)
```

So the destination address is dominated by a firmware value **and our
register**, not by a pointer in the copy structure. That reinforces that the ring
wrap (`S5L8702_SM1_POS_RING_FRAMES`, marked as an inference in the
header itself: *"IRAM1 is 128 KiB, so the position wraps every 32768 frames"*) is a
serious candidate for being wrong — but it also explains why changing it moved the destination
only partially (§11.68): there are two more addends, and one is the firmware's.

**And a tooling note, which has already cost two attempts:** the plugin's `regpc` gives
registers, not memory. To read `[register+offset]` you have to use a
gdb breakpoint with `printf`, or publish the memory from the model as
`ATASONG`/`ATALND` do. Trying to deduce it by filtering register captures yielded audio
samples disguised as addresses (§11.102).

## 11.104 The render ring is HEALTHY, and the gap is characterized

The destination sum broken down with a breakpoint right after the getter
(`0x08016e64`, where `[sp]` already contains firmware_base + SM1_position):

```
GET sp0=0x22025800   r4=0x0bb12d54
GET sp0=0x22025c00   r4=0x0bb12d54     <- advances in 0x400 steps
```

The getter returns **IRAM1 addresses around `0x22025800`**, and they **advance
in order**, 1 KB at a time. That is not a runaway pointer: it is a ring that
progresses as it should. Adding the `[r4+12]` offset (§11.103), the render writes
its 4- to 520-byte chunks within that area.

**With this the picture is closed and every piece measured:**

```
the render writes PCM into the IRAM1 ring       ~0x22025800, advancing  <- HEALTHY
the DMA plays from IRAM0                        0x22008c5c/0x22009c5c   <- HEALTHY
the driver rotates the two IRAM0 buffers        0x22004b10..0x22004b64  <- HEALTHY
the only thing touching those buffers is the memset to ZERO             <- correct underrun
MISSING: whoever moves the PCM from the IRAM1 ring to the IRAM0 buffers
```

And that rules out, by measurement, the §11.103 suspicion that the ring wrap
(`SM1_POS_RING_FRAMES`) was diverting the write: the position the firmware
ends up using is small and orderly, so **the diversion is not there**. The badly sized
ring does corrupt the control area (§11.66) and should be fixed, but it is not
the cause of the silence.

**The final sentence of the diagnosis, with everything measured:** playback produces no sound because
**nobody copies the decoded audio from the render ring to the buffer the
DMA plays**. Everything else in the chain —disk read, landing in memory,
decoding into the ring, buffer rotation, DMA, I2S, WAV— is verified and
works.

## 11.105 THE COMPLETE MECHANISM: one handshake byte, and who sets it

Following the caller of the silence fill all the way down, with conditional gdb
breakpoints (the only tool that filters at capture time; capped probes always
filled up during boot):

```
220077b8: bl 0x220002d4     clears the chosen buffer, 4096 bytes
220077bc: bl 0x220073c8     getter -> object 0x22010318
220077c0: bl 0x22007440     ldr r0,[obj+0x20] -> firmware 0x0816c150
0816c150: ldrb r0,[r0]      <-- ONE instruction: returns *(byte*)
220077c4: cmp r0, #0
220077cc: beq 0x220077f4    ZERO  -> doesn't fill; the cleared silence remains
220077ec: bl 0x22006ac0     NON-ZERO -> FILLS the buffer with audio
```

**Everything depends on one byte.** And that byte, measured with a watchpoint,
**does get set**:

```
flag at 0x0bb169dc
SET flag=1  from lr=0x081fef40   x4
SET flag=0  from lr=0x220055b4   x1
```

`lr=0x081fef40` places the call at `0x081fef3c` (`bl 0x8015b28`), inside the
same function as **`0x081fef28`** — the firmware's send site that was the FIRST
measurement of this session. And it sets it to 1 **four times**, which is
exactly the "3 jobs + 1 startup" measured back then.

**Two corrections this forces:**

1. §11.63 said the buffer clear is the underrun path. **It isn't:** it is an
   unconditional "clear before filling". The fault isn't that someone fills
   with silence, it's that the fill-with-audio step doesn't run.
2. §11.99 said "the producer doesn't exist". **It exists and works** — it sets
   the flag four times. What it doesn't do is repeat it at the rate the
   consumer needs (~43/s).

**The mechanism, now complete and without gaps:**

```
the producer (0x081fef3c) marks "audio available"     4 times
the driver clears the buffer and checks the flag      ~43 times per second
   finds it at 1 -> fills with audio                  4 times
   finds it at 0 -> leaves the silence                the rest
the DMA delivers whatever is there                    18,500 buffers
```

And with that the question of the whole investigation is reduced to its
minimal form, at the same place where it started: **what should make
`0x081fef28` repeat.**

## 11.106 THE COMPLETE CHAIN, measured level by level

Going down from the WAV's silence to the origin, with conditional gdb
breakpoints (the only tool that filters at capture time; every capped probe
filled up during boot):

```
0x081d05dc  setter: str [obj+0x18]; strb 1,[obj+0x67]   4 times, only at startup
     |      (assigns a new buffer and raises the "work available" flag)
     v
0x081feec8  gate: ldrb [r4+0x67]; cmp #1; bne           EVALUATED MANY TIMES,
     |      (consumes the flag if it is 1)               byte=0 always except 4
     v
0x081fef28  send (blx to 0x081d0944)                     4 times
0x081fef3c  sets the "audio available" flag = 1          4 times
     v
0x220077c0  the driver checks that flag (0x0816c150,     ~43 times/second
     |      which is "ldrb r0,[r0]; bx lr")
     v
0x22006ac0  fills the buffer with audio   ONLY if the flag was 1
0x220002d4  clears the buffer (4096 B)    ALWAYS, before checking
     v
DMA -> I2S -> WAV                                        18,500 buffers
```

**The data point that closes it:** the gate `0x081feec8` **is evaluated
repeatedly** —from `0x22003fc4` and `0x220072e8`, two IRAM sites— and the byte
is **zero every time**. So the scheduler works, the task runs and the code
reaches the gate. What doesn't happen is anyone assigning a buffer again.

**The final sentence:** the player makes no sound because the buffer
assignment (`0x081d05dc`) happens **four times when playback starts and is
never repeated**, while the driver needs it some 43 times per second. No code
is missing, no structure is missing, there is no deadlock: what's missing is
the event that re-arms the cycle.

**What remains to be known, and it's the only thing:** who should call
`0x081d05dc` recurrently. On real hardware that caller fires with each consumed
buffer; here it only does so at the beginning. That's where the event the model
doesn't produce will be — and it is exactly the same place the first
measurement of this session pointed to (`0x081fef28`, "three jobs where forty
per second would be needed"), now with the whole intermediate mechanism
explained.

## 11.107 BOTH ASSIGNERS ARE STARTUP ONLY: the recurring path doesn't exist

Arming the breakpoint **before** pressing play (attaching it 25 s later sees
nothing, because the assignments happen ON the press — the same timing mistake
as §11.81):

```
ASSIGNS obj=0x0bae8ad8 value=0x0ba77cf4 from lr=0x0814ff50
ASSIGNS obj=0x0bae8ad8 value=0x09555db0 from lr=0x08150ff8
ASSIGNS obj=0x0bae8ad8 value=0x0ba6f9f0 from lr=0x0814ff50
ASSIGNS obj=0x0bae8ad8 value=0x09555db0 from lr=0x0814ff50
ASSIGNS obj=0x0bae8ad8 value=0x0ba6f9f0 from lr=0x08150ff8
ASSIGNS obj=0x0bae8ad8 value=0x093afe58 from lr=0x0814ff50
```

**Two callers, both initialization:**

- `0x08150ff8` is inside `0x08150d84`, the playback startup function.
- `0x0814ff50` is the return of `bl 0x8183d74` at `0x0814ff4c`, preceded by a
  string of `strb`s to `[r4+0x93d..0x941]` and construction calls —
  unambiguously initialization.

**There is no recurring assignment path.** On real hardware, what re-assigns is
the return of the cycle: the DMA consumes a buffer, its interrupt reaches the
driver, and the driver asks the player for the next one. Here that last leg
—from the driver back to the player— is the one that doesn't happen.

And it fits with everything: the DMA interrupt arrives (18,500 terminal
counts), the driver runs (clears and checks the flag ~43 times per second), but
**it doesn't re-engage with the producer**. That's why the producer only works
the six times at startup.

**The final diagnosis of this investigation, in one sentence:** the retailOS
audio cycle is reactive —consuming a buffer must request the next one— and in
this emulation **the loop opens exactly at the return from the driver to the
player**. Everything else —disk, memory, decoding, ring, DMA, I2S— is measured
and works.

## 11.108 The driver's notice, and who reads it

The function that hands buffers to the DMA (`0x22007780`, whose literals are
`0x22008c5c` and `0x22009c5c`) publishes its status on both branches:

```
data available -> bl 0x220043fc with 1     none -> bl 0x220043fc with 0
```

`0x220043fc` is a bit set/clear on a **status mask at `0x2200ae2c`**, under a
critical section (`0x22001e58` / `0x22001e6c`). So the driver **does publish**
that it has run out of data, on every pass.

With a READ watchpoint (`rwatch`, not a write one: the writes were already
known, what was missing was knowing whether anyone listens):

```
0x22004418   inside 0x220043fc itself
0x220029a0   the kernel dispatcher
0x220029e4   same
value read = 32  (bit 5)
```

And the dispatcher does:

```
2200299c: ldr r0, [r4]        the mask
220029a4: and r0, r0, #70     0x46 = bits 1, 2 and 6
220029a8: orrs r0, r0, [r4+12]
220029b8: bne  -> dispatches
```

**The scheduler only considers bits 1, 2 and 6; the audio driver uses bit 5,
which falls outside that mask.** Noted as a measured fact, with the caveat that
it's probably by design —a status bit, not a wake-up bit— since the same code
runs on real hardware. It shouldn't be treated as a fault without more
evidence.

**State of the search at the close of this line:** the driver correctly
publishes that it has no data, and the scheduler works. What hasn't been found
is the path by which that notice should get back to the player so that it
assigns the next buffer. The two known assigners (§11.107) are initialization,
so the recurring path exists in the firmware but hasn't been located yet.

## 11.109 THE FOUR SITES THAT AREN'T REACHED, and the function that gates them

Static search: **six** firmware sites call `0x08183d74`, which ends up
assigning a buffer and raising the send flag:

```
0x0814f1a8   0x0814ff4c   0x08150ff4   0x081516c8   0x081529bc   0x08152ca0
```

All six probed with gdb, armed BEFORE pressing play:

```
CALLS 0x0814ff4c (init)      x4
CALLS 0x08150ff4 (startup)   x2
the other four: NEVER
```

**Only the two initialization ones run.** The recurring path exists in the
firmware and isn't entered.

**A tool failure that cost two runs, and that's worth not repeating:**
`commands 1-6` is not valid syntax in this gdb. The breakpoints are left armed
WITHOUT a command block, so when one fires gdb **stops the machine and waits**:
the interface freezes and the play press never gets in
(`press ret x20: diff=0`). That reads as "no site runs" when in fact the
emulator was stopped at the first one. You have to put **one `commands N` block
per breakpoint**, each with its own `continue`.

**The structure of the three sibling sites** (`0x081516c8`, `0x081529bc`,
`0x08152ca0`) is identical:

```
ldrb r0, [sp,#0x38]      a local boolean
cmp  r0, #0
beq  <skip>              <-- if 0, does NOT assign
bl   0x81afbe0 ; bl 0x81b0030
ldr  r0, [r4,#0x948]
bl   0x8183d74           ASSIGNS
```

And that boolean is decided by `0x08150ad0`, which is passed its address
(`add r3, sp, #0x38 ; bl 0x8150ad0`):

```
8150ad4: sub  sp, sp, #1024      a 1 KB buffer on the stack
8150af4: strbne r9, [r8]         *flag = 0 on entry
8150af8: bl   0x82685a0          first check
8150b00: beq  0x8150b6c          if it fails -> exits with flag = 0
```

**There is the next step, now without ambiguity:** `0x08150ad0` sets the
boolean to zero on entry and only raises it if a chain of checks passes.
Knowing which one fails —starting with `0x082685a0`— names the condition that
keeps the player from asking for buffers again, which is the only link left.

## 11.110 THE EXACT CONDITION: the player's container is empty

Going down the last branch of `0x08150ad0` (§11.109) with gdb on a live
instance:

```
8150c10: blx r1            virtual call [[obj]+0x1b8]
8150c14: cmp r0, sl        r0 = 0  and  sl = 0  -> EQUAL
8150c24: beq 0x8150c3c     <-- jumps: "nothing has advanced", doesn't request a buffer
```

The method resolved live is **`0x0814fe60`**:

```
814fe68: add  r0, r0, #0x60      the container at [obj+0x60]
814fe6c: bl   0x82685a0          does it have elements?
814fe74: ldrne r0, [r4,#0x64]    if YES -> r0 = [obj+0x64]
814fe78: ldrne r0, [r0,#0x12c]   -> returns that field
814fe7c: moveq r0, #0            if NO  -> returns 0
```

And read on the machine (obj = `0x0ba954b4`):

```
[obj+0x60].. = 0x088ef3e4 0x00000000 0x00000000    the container
[obj+0x64]   = 0x00000000                          NULL
```

**The container is empty and there is no current element.** That's why the
method returns zero, the comparison gives equality, and the player concludes
there has been no progress and doesn't ask for buffers again.

**An explicit method caveat:** this reading was taken with the player ALREADY
closed, so by itself it could be describing the state after the abort and not
that of playback. What supports it is the §11.109 measurement, taken **during**
playback, where that same call returned 0 in the six observed executions. Both
point to the same place, but the late reading shouldn't be presented as proof
on its own.

**What remains:** why the `[obj+0x60]` container is empty while playing. That's
where it joins §11.99 —the audio structures nobody fills— but now with the
concrete object and the concrete field identified.

## 11.111 RETRACTION of §11.110, and the real chain down to the silence

§11.110 said the player's container was empty. **That is false**, and the
caveat the section itself had noted was the reason: that reading was taken with
the player already closed. Stopping at `0x0814fe70` —the `cmp` that follows the
validation, which is where the branch is decided— during playback:

```
[ 1..14] validation PASSES -> live reference; field [0x0ba6d5d8+0x12c] = 0x00000000
```

The weak reference is **alive** all fourteen times. And the object it points to
has `vtable = 0` and the tag `'plst'` at `+4`: it is a **playlist** object. That
branch was the track-change logic, not the buffer-fill logic. Dead end, and
noted as such.

### The place where the silence is decided

The byte the driver checks before filling (§11.63) lives at `0x0bb169dc`,
inside a descriptor with `0x0000ac44` = 44100 and 2 channels. Watching it:

```
WRITES byte -> 1   pc=0x0816bdd4  lr=0x081fec00
WRITES byte -> 0   pc=0x0816bdd4  lr=0x220055b4
```

**A method correction, and this is the third time this mistake costs a
conclusion:** from that I deduced "it's activated only once". False. A gdb
watchpoint only notifies when the value *changes*, and the immediate-send
branch writes −1 over −1 and 1 over 1. Counting EXECUTIONS with plugin probes:

```
decide 4   defer 4   submitnow 3   setavail 3   setavail2 1   gateraise 3
```

Four passes, not one. The rule of §11.66 —instrument the branches, not the
condition— now has a sibling: **count executions, not value changes.**

### Why it stops at four

The audio loop in IRAM:

```
22006324: bl 0x22003850     ; wait for render credit -> 0x0808af58
22006328: cmp r0, #0
2200632c: beq 0x22006310    ; 0 -> sleeps and retries
2200631c: cmp r7, #1
22006320: bgt 0x220061fc    ; on the second time, gives up
```

The model's own comment (`ipod_classic_sm1.c`, `SM1_IRQ_EVERY`) claimed that the
cap of 8 came from that wait **timing out**. **Measured, it's false:**

```
[1..8] the wait returned 0x00000001 -> SUCCESS
```

It is satisfied all eight times. The pump exits afterward, at `0x22006358`,
which requires three things at once. With all three read:

```
[sp+24]=0x00000000  OK      [r4+0x58]=1  OK      r0=0x00000000  <-- 2 is needed
```

`r0` is what `0x22003850`+8 returns → **`0x080a3c00`**, the getter for the next
render buffer. It returns 0 instead of 2 all eight times.

### The guard, and the state nobody maintains

`0x080a3c00` hangs off a global table at `0x0896a684`:

```
0x896a684:  0x00000000  0x2203ff28  0x2203ff00  0x22038000  0x22038100
              descriptor ^          bootinfo ^   slots ^      PCM ^
```

and it has two guards:

```
80a3c2c: tst r2, r0     ; r2 = enable mask, halfword at 0x2203ff28
80a3c30: beq exit       ; guard 1
80a3c4c: cmpeq r3, #1   ; r3 = slot state, halfword at 0x22038000+idx*4+10
80a3c50: bne exit       ; guard 2
```

Measured: **guard 1 cuts it off**, `mask=0x0000` with `bit=1`, all four times.

And with the plugin's `memw=` over the ENTIRE run (≈180 s, boot included), the
only PCs that write to `0x2203ff00..0x2203ff60` are:

```
32 x pc=0x0800f54c -> 0x2203ff00..07     8 x pc=0x08081da0 -> 0x2203ff00 (halfword)
 2 x pc=0x0800f52c -> 0x2203ff08, 0x0c
```

**Nobody ever writes `0x2203ff28`.** The slot array (`0x22038000`) is written,
but only once at boot (64 words from `0x08085ebc`) and never during playback.

### Proof by intervention

Forcing the mask at the guard point —without touching the firmware— guard 1
passes and guard 2 is reached **for the first time**, which reveals the second
wall:

```
[1..8] mask forced to 1  ->  guard 2: slot state = 0 (1 is needed) -> CUTS OFF
```

Forcing both, `0x080a3c00` hands over the buffer. The machine then goes black,
which is to be expected when handing over a buffer with invented state.

### What this names

The two fields that cut it off —the mask at `0x2203ff28` and the slot state at
`0x22038000`— are state that lives **in IRAM1** and that **no CPU code writes
during playback**. Our SM1 model is a register bank: it never writes memory.
The firmware sends it 519 COMMITs and 518 RUNs (§11.x) and then reads the
result in those structures.

That is: **the SM1 is not just a register peripheral, it is an engine that keeps
its state in IRAM1**, and that half of the model doesn't exist. That explains
why each guard that was forced revealed another beneath it: it wasn't a missing
bit, what's missing is the producer of all that state.

### What has NOT been proven

What exactly the engine's memory contract is. Inventing it field by field is
exactly what the `ipod_classic.h` header warns against —"filling them in from a
plausible assumption is inventing with the appearance of rigor"— and the two
guards already forced show there's more behind them. `SM1_POS_RING_FRAMES` was
not reduced: the test with `SM1_POS=24576` left the mask equal to zero and the
counters identical, so the ring wasn't the cause (although it's still true that
the write pointer runs off the end of IRAM1 and that `IRAMSPILL` covers it up).

**The music still doesn't play.**

## 11.112 THE SM1 PUBLISHES ITS STATE IN IRAM1 — render pump wall broken

The job descriptor the firmware hands to the engine, read live at
`0x08082194`:

```
descriptor @0x08ade494:  +0=0x08ad003c  +4=0x08ad003c  +8=1  +12=1  +16=0x38500100
chain      @0x08ad003c:  +0=0 (end)  +4=0x01a10f80  +8=0x88ad004c
buffer     @0x08ad004c:  ff fb 90 64 ...        <-- MP3 frame header
```

and the corresponding register:

```
0x38500100 = 0x08ad003d      the chain address | bit 0 = run
0x38500110 = 0x00000060      sub-block 4 = STOP (96)
```

That is: **the SM1 receives a chain of descriptors in SDRAM with MP3 frames**,
through offset `+0x00` of sub-block 4, which our model doesn't handle (it only
handles `+0x10` command and `+0x14` status).

### What CAN be grounded, and why

Sweeping the entire 4 KiB page, **no register contains `0x22038100` or
`0x22038000`**: the destination of the engine's output isn't programmed by
register and can't be deduced from anything measured. Modeling the decoding
would be inventing.

But the **buffer geometry isn't a hidden constant: the engine publishes it**.
The firmware doesn't carry it around, it reads it back:

```
80a3c54: ldrh r1,[desc,#4]        ; frames per slot    <- published by the engine
80a3c58: ldrh r0,[slot,#12]       ; samples in slot    <- published by the engine
80a3c68: add r1, pcm, r1,lsl#1    ; buffer = PCM + idx*frames*2
80a3c64: lsl r0, r0, #1           ; size = samples*2
```

So any coherent geometry that fits below the descriptors will do.
`SM1_SLOTS=1` publishes 2048 frames per slot (the audio DMA's own burst,
measured in the self-linked LLI) and 7 slots: 7 × 4096 = 28 KiB, ending at
`0x2203f100`, clear of `0x2203ff00`.

### Measured, with a control

Same navigation, same binary, the only difference is the variable:

| | without `SM1_SLOTS` | with `SM1_SLOTS` |
|---|---|---|
| `rendret` | 8 | **2505** |
| `bufok` (buffers delivered) | **0** | **2504** |
| `dpump` | 16 | **2505** |
| `framepull` (frames to the codec) | 1 | 1 |

The render pump goes from giving up after 8 invocations —the invariant number of
the whole project— to **running at ~38/s**, with 43/s as the real cadence. And visually:
without the change, at 18 s the device has already gone back to the *Songs* list; with it, it is still
in the player at 18 **and at 38**.

### The next wall, and it is not mine

`framepull = 1` in BOTH columns: the decoder is asked for **a single frame**,
the same with the change as without it. It is pre-existing (it was already noted as
"decoder single-frame one-shot") and is now isolated: the pump runs and fills
silence because nobody hands it frames.

**Honest regression:** `decide` (the send gate) drops from 8 to 1 and `setavail` from 7
to 0 with the change. It is not explained. That is why `SM1_SLOTS` stays **off by
default**: it is a measured advance, not yet a net gain.

**The music still does not play**, but the player no longer gives up.

## 11.113 RETRACTION: the 32768 peak did not come from the guest path

In §11.112 I accepted that, with the engine filling the ring from the
host decoder, **the WAV peak went from 11444 to 32768** and that this
proved that genuine PCM was flowing through the guest's own path.

**It is false.** The control refutes it unambiguously — same decoding, the only
difference my change:

```
HLE_AUDIO_DATAPATH only            21.058% non-zero   peak 32768   (segments 16..20 full)
+ SM1_SLOTS filling the ring        0.20-0.47%        peak 11444
```

The 32768 came from the **host shortcut**, which keeps feeding the DMA until my
first slot publication turns it off with `hle_np_owns_feed`. That is: not only
did my path not contribute it, my change **removed** the audio that did work.

The PCM fill is retracted. And with it retracted, the combination still gives
0.258%: with the render pump alive, the guest's render **writes silence
over** the PCM that the shortcut leaves ready in the same DMA buffer. The two
mechanisms compete for the same destination, and the one with real data only wins
while the pump is dead.

### What does still stand

`SM1_SLOTS` publishes the engine state in IRAM1 and that **does** revive the render
pump, with a control and repeatedly: `rendret` 8 → 1343 (~20/s, the exact
slot cadence at 44100 Hz) and the player stops aborting at 5-18 s; it is still
open at 38. That is real and measured.

### The remaining wall, unembellished

Reviving the pump only helps if the pump has audio to move, and its audio is produced
by the firmware's own decoder, which **runs only once**. That is the same
wall the project already had, now isolated: it is no longer hidden behind the pump's.

**The music does not come out of the native player.**

## 11.114 With the pump alive: the whole pipeline works and the producer delivers zeros

With `SM1_SLOTS=1` the pump finally walks its success branch (`0x22006370`), which had
never executed. That is where it requests the audio:

```
22006464: bl 0x22003808          ; -> 0x081db410
22006474: bl 0x22003810          ; -> 0x081db4c8   REQUESTS audio, output in [sp+52]
22006478: cmp r0, #0
22006480: bne 0x22006500         ; if it fails, the pump exits
22006494: blne 0x22000020        ; otherwise, COPIES source -> slot, r5*4 bytes
```

Measured during playback:

```
of 300 requests: 300 with data, 0 failed
[ 1] r0=0  samples=1024  source=0x08a873c0  dest=0x22038100
[ 2] r0=0  samples=1024  source=0x08a8b3c0  dest=0x22038100
     ... alternating two source buffers, double buffer in SDRAM
```

And the slot index rotates as it should, modulo the counter the model publishes:

```
[1] modulo->1  [2] ->2  [3] ->3  [4] ->4  [5] ->5  [6] ->6  [7] ->0   counter=7
```

That is: **the whole pipeline works**. The guard delivers buffers, the index rotates
through the seven slots, the audio request never fails, the copy is made.

### And yet

```
[  3] source=0x08a873c0  samples=1024
0x8a873c0:  0x00000000 0x00000000 0x00000000 0x00000000 ...
[ 60] source=0x08a8b3c0  ... all zeros
[150] source=0x08a8b3c0  ... all zeros
```

**The producer delivers 1024 samples of silence, every time.** It does not fail, it does not give up,
it does not run out of credit: it returns success and returns zeros.

This replaces "the decoder runs only once", which relied on badly
named probes (`framepull` 0x081e93d8 turned out to be a notification ring drain,
not the codec). The correct, measured formulation is: **everything below the
producer works; the producer, `0x081db4c8`, delivers silence.**

The codec's output buffers are `0x08a873c0` and `0x08a8b3c0` in SDRAM, double
buffered. That is the next target, and it is concrete: who should write to them.

**The music does not come out of the native player.**

## 11.115 SM1_DECODE: the right place, and a control that refutes it all the same

The producer `0x081db4c8` **is not a decoder**: it is a dispatcher of two fixed
buffers, which always returns success.

```
81db4f0: ldrne r1, =0x08a873c0     ; two addresses, literals
81db4f4: ldreq r3, =0x08a8b3c0
81db504: str r1,[r2]               ; hands out one
81db510: mov r0,#0                 ; and always reports success
```

The firmware copies 1024 stereo frames from there (`r5*4` = 4096 bytes, measured). So
those two buffers are the REAL destination where the engine must leave its PCM — not the
IRAM1 ring that §11.113 tried and failed. `SM1_DECODE=1` fills them.

In addition, two errors of my own corrected with measurement:

- **Factor of 2 in the rate.** A slot is `SM1_SLOT_FRAMES` 16-bit *samples* =
  4096 bytes, which in stereo is 1024 **frames**, and `pos` counts frames. Dividing by
  2048 released slots at half the rate at which they are consumed.
- **The position came from the wrong place.** With decoding, `sm1_stream_pos`
  took the host's cursor instead of the I2S that the guest drives. Measured: the
  pump froze at ~44 activations even with a 150 s window.

With both corrected, the measurement is spectacular... and misleading:

```
without SM1_DECODE   0.258% non-zero   peak 11444   rendret   44
with SM1_DECODE     28.754% non-zero   peak 32768   rendret 1699   (segments 15-20 continuous)
```

### The control that refutes it

`SM1_FEED_OFF=1` turns off the host shortcut, leaving the guest path as the
only possible route for PCM to the speaker:

```
28.754%  ->  0.181%      peak 32768 -> 11444
```

**The music came from the shortcut, not from the guest path.** The 28.75% versus the shortcut's
21.06% is only explained by `SM1_SLOTS` keeping the player alive longer,
so the shortcut feeds for longer -- not by the guest contributing anything.

`SM1_DECODE` does put the PCM where the guest reads it, and the pump does copy it to the
slots. What does not happen is the **last hop**: from the IRAM1 slot to the IRAM0 DMA
buffers, which the output driver performs and which is conditioned on the
"audio available" byte (§11.111). That byte is raised by the send path, and `decide` is still
1.

**That last hop is all that remains. The music does not come out of the native player.**

## 11.116 Where the chain stands, and the only gap that remains

With `SM1_SLOTS` + `SM1_DECODE`, measured during playback:

| link | state |
|---|---|
| "audio available" byte (0x0bb169dc) | goes to **1 and stays at 1** -> the output driver fills |
| dispatcher `0x081db4c8` | delivers a buffer, **300/300 successful** |
| render pump | **1699 activations**, ~26/s |
| slot index | rotates 0..6 modulo the published counter |
| source -> slot copy | is made, 4096 bytes per round |
| **content** | **zeros, in source and in slot** |

That is: **the whole chain works and carries silence.**

And the gap is narrowed down to one point: the codec's two output buffers,
`0x08a873c0` and `0x08a8b3c0`, are filled by the firmware itself with a `memcpy`
(0x22000198, 284 writes measured) and what it copies is zeros. So much so that
**it even overwrites the PCM that `SM1_DECODE` leaves there** -- checked: with the
variable active, the buffer still reads as zero at requests 40 and 120.

So the real decoder is upstream of that `memcpy`, and it produces silence.
Locating it with a breakpoint is not feasible: `0x22000198` is the runtime's `memcpy`, it
runs constantly, and three passes were lost because it freezes the interface and the
play press does not get through. With the plugin's `sendex=` (register filter, without
stopping the machine) another two passes were lost for the same reason: with `SM1_DECODE` +
real decoding the machine is so loaded that the harness cannot press. **That
is a harness limitation, not a result**, and it is worth noting before someone
reads it as "there is no copy".

### Session balance

- **`SM1_SLOTS`** is a real advance, attributed with a control: render pump 8 -> 1343
  (~20/s, the exact slot cadence at 44100 Hz) and the player stops aborting at
  5-18 s -- it stays open at 38, with captures.
- **`SM1_DECODE`** puts the PCM in the correct, measured place, but the firmware overwrites it;
  and its apparent audio gain (28.75%) was refuted by `SM1_FEED_OFF` (-> 0.18%).
- Five fix attempts: one standing, four retracted or refuted by their
  own controls.

**The music does not come out of the native player.**

## 11.117 MUSIC COMES OUT through the player's own path

I had the copy's direction backwards in my reading, and that was all. The caller of
the buffer hand-off is `lr=0x220064f4` -- **the pump itself**:

```
22006488: ldrne r0,[sp,#52]   ; destination = the codec buffer it just acquired
2200648c: ldrne r1,[sp,#28]   ; source      = the IRAM1 slot
22006494: blne 0x22000020     ; slot -> codec buffer
220064f0: bl  0x081db520      ; and hands it off  (lr = 0x220064f4)
```

The codec buffers are the pump's **output**, not its input. The audio has to
already be in the **IRAM1 slot** -- which is what "ready" meant from the
start (§11.112). Filling the codec buffers (§11.115) meant writing at the
far end of the copy, where the pump immediately overwrote it with the empty
slot: that is why they kept reading as zero.

`SM1_DECODE` now fills the slot. And with `SM1_FEED_OFF=1`, which turns off the host
shortcut and leaves the guest path as the **only possible route** for PCM to the
speaker:

```
fill in the codec buffer        0.181% non-zero   peak 11444
fill in the SLOT                5.676% non-zero   peak 32768   (repeated: 5.632%)
                                                  segments 19-20 with continuous music
```

Same indicators, same navigation, the only thing that changes is the PCM destination.
**The music comes out through: engine -> IRAM1 slot -> render pump -> codec buffer ->
output driver -> DMA -> I2S. All of that is the retailOS firmware.**

It is the same place the first attempt pointed at, and it measured worse back then for three
reasons since corrected: the release rate was off by 2, the position
came from the host's cursor instead of the I2S, and the shortcut was turned off without the pump
being alive to replace it.

### Why it starts late

The music appears in the last ~10% of the capture because the decoder takes time to
have PCM: the log says `decoded track 3 -> 52187136 PCM bytes (295.8 s)`, and until
that finishes `have_pcm()` is false and the slot is filled with nothing. It is not a fault
of the path: it is the emulated codec's startup latency.

### The harness, fixed along the way

`press` verifies each attempt by comparing captures, and with this configuration the machine
is so loaded that the capture fails and the press is lost -- **five passes
lost this way**. `seq ret:120` sends the key without a capture and does not depend on the load; the
entry is confirmed with the `StartPlayback` counter, not with an image.

## 11.118 The "Low Battery" screen, and the player that freezes

### The battery warning (RESOLVED)

The user reported that the device showed a battery screen and switched off.
Reproduced and captured: it is **"Low Battery -- Connect to Power"**, exactly the
symptom that `ipod_classic_pmu.c` considered unresolved ("raising the battery ADC to
full does NOT stop retailOS from showing it, so the verdict does not come from the channel this
model answers").

With `PMUTRACE=1` you can see that the ADC **is read correctly**: `0x55 = 0xd3` = 211, which is the
844 of 1023 that the model publishes. So the voltage is not the problem. What happens
is that `MBCS1_HEALTHY = 0x00` describes "on battery, no charger", and the firmware is
asking precisely to be plugged in -- and nothing ever tells it that it is.

`PMUCHARGER=1` reports an adapter present and healthy. **Measured:** the screen
disappears, the battery icon turns green with the charging bolt, and the device stops
switching off. It is a modeling choice, not a measurement, and that is why it sits behind a variable: an
emulated iPod has no cell to discharge, so "in the dock" describes it at least
as honestly as "with a battery that never moves".

### The end-of-frame notification (PARTIAL)

The LCD block has an interrupt line connected to `S5L8702_LCD_IRQ` and an `INTCON`
at +0x18 that the model stored and did not use: **the line had never been raised**.
`LCD_FRAME_IRQ=1` pulses it at the start of each frame.

Measured: **the player entry animation goes from freezing to completing.**
Without the variable, layer 1 stays stuck at 320x240 at (0,0); with it, it reaches
`width=-80, x=400`, which is the end of the sweep documented in the compositor. But it
is not enough for the player screen to appear, and in some passes it leaves the
audio pump at 3 activations instead of 1451, so **it is not resolved**.

### What remains, narrowed down

With `PMUCHARGER=1` the audio plays fine (21% non-zero samples, peak 32768,
`rendret` 1577) while **the interface stays frozen on the generic cover art**:

- SELECT, MENU and the wheel do not change a single pixel -- checked with hashes of the
  captures, five presses in a row, identical image.
- The CPU is asleep in the idle loop's WFI, not spinning.
- The disk keeps reading the song and the audio keeps coming out.

That is: **it is not a drawing fault, it is the interface task blocked**, waiting for something
that does not arrive, while the rest of the system stays alive. With the compositor's five
surfaces dumped, none contains a playback screen with a progress bar:
layer 0 the clock, layer 1 the cover art, layer 2 the list with the speaker icon. The
firmware never draws it.

**Seeing the complete player has not been achieved.**

## 11.119 THE COMPLETE PLAYER: the compositor had the layers backwards

The Now Playing screen **was always being drawn**. We were covering it.

With the surfaces dumped while the player was open, layer 0 contains the whole
player -- "Now Playing" bar, artist, album, "4 of 4" and the progress bar -- and
layer 1 the cover art, opaque and full-screen. The compositor drew back to
front starting with slot 0, so the cover art erased the player and
left exactly the white right side that suggested a half-drawn
screen.

And there is a second half of the same error: **the model discarded the alpha channel**. The
32-bit layer really carries it -- dumped while the player is on
screen, its alpha byte takes **243 distinct values**, which no "ignore it"
reading survives.

`DISP_ALPHA=1` composites front to back with *source-over*. That explains **both**
things at once: the player ends up on top of the cover art, and the language menu
sweep -- which is where the original Z order was deduced from, assuming opaque layers --
keeps working because the incoming panel is transparent where the outgoing one
must show through. Checked: the language menu and the lists are drawn the same as before.

```
Now Playing                    [>] [battery charging]
+-----------+   Billie Jean
| cover art |   Michael Jackson
|           |   Greatest Hits
+-----------+   1 of 4
 0:00  [============================]  -4:55
```

### The ring was too big

With `SM1_DECODE` the interface froze halfway through entering while the audio kept
playing. Cause: we publish 7 slots, i.e. 28 KiB from `0x22038100`, and **writing
PCM over that whole range clobbers memory that is not ours**. Publishing the state is
harmless; writing 28 KiB is not. `SM1_NSLOTS` allows sweeping it, and with **2
slots** the interface holds up and the audio keeps coming out.

**Result, all at once and measured:** full player screen with song
name, artist, album, track N of M and progress bar; audio 12.89% non-zero
samples with peak 32768 and the host shortcut OFF; and the player does not close.

### What is missing

The elapsed time stays at **0:00**. Making the SM1 position register
follow the I2S (as `sm1_stream_pos` already does) does **not** move it, so that counter does not
come from there: it is kept by the player's internal bookkeeping. It is the only thing left
for the bar to advance.
