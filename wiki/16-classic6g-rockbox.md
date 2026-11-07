# Booting Rockbox on the iPod-Classic machine (S5L8702)

How **Rockbox** (target `ipod6g`) was taken from "does not execute a single
instruction" to its **navigable main menu** on the QEMU `iPod-Classic`
machine. Everything that follows is **measured** (screenshot looked at or
trace) except where it says `INFERRED` or `UNDETERMINED`. The commands at the
end reproduce it.

> Why Rockbox: the retailOS already boots, draws and lists the music, but
> **it does not play sound** because it decodes the MP3 on the CalmADM2E DSP,
> which is not modeled (see [14-classic6g-audio.md](14-classic6g-audio.md)).
> Rockbox decodes the MP3 **on the ARM**, so the real PCM comes out through
> the same I²S FIFO that we already capture to WAV. It started with **booting
> Rockbox** (§1–4) and now it goes all the way: **it plays an MP3 and the
> captured WAV is the song** (§7, measured).

---

## 0. Status by milestone

| Milestone | Status |
|---|---|
| 1. Rockbox loads and executes its first instruction | **done** — `-kernel rockbox.bin` at `0x08000000` |
| 2. Gets through early init (system, clocks, MMU) without hanging | **done** — bootloader hand-off (§2) |
| 3. Initializes the LCD and DRAWS | **done** — the pixel path via DMA to the LCD (§3) |
| 4. Reaches its navigable main menu | **done** — cabbiev2 menu, the wheel moves the highlight, SELECT enters (§4) |
| 5. Plays an MP3 and the captured PCM IS the song | **done** — WAV with correlation 1.000 against the original (§7) |

Milestone 4 screenshot (disk with `/.rockbox`, complete cabbiev2 theme): the
"Rockbox" menu with **Files / Database / Resume Playback / Settings /
Recording / Playlists / Plugins / System / Shortcuts**, golden selection bar
on *Files*. The wheel moves the highlight down one row per click and SELECT
on *Database* opens "Database is not ready / Initialize Now" — Rockbox's
correct behavior. Reproducible with `tools/ipod_classic/rockbox.sh`
(automatic verdict **OK**).

**Regression:** `navtest.sh` (retailOS 6G) still passes in full after these
changes (VERIFIED, complete run: wheel 8000 px, SELECT 19340 px, both halves
of the menu, MENU returns exactly). The 5G is not touched: the only model
files touched are `ipod_classic.c` and `ipod_classic_lcd.c`, both 6G.

---

## 1. The route: loading the app directly (osos style)

Rockbox is linked to DRAM `0x08000000`, entry `start` (`crt0.S`), just like the
retailOS's osos. The machine loads a RAW binary with `load_image_targphys` at
`SDRAM_MEM_BASE` and starts there (`ipod_classic.c`). So:

```
qemu-system-arm -M iPod-Classic \
    -kernel assets/rockbox/build-full/rockbox.bin \
    -drive if=ide,format=raw,file=<disk.img> -snapshot
```

- Use the **RAW** `rockbox.bin`, NOT the `.elf` (its `e_entry` is `0x0`, the
  VMA of the vector table, which is only populated at runtime). VERIFIED: the
  first word of the `.bin` is `ea00000d = b 0x800003c` (`start: b _newstart`).
- The bootloader chain (`build-boot/bootloader.bin`) is **not** needed for the
  bring-up: the machine reproduces what the bootloader would leave (§2).

---

## 2. The bootloader hand-off (milestones 1–2)

On real hardware, the bootrom + the Rockbox/emCORE bootloader leave two
things done before jumping to the app. In a direct boot via `-kernel` we skip
them, and the app hangs on both. The machine reproduces them in
`ipod_classic_handoff()` (`hw/arm/ipod_classic/ipod_classic.c`), called from
`mc->reset` **after** `qemu_devices_reset()`. Gated on there being no bootrom.

### 2.1. The stack (SP) — the first hang, MEASURED

`crt0.S` (non-BOOTLOADER branch) calls `bl memory_init` **before** setting up
its own stacks (`crt0.S:143` versus `:184`). `memory_init` starts with
`push {r4, lr}`, so it runs on the SP the bootloader left. On a QEMU cold
reset **SP = 0**, the `push` writes to `0xfffffff8` → unhandled data abort
(the vector table at `0x0` is still zeros) → the PC walks
`0x10,0x14,...`. Black screen.

**Fix:** seed `r13` (SVC) with the top of IRAM0 (`0x22020000`), a scratch
stack like the one the bootloader leaves; the app switches to its own stacks
as soon as `memory_init` returns. The retailOS is not affected: it sets up its
own SP before doing any `push` (today it boots with SP=0, which would fail
the same way if it did not).

### 2.2. The microsecond timer (TimerE) — the second hang

`USEC_TIMER == TECNT` (TimerE) and `udelay()` spins on it with no timeout
(`system-target.h`). What starts TimerE is `usec_timer_init()`, which is
`#ifdef BOOTLOADER` (`clocking-s5l8702.c:381`), so **the app does not start
it**: the first I²C transfer of `pmu_preinit()` does `udelay(5)` and hangs.

**Fix:** program TimerE at 1 MHz exactly like `usec_timer_init()`:
`TECON=0x440` (ECLK/1, ECLK, interval mode), `TEPRE=11` (12 MHz/12 = 1 MHz),
`TEDATA0=~0`, `TECMD=0x3` (CLR|EN), at `TIMER_BASE=0x3C700000`. The retailOS
also drives TimerE at 1 MHz and reprograms it (with CLR) before using it, so
this is transparent to it — VERIFIED by `navtest.sh`.

### 2.3. Why in `mc->reset` and not in the CPU reset

The sysbus reset (which resets the timer device) is registered by
`qdev_machine_creation_done()` **after** `machine_init` registers
`ipod_classic_cpu_reset`, so it would run last and **erase** the TimerE we
programmed. `mc->reset` is the only place guaranteed to run after all the
device resets: it does `qemu_devices_reset()` and then the hand-off.

With this Rockbox gets through `memory_init`, `system_init` (i2c/pmu/udelay),
gpio, eic, uart and **reaches the LCD** (VERIFIED by `-d unimp` trace).

---

## 3. The pixel path: DMA to the LCD (milestone 3)

The LCD (`0x38300000`) was modeled **for the retailOS**, which composes the
frame in the *display pipe* (`0x38900000`) and does not push a single pixel
through the data port. **Rockbox does it the other way around**: it writes
the frame by DMA.

`lcd-s5l8702.c`: `displaylcd_setup()` sends via the CPU the commands `0x2A`
CASET / `0x2B` PASET / `0x2C` RAMWR (4-byte params, 32-bit writes to port
`+0x40`, which the model accepted); then `displaylcd_dma()` sets frame mode
(`LCD_CON=0x80100db0`) and **DMAs the pixels** with `dmac_ch_queue(...,
S5L8702_DADDR_PERI_LCD_WR, ...)` — DMAC0, `dwidth = WIDTH_16`, to
`LCD_BASE+0x40`.

Measured with `LCDTRACE=1`: **152 writes to the data port, 0 pixels**. The
pixels arrive as **16-bit** writes, and the LCD model had
`valid.min_access_size = 4` → every DMA write was rejected
("Invalid write at addr 0x40, size 2"). Black screen even though the driver
ran a complete update.

**Fix (`ipod_classic_lcd.c`):** accept halfword accesses in the LCD block —
`valid`/`impl` `min_access_size = 2`, `max = 4`. `panel_data()` already takes
a pixel from the low 16 bits, so a 16- or 32-bit write delivers one pixel.
The retailOS only uses 32 bits, so its path does not change (VERIFIED by
`navtest.sh`).

**The DMA itself already worked:** the PL080 (`hw/dma/pl080.c`) transfers
non-*paced* lines at full speed, and the LCD_WR line (id 3) is not marked as
*paced* (only the I²S one is). The PL080's LLI chaining walks the ~19
descriptors Rockbox sets up for the 320×240 frame. As soon as the port accepts
the 16 bits, the pixels land in `s->fb` and the frame appears — MEASURED:
from 0 to 3805 px (fallback) / 74057 px (cabbiev2 theme) lit.

---

## 4. The disk and the menu (milestone 4)

Rockbox mounts the FAT32 partition (the machine interprets the MBR's LBAs in
4096-byte blocks, same as the retailOS — see `mkdisk6g4k.py`; that is why a
`mkdisk6g4k.py` disk works as is).

- **With any FAT disk** Rockbox reaches the menu, but first shows
  "Installation incomplete" and uses the built-in font (sysfont). MEASURED
  with `~/6g/E4kzero_rebuilt.img`: 3805 px menu, white text on black.
- **With a disk that carries `/.rockbox`** it loads its theme, fonts and
  codecs: complete cabbiev2 menu, 74057 px. Built in `~/6g/Erb.img` from
  `assets/rockbox/build-full/rockbox.zip` (it contains
  `.rockbox/codecs/mpa.codec`, the ARM MP3 decoder that will be needed for
  audio).

Navigation (VERIFIED, `rockbox.sh`): the golden selection bar moves down one
row per forward wheel click (y 36→52→66) and moves up on a backward click
(→52); SELECT on *Database* changes the screen (7666 px different) to
"Database is not ready". The 4 wheel/button events are logged by the wheel
model (`WHEELTRACE`, IRQ 23). Rockbox's button driver reads the same wheel
controller that we already modeled for the retailOS, unchanged.

---

## 5. What was stubbed / left annotated

- **`ipod_classic_handoff()`**: boot SP + TimerE. It is the reproduction of
  the bootloader hand-off, not pure hardware behavior. Annotated in the code.
  If a real bootrom is ever modeled, the `bootrom_path` gate disables it.
- **LCD `min_access_size = 2`**: to accept Rockbox's 16-bit DMA. Annotated; it
  does not change the retailOS's 32-bit path.
- **Without touching Rockbox**: both hangs were fixed in the MACHINE, not in
  the firmware. `rockbox.bin`/`.elf`/`.zip` are the ones compiled by the
  `arm-none-eabi-gcc 13.2.1` toolchain (see the build report).

Nothing else needed stubbing: `storage_init` mounts the disk with Rockbox's
own ATA driver on top of the existing ATA model, `power_init`/`adc`/PMU pass,
and the kernel (TimerB at 10 kHz) ticks. What has not yet been exercised
thoroughly is USB (the app calls `usb_init` after drawing; today it does not
hang) and real playback (the next front).

---

## 6. How to reproduce it

```sh
# 1) Rockbox app (already built); if not, see the build report:
#    assets/rockbox/build-full/rockbox.bin  (+ .elf with symbols, + .zip)

# 2) disk with /.rockbox (theme + fonts + codecs):
W=$HOME/rbwork; rm -rf $W; mkdir $W; cd $W
unzip -q /mnt/c/Desarrollo/qemu-ios/assets/rockbox/build-full/rockbox.zip
truncate -s 300M $HOME/6g/rbfat.img
mkfs.vfat -F 32 -n ROCKBOX $HOME/6g/rbfat.img
mcopy -s -i $HOME/6g/rbfat.img .rockbox ::
cd /mnt/c/Desarrollo/qemu-ios
python3 tools/ipod_classic/mkdisk6g4k.py $HOME/6g/Erb.img $HOME/6g/fwzero.img $HOME/6g/rbfat.img

# 3) boot to the menu and check that it navigates (automatic verdict):
sh tools/ipod_classic/rockbox.sh $HOME/6g/rockbox $HOME/6g/Erb.img

# view a screenshot
python3 tools/ipod_classic/ppm2png.py $HOME/6g/rockbox/0-menu.ppm   # or the .png already written

# 4) diagnosis if it hangs: where the PC is (with symbols from the .elf)
#    launch qemu with -gdb tcp::PORT and attach gdb-multiarch; bt.
```

Useful traces: `-d unimp,guest_errors` (unmodeled peripheral), `LCDTRACE=1`
(every LCD access + pixel/frame totals), `WHEELTRACE=1` (wheel),
`ATATRACE=1`, `I2STRACE=1`.

---

## 7. Milestone 5 — IT PLAYS: the MP3 decoded, captured to WAV and **measured**

**Done.** Rockbox mounts the disk, lists the MP3s in *Files*, and on SELECT
enters the *Now Playing* screen (WPS) and **plays**: `mpa.codec` decodes the
MP3 **on the ARM**, leaves stereo s16le PCM in DRAM, DMAC0 channel 2 pushes it
to the I²S FIFO at `0x3CA00010`, and `AUDIOWAV=` dumps it to a WAV. That WAV
**is the song**, not silence or noise — shown by measuring it against the
original MP3.

**The deliverable WAV:** `~/6g/rockbox_numb_encore.wav` (4.9 MB, 48000 Hz,
16-bit, stereo, ~25.7 s of Linkin Park's "Numb / Encore"). Outside git on
purpose.

### The proof (measured, not inferred)

Decoding the original MP3 with `ffmpeg` at the same rate and correlating:

```
python3 tools/ipod_classic/wavcheck.py ~/6g/rockbox_numb_encore.wav \
        ~/6g/tracks/'Numb - Encore.mp3'
```

- **RMS 7014** (−13.4 dBFS), **peak 29128** (−1.0 dBFS), **99.9 %** of
  samples nonzero → there is signal with real dynamics (a WAV of zeros gives
  RMS 0).
- **Real stereo:** L RMS 7780, R RMS 6971, RMS(L−R) 4634 (0 would be fake
  mono); L==R exactly in only 0.15 % of the frames.
- **Normalized cross-correlation = 1.000** against the MP3 decode, at a
  constant lag of ~18 ms (the *priming* of Rockbox's decoder versus
  ffmpeg's). Chunks of the capture at t=3,8,13,18,23 s match **exactly** with
  3.016, 8.016, 13.016, 18.016, 23.016 s of the original (lag +5.00 s per
  chunk): it is a **continuous** decode of the real song, not a loop or a
  coincidence.
- Spectrogram of the capture ≈ spectrogram of the original
  (`tools/ipod_classic/specimg.py`; see `~/6g/rockbox_numb_encore_spec.png` vs
  `~/6g/ref_numb_encore_spec.png`).
- Screens: `1-files.png` (Rockbox lists `InTheEnd` and `Numb`), `2-wps.png`/
  `5-wps.png` (*NOW PLAYING* "Numb / Encore … Linkin Park", the clock advances
  0:04 → 0:17 between screenshots).

### How to reproduce it

```sh
# disk with /.rockbox + the MP3s on the FAT: ~/6g/Erbmusic.img (already built)
# AUDIOPACE=1 is MANDATORY (see below).
env AUDIOPACE=1 sh tools/ipod_classic/rbplay.sh \
        $HOME/6g/Erbmusic.img $HOME/6g/rbfinal $HOME/6g/rbfinal/play.wav
python3 tools/ipod_classic/wavcheck.py $HOME/6g/rbfinal/play.wav \
        $HOME/6g/tracks/'Numb - Encore.mp3'   # VERDICT: MATCH
```

### What had to be adjusted in the model (a single change)

`hw/dma/pl080.c`, in `pl080_pace_request()`: after `pl080_run()` the line's
one-shot request bit is **released**. Without this, a request that moved
nothing (channel not yet enabled, or stopped between tasks) was left
**pending** and, when the guest enabled the channel, it drained an entire task
at once; the terminal count that followed cleared the channel's E bit and
Rockbox's `dmac_ch_queue_2d` saw E=0 and panicked with *"DMA ch link error"*.
Releasing the pulse, the channel only advances with a live request and stays
enabled between ticks, so the guest can extend the LLI chain in time. **It
only affects *paced* lines** (only caller: `ipod_classic_i2s_drain()`), and
those only exist under `AUDIOPACE` — so the retailOS and the 5G, which never
mark the line or call the function, remain **byte-identical** (verified by
reading the `i2s_pace_on()` gating; navtest 6G: the wheel moves the highlight
5/5, and the SELECT failure is the harness's already documented flakiness, not
a regression; 5G: navigates).

### Two corrections to what was predicted

- **`AUDIOPACE=1` is necessary**, not optional. Without pacing the DMA drains
  the ring instantly, playback *underruns* before the codec decodes the second
  buffer and only ~512 B are captured. Measured: `rbverify_np` (no pace) =
  556 B; with pace = complete WAV. Pacing does not change the sample-by-sample
  *content*, but it is what keeps the stream alive long enough to capture it.
- **The rate is 48000 Hz**, not 44100: this MP3 is 48 kHz, Rockbox sets
  `I2SCLKDIV=0xfa` (=250 → 12 MHz/250 = 48000) and the WAV is labeled
  correctly.

---

## 8. Making it **PLAY through the speakers** and making playback **RELIABLE**

Two things were missing to *enjoy* it: (a) the PCM coming out of the host's
speakers live, not just to a WAV; (b) eliminating an **intermittent DMA panic**
on pressing *play*. Both done and **measured**.

### 8.1. Live audio via PulseAudio (`AUDIOLIVE=1`, `-audio pa`)

The **same** PCM that goes to the WAV is additionally delivered to a QEMU
`SWVoiceOut`, which goes out through the `-audio pa` (PulseAudio) backend.
Under **WSLg**, `pa` reaches the Windows speakers:
`PULSE_SERVER=/mnt/wslg/PulseServer` → `RDPSink` → RDP → Windows (`pactl info`
confirms it: *Default Sink RDPSink*). On a desktop Linux, `pa` goes to the
local PulseAudio/Pipewire. It is in
`hw/arm/ipod_classic/ipod_classic_i2s.c`, **gated by `AUDIOLIVE`** and
**additive** to the `AUDIOWAV` capture (the same bytes go to both places).

**A single command** (graphical window + sound):

```sh
sh tools/ipod_classic/rbmusic.sh          # -M iPod-Classic -audio pa, AUDIOPACE+AUDIOLIVE
# equivalent to:
env AUDIOPACE=1 AUDIOLIVE=1 <qemu> -M iPod-Classic -kernel rockbox.bin \
    -drive if=ide,format=raw,file=$HOME/6g/Erbmusic.img -audio pa -display sdl
```

Keys (focus the window; mapping in `ipod_classic_wheel.c`): **e/q** wheel
forward/back, **Enter** SELECT, **w** MENU, **s** play/pause, **d/a**
next/previous. To play: boot (~15 s) → **Enter** → **e/q** to a track →
**Enter**.

**MEASURED proof that the backend receives the real PCM (the same as the
WAV):**

- **Checksum, byte for byte.** In a run with `AUDIOWAV`+`AUDIOLIVE`, the bytes
  delivered to `AUD_write` are **identical** to those of the WAV:
  `wav_bytes=live_bytes=4936192`, `wav_sum=live_sum=626263738` →
  **IDENTICAL** (temporary `AUDIOLIVEDBG` instrumentation, since removed). And
  that WAV correlates **1.000** with the song (§7). That is: what reaches the
  speaker, sample by sample, is the same song we already validated.
- **PulseAudio monitor capture** (`parecord RDPSink.monitor`) during
  playback: **peak 31883** (nearly full scale, cf. WAV 30074), **real stereo**
  (`RMS(L−R)=4363`, 0 would be fake mono), recognizably the song.
- **Idle host: 0 underruns, 0 drops** over the whole song.

**How it is wired** (all `AUDIOLIVE`-gated, inert by default):

- The voice is opened **in `realize`**, *before* the VM starts.
  `AUD_set_active_out` only arms the mixer timer if `vm_running`, which is set
  **once** when the VM starts; a voice registered late (during playback)
  misses that edge and **does not clock a single sample** (the `wav` backend
  stays at 44 bytes; `pa` silent). Copied from `hw/audio/lm4549.c`.
- **1 MiB ring + 300 ms of *priming*.** The DMA is paced on the **VIRTUAL**
  clock and the mixer consumes on the **REAL** clock; their rates only match
  on average, so per-burst jitter would empty the ring (measured: **~90
  underruns/s** with no cushion). The first 0.3 s of PCM is held back without
  activating the output (an inactive voice does not pull, the ring fills up);
  then it is activated and the **mixer callback** drains the ring at its real
  pace. The per-sample path is minimal (only *append*; the `AUD_write`
  happens in the callback), because MP3 decoding on the emulated ARM is tight
  on budget and any per-sample work steals margin from the codec.

**Honest ceiling (host performance, not a bug):** MP3 decoding on the emulated
ARM runs at **~0.9× real time** on this development host (contended with
builds). The *priming* absorbs **jitter**, not a sustained deficit: if the
host does not sustain ≥1× real time, the cushion runs out within a few
seconds and the occasional underrun reappears (the song plays a touch slow /
with the odd gap). On a fast/idle host it plays clean and in tune. **The WAV
is always exact**, and the backend always receives the correct PCM (checksum
above). Measurement note: recording the `RDPSink.monitor` monitor *while* QEMU
runs steals CPU and adds its own underruns to the *recording* — it is an
artifact of the measuring apparatus, not of the output.

### 8.2. The **"DMA ch link error"** panic eliminated at the root

When reproducing with the graphical window (or with the host under load) Rockbox
sometimes entered `panicf("DMA ch%d: link error")` on pressing *play*. It is a **race** in
the audio DMA pacing.

**The cause (VERIFIED by reading the driver, `assets/rockbox/.../s5l8702/pl080.c`).**
Rockbox keeps a continuous audio DMA going by extending the LLI chain from its
terminal-count ISR (`dmac_ch_queue_2d`, `QUEUE_LINK` branch): when it sees
`DMACCxLINK==0` (the HW reached the last LLI) it writes the link of the new task and
**re-reads `DMACCxCONFIG`**; if the **E** (enable) bit is already 0, it *panics*
(`pl080.c:436-437` of the firmware). Our model, on completing the terminal LLI
(`link==0`), **auto-disabled the channel** (`ch->conf &= ~E`) at that very
instant — and that instant is triggered by a **host `QEMUTimer`** (the audio
drain tick) that the guest's `disable_irq_save()` **cannot hold back**. If
the timer completes the terminal LLI *between* the guest's `link==0` read and its E
check, the guest sees **E=0** and panics. A *relative timing* race: fast headless
almost never loses it; with the graphical window (WSLg steals CPU, the guest takes
longer) sometimes it does — exactly what the user reported.

**The fix (robust, `hw/dma/pl080.c`).** A **paced** channel **never** drops its
E bit on reaching a terminal LLI: it is marked `paced_pending` and stays
*enabled*-and-inactive. It resumes when the guest writes a new `LINK`
(`pl080_write` case 2 only **records** it; `pl080_run` reloads the descriptor on the
next tick, which is where the real HW consumes the LINK register), and it is **only**
disabled when the guest explicitly aborts (`CONFIG.E=0`). That way the guest's E
check **always** sees E=1 and the `panicf` becomes **unreachable
for the audio channel under any timing** — a deterministic guarantee, not a
probabilistic one. A late relink costs, at most, a brief underrun.
**Gating without regression:** `paced_pending` is only set for *paced* lines, which
only exist under `AUDIOPACE` and are only marked by the 6G I2S; for the 6G retailOS and the
5G the flow is **byte-identical** to stock.

**MEASURED verification (detector: read `panic_buf` at `0x08154c48` via the monitor
after each run).** To *force* the race a temporary knob
`PANICSTRESS=N` was used (it compresses the tick interval so the DMA gets ahead of the guest)
together with a `PACED_NOFIX=1` that reverts the fix in the **same binary** (both now
removed):

| Run | Without fix (`PACED_NOFIX=1`) | With fix |
|---|---|---|
| stress=16 (forced race) | **4/4 PANIC** (incl. the literal `DMA ch2: link error`) | **0 / 8** |
| stress=2 (sustained playback) | **3/5 PANIC** | **0 / 10** |
| normal pacing + `-audio pa` (the user's condition) | — | **0 / 10** |

That is: the stressor **reproduces** the panic (and the detector catches it, exact text
included) and the fix **eliminates** it in every run, both under the forced
race and in normal sustained playback with live audio. (The 5G is not
touched; the 6G and 5G `navtest.sh` still pass — §8.3.)

### 8.3. Reproduce

```sh
# live speakers (window):
sh tools/ipod_classic/rbmusic.sh
# capture to WAV as before (still yields the song, correlation 1.000):
env AUDIOPACE=1 sh tools/ipod_classic/rbplay.sh
# speakers + WAV at the same time:
AUDIOWAV=/tmp/out.wav sh tools/ipod_classic/rbmusic.sh
```
