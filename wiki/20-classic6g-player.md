# How the retailOS player came out (iPod Classic 6G)

Results document. The complete investigation, with the retractions, is in
`19-classic6g-dsp-hle.md` §11.110–§11.119; this is the summary of what was broken and
what fixed it.

Starting point: the player opened, showed half a screen and after 5–18 s went back to
the list on its own. No sound.

```
                BEFORE                             NOW
   +---------------------------+      Now Playing        [>] [bat]
   |  [artwork]                |      +----------+  Billie Jean
   |                           |      | artwork  |  Michael Jackson
   |            (blank)        |      |          |  Greatest Hits
   |                           |      +----------+  1 of 4
   +---------------------------+       0:00 [==============] -4:55
     and after 18 s, back               and playing
        to the list
```

## The four failures, in order of discovery

### 1. The render pump gave up after 8 invocations — `SM1_SLOTS=1`

The audio loop asked `0x080a3c00` for a buffer and gave up because it returned 0 where
`0x22006358` requires 2. The first guard of that function reads an **enable mask** at
`0x2203ff28`, and with `memw=` over an entire run (~180 s, boot included) **no CPU PC
ever writes it**.

It was not a missing bit: it is state that **the SM1 engine publishes** in IRAM1, and
our model was only a register bank that never wrote memory.

What made it implementable without guessing: **the geometry is not a hidden hardware
constant, the engine publishes it and the firmware reads it back** to derive address
and size (`0x080a3c54`..`0x080a3c68`). Any coherent geometry works.

| | without | with |
|---|---|---|
| `rendret` | 8 | **1343** (~20/s, the slot cadence at 44100 Hz) |
| buffers delivered | 0 | 1342 |

Two details cost two iterations: releasing all the slots at once destroys flow control
(the pace must come from the I2S counter), and the frame notification must only be
raised when one has **really** completed, or the pump asks, fails and gives up at 16.

### 2. The copy went the other way — `SM1_DECODE=1`

The "producer" `0x081db4c8` is not a decoder: it is a dispatcher of two fixed buffers
that **always returns success** and delivers zeros. And the pump's copy is

```
22006488: ldrne r0,[sp,#52]   ; destination = the codec buffer
2200648c: ldrne r1,[sp,#28]   ; source      = the IRAM1 slot
22006494: blne 0x22000020     ; slot -> codec buffer
```

that is, the codec buffers are the pump's **output**, not its input: the audio has to
already be in the slot. Filling the codec buffers (the first attempt) was writing to
the far end of the copy, where the pump overwrote it right afterwards.

Decoding MP3 the way an undocumented codec does is not reconstructed bit by bit: it is
**emulated**, and `ipod_classic_hle_audio.c` already does that with verified output.

Checked with `SM1_FEED_OFF=1`, which turns off the host shortcut and leaves the guest's
path as the **only possible route** for the PCM to the speaker:

| PCM fill | result |
|---|---|
| in the codec buffer | 0.18 % nonzero, peak 11444 |
| **in the slot** | **5.7 – 29 % nonzero, peak 32768** |

The real route is: engine → IRAM1 slot → render pump → codec buffer → output driver →
DMA → I2S. **All of that is the retailOS firmware.**

### 3. The compositor had the layers backwards — `DISP_ALPHA=1`

**The Now Playing screen was always drawn. We were covering it.**

With the surfaces dumped while the player was open: layer 0 contains the entire
player —title bar, artist, album, "4 of 4", progress bar— and layer 1 the artwork,
opaque and full screen. The compositor painted back to front starting with layer 0,
so the artwork **erased** the player and left exactly the blank right side that looked
like half a screen.

And the second half of the same error: the model **discarded the alpha channel**. That
layer really does carry one — its transparency byte takes **243 distinct values**.

Compositing front to back with *source-over* explains **both** things: the player ends
up on top of the artwork, and the language menu's sweep —from which the original Z
order was deduced **assuming opaque layers**— still works because the incoming panel
is transparent where the outgoing one should show through. Verified: menus and lists
draw the same as before.

### 4. "Low Battery — Connect to Power" — `PMUCHARGER=adapter`

The device showed that screen and powered off. `ipod_classic_pmu.c` considered it
unresolved ("raising the ADC to full scale does NOT prevent it").

With `PMUTRACE=1` you can see that the ADC **is read correctly** (`0x55 = 0xd3` = the
844 of 1023 that we publish), so the voltage was never the problem:
`MBCS1_HEALTHY = 0x00` describes "on battery, no charger", and the firmware was asking
precisely to be plugged in.

**Careful about which one is reported**, and getting it wrong is measured: with the
**wall adapter** the device enters its full-screen charging screen and stops playing —
which is what a real iPod does, that is, the model got it right and the choice was
bad.

**On battery** the player keeps playing, and the firmware judges the charge from the
battery conversion alone: firmware 2.0.5 converts the battery as ADCC1 `0x1d`
(BATSNS_SUBTR, average of 16) and polls ADCS3/ADCS1. The machine's `battery` property
(start-up value `PMUBATT`, raw 0–1023, default 844) sets that reading. At 0 the
firmware builds `TChargingModeLowPowerCntlr` (`0x0820ca90`), shows "Low Battery —
Connect to Power" (`ChargingMode_Image_Critical`) and powers off. Battery power also
reaches the Now Playing idle clock (`TMediaNowPlayingCntlr_Idle`): play a track and
leave the VM alone for the backlight timeout plus 60 s. The adapter blocks it.

## How to bring it up

```sh
IRAMSPILL=0x10000 SHA1_ZERO=1 SM1_IRQ=1 \
SM1_SLOTS=1 SM1_NSLOTS=2 SM1_DECODE=1 SM1_FEED_OFF=1 \
DISP_ALPHA=1 PMUCHARGER=adapter \
HLE_AUDIO_DATAPATH=1 HLE_AUDIO_SUSTAIN=1 HLE_AUDIO_DISK=<image> \
qemu-system-arm -M iPod-Classic ... -display sdl
```

`SM1_NSLOTS=2` is not cosmetic: with the default 7 slots, 28 KiB are written starting
at `0x22038100` and that **tramples someone else's memory**, freezing the interface
halfway in while the audio keeps playing. Publishing the state is harmless; writing
28 KiB is not.

Keys: `q`/`e` wheel · `Enter` center · `w` menu · `a`/`d` track · `s` play-pause.
The firmware discards ~5 of every 7 center presses, so you have to keep at it -- a
single lost run reads as "the player does not open" and has already cost several
passes.

### Sound on the host

The model's I2S only knows how to write WAV; it has no audio output of its own. To
hear it live the file is routed to `ffplay`, and **`SDL_AUDIODRIVER=pulse` is
needed**: with the default driver ffplay **hangs without printing anything**
(measured: 20 s timeout on a 3 s file). That, and not a lack of audio, is what made it
look as if the emulator produced no sound.

## What still does not work

- **The elapsed time stays at 0:00.** Making the SM1 position register follow the I2S
  (as `sm1_stream_pos` already does) does **not** move it, so that counter is kept by
  the player's internal bookkeeping, not by the register.
- **Pause and track change.** Pause cuts the audio cleanly, but does not resume; when
  changing songs a brief burst plays and then goes silent. The PCM cursor of
  `sm1_fill_slot` is a static that is never reset, so on a track change it reads from
  the wrong place.
- **Everything is behind an environment variable.** The default build stays byte for
  byte as it was, because each of these changes has measured trade-offs and none is
  settled enough to impose it.
