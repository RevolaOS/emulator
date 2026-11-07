# iPod classic 6G firmware format

Material analyzed: `iPod_24.1.1.2.ipsw` (60 MB), provided by the user.
Tool: `tools/ipod_classic/fwtool.py`.

## Level 1 — the `.ipsw`

It is a ZIP with two entries:

```
Firmware-24.9.1.2    92.930.048 bytes   <- the firmware partition
manifest.plist              672 bytes
```

The `manifest.plist` declares:

| Key | Value |
|---|---|
| `FirmwareName` | `Firmware-24.9.1.2` |
| `BuildID` | 152207360 (`0x0912_0000`) |
| `VisibleBuildID` | 17989632 (`0x0112_0000`) |
| `FamilyID` | 11 |
| `UpdaterFamilyID` | 24 |

## Level 2 — the firmware partition (`]ih[`)

This is the classic iPod format, the same one Rockbox's `ipodpatcher` manipulates.

```
0x0000  256 bytes of ASCII art "STOP / Copyright(C) 2001 Apple Computer, Inc."
0x0100  magic "]ih["
0x0104  uint32  image directory offset  = 0x00004000
0x0108  uint32  0x0003010c  (unidentified)
...
0x5000  image directory
```

**Watch out for the directory offset:** the field says `0x4000` but the actual
directory is at `0x5000`. On the S5L870x the firmware area starts `0x1000`
into the payload, so `dir_real = 0x1000 + dir_offset`. `fwtool.py` tries
`0x1000` and then `0x0`.

### Directory entries (40 bytes each, little-endian)

```
0x00  char[4]  dev          (stored reversed: 'ATA!')
0x04  char[4]  type         (stored reversed: 'rsrc', 'osos', 'aupd', 'hash')
0x08  uint32   id
0x0c  uint32   devOffset    offset within the file
0x10  uint32   len
0x14  uint32   addr         load address
0x18  uint32   entryOffset
0x1c  uint32   chksum
0x20  uint32   vers
0x24  uint32   loadAddr
```

### Actual contents of this firmware

| # | type | devOffset | len | addr | vers |
|---|---|---|---|---|---|
| 0 | `rsrc` | `0x00006000` | `0x04e00000` (78 MB) | `0x00000000` | 0 |
| 1 | `osos` | `0x04e07000` | `0x009773e3` (9.5 MB) | **`0x08000000`** | `0x00013000` |
| 2 | `aupd` | `0x05780000` | `0x0011c8b3` (1.1 MB) | `0x08000000` | `0x00013000` |
| 3 | `hash` | `0x0589e000` | `0x00001000` (4 KB) | `0x00000000` | 0 |

- `osos` is Apple's operating system (Pixo/RetailOS).
- `aupd` is the updater.
- `hash` is filled with `0xFF` — it is generated on the device, it does not come here.
- **`rsrc` is NOT empty.** For days this line said that `rsrc` and `hash`
  were both filled with `0xFF`; that was false, and it cost dearly. See below.

### CORRECTED (2026-08-10) — `rsrc` is a FAT16 volume with the fonts

A look at `dev_offset` without the `0x1000` bias lands in the padding and reads
as an empty image. Applying it, `rsrc` starts at `0x7000`, and the FAT boot
sector is `0xe00` further on:

```
0x00007e00  FAT16 boot sector
            OEM "MTOOL399", label "IPODRESOURC", 159 744 sectors (78 MB)
0x0002f000  root directory  ->  RESOUR~1/  ("Resources")
```

And inside, exactly what the retailOS looks for in `"Resources/Fonts"`:

| File | Bytes | |
|---|---|---|
| `HELVET~1.TTF` | 457 656 | Helvetica, the interface typeface |
| `HELVET~2.TTF` | 453 452 | |
| `CJK16.TTF` | 2 919 256 | Chinese, Japanese and Korean |
| `CJK11.TTF` | 1 920 848 | |
| `MONOHO~1..4.SBI` | 22–40 KB | bitmaps |

Plus `Games/`, `Trainers/` (the Nike+ workouts in XML) and `Voices/` with the
voice recordings of Lance Armstrong and Paula Radcliffe.

Without this the screen stays black: the font loader iterates a real
directory, finds nothing, returns NULL, and the firmware calls through that
NULL's vtable and dies. **This is where the characters on the language screen
come from** — the kanji and the Greek are these two TTFs. See
[11-classic6g-retailos.md](11-classic6g-retailos.md).

To see it:

```bash
python tools/ipod_classic/fwtool.py list assets/extracted/Firmware-24.9.1.2
#  [0] rsrc off=0x00007000 len=0x04e00000
```

> **Key fact:** `addr = 0x08000000` confirms the S5L8702's SDRAM base, and
> matches Rockbox's `DRAM_ORIG`. See [02-s5l8702-hardware.md](02-s5l8702-hardware.md).

## Level 3 — the `8702` image header

Both `osos` and `aupd` start with a SoC-specific header. The following layout
is **verified to the byte** against both images (the fields match exactly the
actual size of each blob in the file):

```
0x00  char[4]  "8702"          magic (NOT reversed)
0x04  char[3]  "1.0"           version
0x07  uint8    0x03            type / encryption
0x08  uint32   0               unidentified
0x0c  uint32   payload size, without padding
0x10  uint32   footer_offset + footer_size
0x14  uint32   footer offset, relative to the start of the payload
0x18  uint32   footer size
0x40  byte[20] SHA-1
0x54..0x7ff    zeros (padding)
0x800          payload starts
```

All offsets/sizes inside the header are **relative to the start of the
payload (0x800)**, not to the start of the file. Total size on disk = `0x800 + field@0x10`.

### Measured values

| Image | payload (`+0x0c`) | footer (`+0x14`, `+0x18`) | computed total | last non-zero byte |
|---|---|---|---|---|
| `osos` | `0x976f7c` | `0x977000` + `0xbe3` | `0x9783e3` | `0x9783e3` ✔ |
| `aupd` | `0x11c444` | `0x11c4d0` + `0xbe3` | `0x11d8b3` | `0x11d8b3` ✔ |

The directory `len` is always `total - 0x1000`. The footer is `0xbe3` bytes
in both — it is a fixed-size signature/certificate blob.

SHA-1 at `+0x40`: `osos` = `8eea8def4f63b9be9e51a8a2d2e4ca1746e6c7f9`,
`aupd` = `063fbf7802b9de9c1b1e163db6af0189426bbd1e`.

## Encryption status

Payload entropy measured over 64 KB blocks:

| Image | first 64K | middle | end |
|---|---|---|---|
| `osos` | 7.937 | 7.997 | 7.998 bits/byte |

Practically 8 bits/byte ⇒ **the payload is encrypted**. There are no ASCII
strings or ARM code patterns. The encryption uses the GID key that lives inside
the SoC's AES engine.

### Consequence for the project

Booting Apple's `osos` in the emulator requires the AES engine model to be able
to decrypt with the S5L8702 key. That is why **the first target is not Apple's
OS but Rockbox**, which is open source, builds unsigned and loads from
`0x08000000` just like the `osos`. See [04-roadmap.md](04-roadmap.md).

### RESOLVED (2026-08-09) — the key is no longer needed

The reasoning above assumed that you had to *have* the GID key. You don't:
it is enough for **the device itself to decrypt**. With the physical iPod
classic 6G running emCORE, its AES engine decrypts the payload over USB in 1.2 s.

`osos_decrypted.bin` is in `assets/classic6g/` (gitignored) and is
unmistakably Apple's retailOS: ARM vectors, `Copyright 2001-2008 Apple
Inc.`, the interface (`Cover Flow`, `Now Playing`), `RTXC v3.2b`.

The full procedure, the protocol and the tool:
[10-classic6g-emcore.md](10-classic6g-emcore.md).

## Using the tool

```bash
python tools/ipod_classic/fwtool.py list    "<path>/Firmware-24.9.1.2"
python tools/ipod_classic/fwtool.py extract "<path>/Firmware-24.9.1.2" <outdir>
```

`extract` produces, for each image: `.img` (full blob), `.payload` (body without
header) and `.footer`.
