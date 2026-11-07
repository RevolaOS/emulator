# Emulating clickwheel iPods in QEMU

Reverse-engineering and development log of the QEMU-iOS port. Working
branch: `ipod_classic_6g`, starting from `ipod_touch_2g` (QEMU 8.2.0).

It started out targeting the **iPod classic 6G** (S5L8702) and ran into the
fact that its firmware is encrypted with a key that lives in the silicon. When
a physical **iPod Video 5G** turned up, whose firmware is in the clear, the
goal of booting real software moved there. On August 9, 2026 the Classic's
blocker fell: there was no need to extract the key, but rather to have **the
device itself decrypt** — the physical iPod running emCORE hands out the
`osos` in the clear over USB. And on August 10 the Classic went from booting
to **being seen** and from being seen to **being navigated**.

**Today both machines boot Apple's original firmware and both can be
navigated.**

| Machine | SoC | Status |
|---|---|---|
| `iPod-Video` | PP5022C | **Boots both firmwares of the real device**: Rockbox up to the menu, navigable; and Apple's retailOS drawing its interface, with navigable menus and submenus |
| `iPod-Classic` | S5L8702 | **Boots Apple's retailOS and can be navigated**: RTXC kernel, ATA with DMA, `rsrc` volume mounted, typefaces, the language selection screen — 75 463 of 76 800 pixels — which the wheel moves, and from it **the main menu in the chosen language, with the right panel following the highlight, Enter going into Music and MENU returning to the same frame**. Missing: the guest drops one SELECT out of every three or four, with no cause found |

For a one-page introduction, start with the
[root README](../README.md). To build it, run it and test it:
[05-build-notes.md](05-build-notes.md).

## Index

### How everything was figured out — the two highest-yield documents

| Document | Contents |
|---|---|
| [12-history-6g.md](12-history-6g.md) | **Technical chronology of the iPod classic 6G**: from a firmware that was an unreadable encrypted blob to a navigable language screen. Every obstacle with its symptom, its cause and **how it was found**; the catalog of retracted hypotheses; and the chapter on **the three times a discarded lead turned out to be the right one** — what they have in common and what should have been done differently. |
| [13-classic6g-music.md](13-classic6g-music.md) | **Putting music on the emulated 6G**: why copying the MP3 is not enough, how the `iTunesDB` is built with `libgpod`, and where the model and serial number come from (from the NOR's `SysCfg`, with the tags reversed). With the risk point stated plainly: the FireWire identifier is **made up**, and if the firmware validates the hash against its own, the list will come out empty. |
| [14-classic6g-audio.md](14-classic6g-audio.md) | **The audio path (getting it to play)**: the I²S block `0x3CA00000` modeled (readback + FIFO + capture to WAV), the CS42L55 codec over I²C `0x4A`, and DMA pacing at the sample rate (milestone 2, demonstrated). With the two walls measured: **the retailOS decodes on the CalmADM2E DSP** (not on the ARM), so the captured PCM is silence; and **the buttons do not register in the song list**, which is what prevents reaching Now Playing. The path to real audio is Rockbox. |
| [15-classic6g-walls.md](15-classic6g-walls.md) | **The Classic 6G's walls, and how each one fell**: a synthesis of the whole port — each obstacle (encrypted firmware, black screen, the compositor, the fonts, the wheel, the FAT mount, the `iTunesDB` signature) with its symptom, its cause and **how** it was unblocked. Includes the repeated pattern of "the discarded lead that was the clue" and the walls still standing. |
| [17-classic6g-dsp-retailos.md](17-classic6g-dsp-retailos.md) | **The retailOS audio DSP (CalmADM2E), to emulate it later**: why the retailOS does not play sound (it decodes on a DSP, not on the ARM), the **CalmADM2E register map** (`DDATA`/`UDATA`/`COMMUN` mailbox + sequential buffers in DRAM, from Rockbox's `s5l87xx.h`), the recommended **HLE** route with the 5G's VideoCore precedent, and the concrete RE steps still missing. Starting point for "getting the retailOS to play sound". **Its premise is corrected by [19](19-classic6g-dsp-hle.md).** |
| [19-classic6g-dsp-hle.md](19-classic6g-dsp-hle.md) | **HLE specification of the retailOS audio, and inventory of unknowns**: a synthesis of two RE investigations (disassembly + trace) that **correct by measurement** the premise of [17](17-classic6g-dsp-retailos.md). VERIFIED: the retailOS **does not touch** `0x39000000`/`0x3F000000` in any reachable state (the "reference count" was the float `0.5f` and data); the HW driven during playback is an **image scaler** (`0x391/2/3`, the artwork), not audio; the real decode is **MeCCA/AudioCodecs in software**. The candidate register map, the 4 protocol questions answered by measurement (with what remained UNKNOWN), and a **decision-tree HLE plan** whose first cheap node decides where the model goes before writing a line. |
| [16-classic6g-rockbox.md](16-classic6g-rockbox.md) | **Booting Rockbox to its main menu**: the direct `-kernel rockbox.bin` route, the two hangs of booting without a bootloader (SP=0 in `memory_init`, stopped TimerE that hangs `udelay`) reproduced as a hand-off in `mc->reset`, and the pixel path via **16-bit DMA to the LCD** that the model (built for the retailOS) rejected. It ends at the navigable cabbiev2 menu, with the retailOS's `navtest.sh` intact. It is the first step toward **real audio** (Rockbox decodes the MP3 on the ARM). |
| [09-history-5g.md](09-history-5g.md) | The same for the iPod Video 5G: every obstacle, its symptom and its cause, **including the hypotheses that turned out to be false** and why they held up. With the catalog of retracted errors, the debugging techniques ordered by how much they yielded, and the open questions. |

### The iPod classic 6G (S5L8702)

| Document | Contents |
|---|---|
| [10-classic6g-emcore.md](10-classic6g-emcore.md) | **The real device over USB**: the emCORE r859 protocol (and why the official clients do not work), **the decryption of the `osos` with the SoC's own GID key** — the foundational blocker, solved —, the dump of the NOR with its `SCfg` block, the proof that **the bootrom is locked** and cannot be dumped this way, and the address that hangs the monitor. |
| [11-classic6g-retailos.md](11-classic6g-retailos.md) | **The retailOS running**, 17 sections: the image's two link regions, the unnamed block at `0x3C100000`, the panel and the ATA bridge, the no-disk crash cycle and its cause, **the compositor at `0x38900000` and where the pixels come from** (§13), **why the boot with a disk was stalled: DMAC0, not the ATA** (§14), **the click wheel and the screen that can be moved** (§17), and three independent audits. |
| [01-firmware-ipsw.md](01-firmware-ipsw.md) | Format of the `.ipsw`, the `]ih[` firmware partition and the `8702` image header. Includes the correction that cost days: **`rsrc` is not empty, it is a FAT16 with the typefaces** the interface needs. |
| [02-s5l8702-hardware.md](02-s5l8702-hardware.md) | **S5L8702 reference**: memory map, peripherals, IRQ numbers and board components, **with what is modeled in this tree and what is not**. Corrected in `e17924eab`: the first version let in bases that only exist on S5L8700/8701. |
| [08-s5l8702-external-sources.md](08-s5l8702-external-sources.md) | Corrections to the memory map, bootrom, boot chain, click wheel, and the **QEMU model of the S5L8702 that already exists** in `freemyipod/qemu`. |
| [03-reuse-qemu-ios.md](03-reuse-qemu-ios.md) | **Planning document, historical**: which models from the `ipod_touch_2g` branch were meant to be reused. It carries a banner with what was actually reused. |

### The iPod Video 5G (PP502x)

| Document | Contents |
|---|---|
| [06-ipod-video-5g.md](06-ipod-video-5g.md) | **The real device**: identification (and the source conflict over which chip it carries), disk layout, its **unencrypted** firmware, the dual boot, and how to dump the flash ROM from Rockbox itself. |
| [07-pp502x-hardware.md](07-pp502x-hardware.md) | **PP5020/PP5022 reference**: memory map with what is modeled and what is not, the bit 26 and bit 31 aliases, IRQs, the registers that have behavior, the IDE with its DMA, the I²C and the PCF50605, the two DMA blocks, the Broadcom BCM2722 and the VideoCore host interface, the click wheel and the boot model. It ends with **the model's assumptions that are not data**. |

### Cross-cutting

| Document | Contents |
|---|---|
| [04-roadmap.md](04-roadmap.md) | Status by milestone and the course of each blocker that had to be removed. The longest document and the closest to a diary. |
| [05-build-notes.md](05-build-notes.md) | How to build (WSL), **how to run it and test it**, click wheel keys, the machine properties that do not show up in `-M help`, three environment traps (CRLF, git symlinks, `--disable-werror`) and how to find out where the firmware has stopped. |
| [18-environment-tools.md](18-environment-tools.md) | **What to install and download to reproduce the whole project**: the complete list grouped by what it is for — WSL2+WSLg and usbipd (Windows), the QEMU dependencies, the ARM toolchain and `gdb-multiarch` (RE), `dosfstools`/`mtools`/`libgpod` (disks and music), `ffmpeg`/`numpy` (verifying the audio), `pyusb`/`libusb` (the device via emCORE), and the downloaded repos (Rockbox, freemyipod, giek). With the `apt install` of everything together. |

## Sources

- **Rockbox** — complete, working port to both devices, with open source
  register-level drivers. It is the primary source for the memory map.
  - For the PP502x: `firmware/target/arm/ipod/`, `firmware/export/pp5020.h`,
    `bootloader/ipod.c`.
  - For the S5L8702: `firmware/export/s5l87xx.h` (2052 lines, ~1400 defines),
    `firmware/target/arm/s5l8702/`, `bootloader/ipod-s5l87xx.c`.
  - Local copy in `assets/ref/` and `assets/rockbox/`.
  - **Method warning**: what a Rockbox driver *names but does not use* is
    exactly where the model has an invisible hole. See `HI_IRQ` in
    [09-history-5g.md](09-history-5g.md).
- **iPodLinux** — `ipodloader2/ipodhw.c`, where the SysInfo block that the
  boot ROM leaves at the top of IRAM comes from.
- **ipodpatcher** — the dual boot model and the `getmodel()` table.
- **freemyipod.org** — reverse-engineering wiki for non-iOS iPods,
  [Classic_6G](https://freemyipod.org/wiki/Classic_6G); **emCORE**, which is
  what makes the Classic's firmware decryptable; and `freemyipod/qemu` branch
  `s5l8702-dev`, a QEMU model of the same SoC.
- **devos50/qemu-ios** — the base emulator, branches `ipod_touch_1g` (S5L8900)
  and `ipod_touch_2g` (S5L8720).
- **Martijn de Vos's blogs** — [Touch 1G](https://devos50.github.io/blog/2022/ipod-touch-qemu/),
  [Touch 2G](https://devos50.github.io/blog/2023/ipod-touch-2g-qemu/).

## Where everything is

```
hw/arm/ipod_video/              PP502x models — 8 files
include/hw/arm/ipod_video/      their headers — 7 files
tools/ipod_video/               5G tools

hw/arm/ipod_classic/            S5L8702 models — 10 files
include/hw/arm/ipod_classic/    their headers — 10 files
tools/ipod_classic/             Classic tools

wiki/                           this documentation
assets/                         GITIGNORED — firmware, extracted images,
                                screenshots, Rockbox reference sources
```

`assets/` is outside git on purpose: it is material that is not ours to
redistribute. Without your own dump of an iPod there is nothing to boot. It
contains, among other things:

| Path | What it is |
|---|---|
| `assets/classic6g/osos_decrypted.bin` | **the Classic 6G's retailOS in the clear**, decrypted by the device itself. It is what `-M iPod-Classic` boots |
| `assets/classic6g/bootflash_nor_1MB.bin` | dump of the Classic's SPI NOR, with its `SCfg` block (where `HwVr` = 19 comes from) |
| `assets/classic6g/aupd_decrypted.bin`, `rsrc_fat16.img` | the decrypted updater and the resource volume |
| `assets/iPod_24.1.1.2.ipsw`, `assets/extracted/`, `assets/images/` | the Classic 6G firmware, unpacked and extracted |
| `assets/ipod-video-fw.bin` | dump of the 5G's firmware partition, **with the MBR in front** (82 252 800 B) |
| `assets/ipod-video-fwpart.bin` | the same **without** the 63 header sectors — it is the one `fwtool.py` wants |
| `assets/ipod-video-disk.img` | disk image built with `mkdisk.sh` (MBR + firmware + FAT32) |
| `assets/images-5g/` | `osos_full.bin`, `apple_retailos.bin`, `rockbox_bootloader.bin`, `rockbox_app.bin`, `vmcs.bin` |
| `assets/apple/`, `assets/nav-baseline/`, `assets/nav*/` | reference screenshots of both devices |
| `assets/ref/`, `assets/rockbox/` | Rockbox sources for S5L8702 and ipod6g |

## Our own tools

### `tools/ipod_classic/` — the Classic

| Tool | What it does |
|---|---|
| `emcore_s5l8702.py` | Speaks the emCORE r859 protocol with the physical iPod over USB: `info`, `hexdump`, `dump`, `norread` and **`decrypt`**, which is the one that gets the `osos` out in the clear with the SoC's GID key |
| `fwtool.py` | Parses and extracts the images of a `]ih[` container. **Also used for the 5G** |
| `mkdisk6g4k.py` | **Builds the bootable disk** from the `.ipsw`'s `Firmware-XX` + an empty FAT32, with the MBR written **in 4096-byte units**, which is the device block size of an iPod Classic. `mkdisk6g.py` is the 512-byte-sector variant, from the layout investigation |
| `mkfwpart6g.py` | Repackages a firmware partition for another block size |
| `navtest.sh` | **The navigation test**: boots, waits, checks that the screen is still, presses, captures and **decides**. Fails if the noise floor stops being 0 |
| `audit_run.sh`, `audit_peek.sh` | Boot, wait, capture and summarize stderr; the second reads physical memory through the monitor |
| `gdbpoke.py`, `gdbrun.sh`, `gdbtrace.py` | GDB remote protocol client: breakpoints, registers, memory and `--skip` to skip a call **without touching the guest image**. `gdbrun.sh` starts QEMU with `-S -gdb` and drives it |
| `pixcount.py`, `ppm2png.py` | Count non-black pixels of a `screendump` and convert it to PNG **to look at it**. `ppm2png.py --raw 320x240:argb8888` also reads raw framebuffers taken with `pmemsave` |
| `findlit.py` | Where a 32-bit literal is in the image, as a VA, and which `ldr rX,[pc,#imm]` load it. No false positives |
| `lcdtest.S` | **Panel control case**: draws a 320×240 ramp that gives exactly 76 800 pixels and 1920 colors |
| `osana.py` | Analysis helpers for `osos_decrypted.bin`, with the scatter-load base `0x07FF51B8` already applied |
| `norboot_im3.py` | Identifies and decrypts the IM3 image at offset `0x8000` of the NOR |
| `mktestbin.py` | Generates test ARM binaries |
| `ranaddr.sh` | "did this function run?" — `-d in_asm` + grep, **only valid for block entries**, and the script itself warns about it |
| `atatrace.sh` | Boots with a disk and captures every ATA register access plus whatever the firmware says via semihosting |
| `mk6g_*.sh` | Throwaway tools from the disk layout investigation |
| `tests/` | Bare-metal tests (timers, interrupt path) |

### `tools/ipod_video/` — the 5G

| Tool | What it does |
|---|---|
| `dump-firmware.ps1` | Dumps the firmware partition of a connected iPod. **Windows, as administrator.** Opens the disk read-only and refuses if there is more than one candidate or if it is the system disk |
| `mkdisk.sh` | Builds a bootable disk image: MBR + firmware + a FAT32 with `.rockbox`. Needs `dosfstools` and `mtools` |
| `navtest.sh` | **Rockbox** navigation test (regression against `assets/nav-baseline`) |
| `appletest.sh` | Navigation test of **Apple's retailOS** |
| `run.sh` | Launcher (`run.sh rockbox\|apple`). Meant for tracing registers: **does not attach a disk** |
| `extract-vc.py` | Extracts `vmcs.bin` and the rest of the FAT16 embedded in the firmware partition |
| `findlit.py` | Same as the Classic's. It was born from a method error: searching raw bytes gave 139 false positives where there was 1 true one |

> `tools/ipod_video/navtest.sh` and `appletest.sh` **are not assertions**: they
> exit with code 0 even if QEMU dies at boot. You have to look at the figures.
> `tools/ipod_classic/navtest.sh` **does** decide, and fails.
