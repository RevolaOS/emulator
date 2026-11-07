# The retailOS audio DSP: what we know in order to emulate it (CalmADM2E)

Starting document for the future work of **making Apple's retailOS
play sound** in the emulator. It gathers what we already know about the retailOS audio path
and the DSP that decodes, so as not to start from scratch.

Status today: **Rockbox already plays audio** (it decodes on the ARM; see
[16](16-classic6g-rockbox.md)). The **retailOS does not**, because it offloads
decoding to a DSP that is not modeled. This document is about that DSP.

Firmware VA addresses = offset in `osos_decrypted.bin` + `0x07FF51B8`.

---

## 1. The finding: the retailOS decodes on the DSP, not on the ARM

Measured (see [14](14-classic6g-audio.md)): with Rockbox, real PCM comes out through the
I²S; with the retailOS, the PCM captured at the same point is **silence** (RMS 0).
The difference is where the decoding happens:

- **Rockbox** decodes the MP3 on the ARM (`mpa.codec`) and the PCM goes through DRAM.
- **The retailOS** hands it to a **dedicated audio DSP**, the **CalmADM2E**, and
  only moves the result. The ARM never sees the decoded PCM in a form that
  we can capture today.

Evidence of how much the DSP weighs in the retailOS (32-bit literals in the osos):

| Address | What it is | References in the retailOS |
|---|---|---|
| `0x3F000000` | `ADM_BASE` — the DSP's registers in its data area | **245** |
| `0x39000000` | `ADM_SFR_BASE` — the DSP's SFRs in the system | **213** |
| `0x38000000` | SHA-1 accelerator (another block, not the DSP) | 502 |
| `0x3CA00000` | I²S — **already modeled**, it is where the DSP delivers the audio | 2 |

Rockbox **does not touch** `0x39000000`/`0x3F000000` in its audio path (which is why it plays
without a DSP). The retailOS uses them hundreds of times.

---

## 2. What the CalmADM2E is

An **audio decoder module based on CalmRISC**, Samsung's DSP
core. "ADM" = *Audio Decoder Module*. It has its own instruction set
(CalmRISC, 16/32-bit) and runs **proprietary decoding firmware** that the
ARM loads into it. Documented —a huge gift— in Rockbox:
`assets/rockbox/firmware/export/s5l87xx.h:64-137`, section "04. CALMADM2E".

There are **two views** of the same block:
- `ADM_BASE = 0x3F000000` — 16-bit registers, "mapped in the Calm's memory
  data area".
- `ADM_SFR_BASE = 0x39000000` — the same ones, as 32-bit system SFRs.

And its own clock: `DSPCLKMD` at `CLK_BASE+0x38` (`s5l87xx.h:164`).

---

## 3. The register map (from Rockbox `s5l87xx.h`)

**This is the interface that would have to be emulated.** What matters for HLE is that the
DSP is driven through a **mailbox** and some **buffers in DRAM**.

### The mailbox: commands and data host ↔ DSP

| Register | `ADM_BASE+` | `ADM_SFR_BASE+` | What |
|---|---|---|---|
| `CONFIG0/1` | `0x00`/`0x02` | `0x00` | configuration/control |
| `COMMUN` | `0x04` | `0x04` | **communication control** (the "doorbell ring") |
| `DDATA0..7` | `0x06..0x14` | `0x10..0x2C` | **data from host to DSP** (8 words) |
| `UDATA0..7` | `0x16..0x24` | `0x30..0x4C` | **data from DSP to host** (8 words) |

The ARM writes arguments to `DDATAn`, issues a command via `COMMUN`, and reads the
reply in `UDATAn`. That is the command channel — the equivalent of the 5G
VideoCore's mailbox.

### The buffers in DRAM: where the DSP's code and data live

The ARM tells the DSP where its regions are, **all in main DRAM**:

| Register | `ADM_BASE+` | What |
|---|---|---|
| `IBASE_H/L` | `0x26`/`0x28` | start of the DSP's **instruction area** (its firmware) |
| `DBASE_H/L` | `0x2A`/`0x2C` | start of the CalmRISC **data area** |
| `XBASE`,`YBASE` | `0x2E..0x34` | X/Y areas of the MAC (the DSP's multiply-accumulator) |
| `S0BASE_H/L` | `0x36`/`0x38` | start of **sequential buffer 0** |
| `S1BASE_H/L` | `0x3A`/`0x3C` | start of **sequential buffer 1** |

In `ADM_SFR_BASE` the same ones as 32-bit: `ADM_IBASE` `+0x50`, `ADM_DBASE`
`+0x54`, `ADM_XBASE` `+0x58`, `ADM_YBASE` `+0x5C`, `ADM_S0BASE` `+0x60`,
`ADM_S1BASE` `+0x64`.

### The sequential buffers: the key for HLE

`SBFCON`/`SBFSTAT` (`ADM_BASE+0x42`/`0x44`) control two **sequential buffers
in ring mode**, with offset/begin/end registers (`SBL0OFF`, `SBL0BEGIN`,
`SBL0END`, and those of buffer 1, at `+0x46..0x5C`). **INFERRED, very likely:** they are the
**input ring (compressed audio)** and the **output ring (PCM)** — the ARM fills
one with MP3, the DSP decodes and fills the other with PCM, which the DMA then carries
to the I²S. Verifying it is the first RE step.

There is also a cache (`CACHECON`/`CACHESTAT`, `+0x3E`/`+0x40`).

---

## 4. The recommended route: HLE, not emulating the DSP

There is **no** need to execute the CalmRISC or its firmware. What is done is **High-Level
Emulation of the protocol**, just as we did with the **BCM2722 VideoCore on the
iPod Video 5G** (see [09](09-history-5g.md) and `hw/arm/ipod_video/ipod_video_vc.c`):
model *what the block does*, not *how it does it*.

Concrete plan:

1. **Model the `0x39000000`/`0x3F000000` block** as a QEMU device
   (template: `ipod_classic_disp.c` or the 5G's VC). It should accept the writes
   to `IBASE/DBASE/S0BASE/S1BASE`, store those addresses, and answer the mailbox.
2. **Intercept the decode command** in `COMMUN`/`DDATAn`: find out through
   RE which value means "decode this block".
3. **Read the compressed audio** from the input buffer (one of S0/S1, in DRAM,
   at the address the ARM programmed) and **decode it on the host** (libmad or
   ffmpeg inside the model).
4. **Write the PCM** to the output buffer (the other S0/S1) and **signal** (set
   `UDATAn`/`SBFSTAT` and raise the interrupt the ARM expects).
5. From there on the path is already in place: the DMA carries the PCM to the I²S
   (`0x3CA00000`, **already modeled**), the CS42L55 codec is modeled, and WAV
   capture / output to speakers already exist.

Apple's firmware asks "decode this for me" and receives PCM: it doesn't notice that the
CalmRISC didn't run.

---

## 5. What is already modeled downstream of the DSP

All of this **already works** (it was tested with Rockbox) and the DSP only has to
feed it:

- **I²S** at `0x3CA00000` (`ipod_classic_i2s.c`): FIFO, readback, WAV capture.
- **CS42L55 codec** over I²C `0x4A` (`ipod_classic_cs42l55.c`).
- **Audio DMA**: DMAC0, line `IIS0_TX`, paced at the sample rate
  (`hw/dma/pl080.c`, `AUDIOPACE`). `IRQ_DMAC0 = 16`.
- Frequency: 44100/48000 Hz depending on the track (the retailOS sets `I2SCLKDIV`).

So the retailOS-with-audio work is **only the DSP box**: from the
PCM in the output buffer onwards it is already done.

---

## 6. What remains to be found out (RE, first concrete step)

All of this is obtained by **tracing what the retailOS writes** when playing a
song (instrumenting `0x39000000`/`0x3F000000` with a `DSPTRACE`-style trace,
as was done with the compositor):

1. **The DSP boot sequence**: when it loads the firmware to `IBASE`, how
   it starts it (`CONFIG0`?), and where it gets that firmware from (is it in the osos? does it
   decrypt it?). If the DSP firmware has to be loaded and we don't have it, the HLE
   ignores it — that is precisely its advantage.
2. **The mailbox protocol**: which command in `COMMUN` means decode, which
   arguments go in `DDATAn`, what it returns in `UDATAn`.
3. **The S0/S1 buffers**: which is input and which output, their size, the
   exact format of the compressed audio it receives (raw MP3 frames? something pre-parsed
   by the ARM?) and of the PCM it produces (16-bit stereo, interleaved?).
4. **The "block decoded" interrupt**: which one it is and how the ARM waits for it
   (probably to ask for the next block, like Rockbox's double buffer).

The gold source for 2-4: **how the firmware itself does it**. It is also worth
checking whether **freemyipod** or the **giek2000/ipod-classic-firmware-research** research
documents the ADM protocol; and whether the **freemyipod/qemu** model
(branch s5l8702-dev) already touches this block.

---

## 7. References

- `assets/rockbox/firmware/export/s5l87xx.h:64-137` — **the CalmADM2E register
  map**, the primary source for this document.
- [14-classic6g-audio.md](14-classic6g-audio.md) — the measurement showing that the retailOS
  gives silence and Rockbox does not.
- [16-classic6g-rockbox.md](16-classic6g-rockbox.md) — the audio path that
  **does** work (Rockbox on the ARM), downstream of the DSP.
- [09-history-5g.md](09-history-5g.md) and `hw/arm/ipod_video/ipod_video_vc.c` —
  **the precedent**: HLE of the BCM2722 VideoCore by modeling its protocol, not its
  ISA. The pattern to repeat with the DSP.
- [02-s5l8702-hardware.md](02-s5l8702-hardware.md) — the SoC's memory map.
