# iPod Video (5G/5.5G) — the real device

Hardware physically available, dumped on 2026-08-06. Unlike the Classic 6G,
**its firmware is not encrypted**, which made it the realistic path toward
booting Apple's original operating system.

> **It is no longer the only one** (August 10, 2026). The Classic 6G firmware
> is also encrypted and boots anyway: there was no need to decrypt it here;
> instead it was enough to **ask the device itself**, which has the GID key in
> silicon and its AES engine reachable over USB under emCORE. See
> [10-classic6g-emcore.md](10-classic6g-emcore.md) and
> [12-history-6g.md](12-history-6g.md). The entropy comparison further below is
> still correct and is still the five-second test to run before planning
> anything — it's just that the answer "this is encrypted" no longer closes the
> door.

## Identification

Contradictory sources, resolved in order of reliability:

| Item | Value | Source |
|---|---|---|
| Model number | `MA146E` | Apple's *Settings → About* menu |
| **Chip** | **PP5022C** | Rockbox, read from the SoC's own version register |
| Hardware revision | `0x000b0005` | Rockbox HW info |
| Clock | 81805 (~81.8 MHz) | Rockbox HW info |
| RAM | 64 MB | `rockbox-info.txt` and HW info |
| `vers` of the `osos` | `0x0000b012` → `>>8` = **`0xb0`** | the firmware itself |
| USB | `VID_05AC` / `PID_1209` | Windows |

`0xb0` is exactly what the `getmodel()` table in `ipodpatcher` labels as
*"Video (aka 5th Generation)"*, target `ipodvideo`. That value comes from the
firmware, so it is the one that counts.

The model number `MA146` appears in databases as a 30 GB 5G (which would have
32 MB of RAM and a PP5021C), which **does not match** the 64 MB and the PP5022C
that the silicon reports. The device is modified: it has an **iFlash** adapter
with an 8 GB card in place of the original hard drive (the disk model is
reported as `iFlash-Platform iPod Ada`). Most likely parts have been mixed.
**For the emulator, the silicon wins: PP5022C with 64 MB.**

## Disk layout

```
Sector 0        header that looks like a FAT VBR (EB 58 90 "MSDOS5.0")
                + valid MBR partition table at 0x1BE, signature 55AA

Partition #0    type 0x00   LBA 63       160587 sectors    <- FIRMWARE
Partition #1    type 0x0B   LBA 160650   15117429 sectors  <- FAT32 "IPOD" (D:)
```

Windows **does not show the firmware partition** because its type is `0x00`.
It has to be read raw: it starts at `63 × 512 = 0x7E00` and spans `160587 × 512
= 82,220,544` bytes.

`tools/ipod_video/dump-firmware.ps1` automates the dump (requires
administrator; it opens the disk read-only, finds the disk by model, and refuses
if there is more than one candidate or if it is the system disk).

> **A trap in the script itself**, verified on 2026-08-09: the final check that
> looks for the `]ih[` magic at offset `0x100` **always fails on its own
> output**, and it's not that the dump is bad. The dump starts at sector 0,
> which is the MBR, so the `]ih[` is at `0x7e00 + 0x100 = 0x7f00`; at `0x100`
> there are MBR bytes. That's why the script also produces
> `ipod-video-fwpart.bin`, which is the dump **without** the 63 header sectors
> — and it is that one, not the full one, that has to be given to `fwtool.py`.

## Contents of the firmware partition

Same `]ih[` container as the Classic 6G. Directory at `0x4200`:

| # | type | offset | size | addr | entry | vers |
|---|---|---|---|---|---|---|
| 0 | `osos` | `0x004400` | `0x742514` (7.6 MB) | `0x10000000` | `0x735a00` | `0x0000b012` |
| 1 | `rsrc` | `0x746c00` | `0x500000` (5 MB) | `0x10000000` | — | `0x0000b012` |
| 2 | `aupd` | `0xc46e00` | `0x106400` (1 MB) | `0x10000000` | — | `0x0000b012` |
| 3 | `hibe` | `0xd4d600` | `0x2000800` (33 MB) | `0x10000000` | — | 0 |

> **`addr = 0x10000000`** is the load address on the PP502x — the anchor of the
> memory map, just as `0x08000000` was for the S5L8702.

### Trap: the directory offset lies

The header says `dir_offset = 0x4000` at `+0x104`, but the real directory is at
`0x4200`. And in the `Firmware-XX` that comes out of a Classic 6G `.ipsw`, the
same `0x4000` field points to `0x5000`. The displacement changes depending on
where the container comes from.

`fwtool.py` no longer trusts the field: it tries the known displacements
(`+0x1000`, `+0x200`, `+0`) and, if none of them works, scans for the first
thing that parses as a valid directory entry. It validates that both
four-character codes are printable and that the offset falls inside the file,
which is what prevents reading 81 garbage entries.

## Unencrypted — verified

| Measurement | iPod Video | Classic 6G |
|---|---|---|
| Entropy, first 64K | 6.348 bits/byte | 7.937 |
| Entropy, middle | 5.228 | 7.997 |
| Entropy, end | 5.650 | 7.998 |
| ASCII strings ≥6 | 21,016 | ~0 (noise) |

And the strings are unambiguous:

```
portalplayer
PP5020AF-05.00-PP07-05.00-PP07-05.00-DT
2003.10.30  (Build 04)
Digital Media Platform
Copyright(c) 1999 - 2003 PortalPlayer, Inc.  All rights reserved.
booting!  hotstuff  battislo  diskmode  diskscan  softupdt  retailOS
```

`retailOS`, `diskmode`, `diskscan`, `softupdt` are the image names of the
PortalPlayer loader. **Apple's operating system is right there, readable.**

## Rockbox already installed

The iPod dual-boots. `ipodpatcher` puts the Rockbox bootloader inside the
`osos` image and keeps Apple's firmware behind it; at boot one or the other is
chosen.

`D:\.rockbox\rockbox-info.txt`:

```
Target: ipodvideo
Target id: 15
Target define: -DIPOD_VIDEO
Memory: 64
Binary: rockbox.ipod   (765676 bytes)
gcc: arm-elf-eabi-gcc (GCC) 4.9.4
```

That gives us three known, working binaries to test the emulator against: the
Rockbox bootloader, `rockbox.ipod`, and Apple's `retailOS`.

## Status

This device is the one that boots. With its own `osos` dump and a disk image
built with `tools/ipod_video/mkdisk.sh`, the emulator boots **both** firmwares:

- **Rockbox**, up to the menu, navigable with the wheel.
- **Apple's retailOS**, which draws its interface and can be navigated:
  language screen → main menu → *Videos* submenu → back.

> This section used to say only *"gets as far as the Rockbox menu"*. Corrected:
> the retailOS also boots since commits `5b5af8966` and `662d0fb83`.

See [04-roadmap.md](04-roadmap.md) for the journey,
[07-pp502x-hardware.md](07-pp502x-hardware.md) for the registers, and
[09-history-5g.md](09-history-5g.md) for how each thing was figured out.

Resolved since this page was written:

- **Where the Rockbox bootloader ends and Apple's firmware begins inside the
  `osos`**: it's in the table in 07, byte by byte. The retailOS occupies
  `0x000000`-`0x735a00` and the bootloader comes after it, at `entryOffset`.
- **The PP5022C register map**: in 07, with what is modeled and what isn't.

## Dumping the device's flash ROM

No need to compile anything: **Rockbox already includes it**. The entry has
existed since before v3.0, so any `ipodvideo` build will do.

`apps/debug_menu.c`:

```c
#elif defined(CPU_PP) && !(CONFIG_STORAGE & STORAGE_SD)
static bool dbg_save_roms(void)
{
    int fd = creat("/internal_rom_000000-0FFFFF.bin", 0666);
    if(fd >= 0) {
        write(fd, (void *)0x20000000, FLASH_SIZE);
        close(fd);
    }
    return false;
}
```

It applies to this device: `ipodvideo.h` defines `CONFIG_STORAGE STORAGE_ATA`
(not SD), `CONFIG_CPU PP5022` (i.e. `CPU_PP`) and `FLASH_SIZE 0x100000`. The
iFlash mod doesn't break it, because `CONFIG_STORAGE` is a compile-time
constant; it doesn't depend on the physical medium.

### Procedure

1. Eject the iPod from the PC and boot **Rockbox** (not Apple's firmware).
2. MENU → **System** → **Debug (Keep Out!)**.
3. The **first** item in the list is **`Dump ROM contents`**. Select it.
4. **It gives no feedback at all**: no confirmation, no bar, no splash. Wait a
   couple of seconds and exit with LEFT.
5. Connect USB so it mounts the FAT32.
6. In the **root**, `internal_rom_000000-0FFFFF.bin` appears, exactly
   **1,048,576 bytes**.

Caveats, all verified in the code: it doesn't check the return value of
`write()` or whether there is space left, and if `creat()` fails it stays
silent and nothing happens. If the file doesn't appear, that's why. It is
always written with the same name in `/`, so re-running it overwrites.

**Check that the dump is good**: at offset `0xFFE00` you should see the ASCII
bytes `hslf` (which is `flsh` in little-endian). If it's all zeros, it failed.

### ROM format

Directory at `0xFFE00` (the last 512 bytes of the MB), entries of `0x28` bytes,
terminated when the magic stops being `flsh`:

| Offset | Field |
|---|---|
| `+0x00` | magic `flsh` |
| `+0x04` | image id: `disk`, `diag`, `logo`, `scan`, **`vmcs`** |
| `+0x0C` | offset within the flash |
| `+0x10` | length |
| `+0x14` | load address |
| `+0x1C` | checksum (simple byte sum) |
| `+0x20` | version; high byte `0xb0` = iPod Video |

Two independent sources confirm it and agree: `flash_get_section()` in
`firmware/target/arm/ipod/video/lcd-video.c` (which reads the ROM live from
`0x20000000`) and `utils/ipod/flashsplit/flashsplit.c` (which splits a dump).
`flashsplit` produces `diskmode`, `diagmode`, `logo`, `diskscan` and `vmcs`, and
should print `ipod model: ipvd`.

### What we want it for

**It isn't needed to boot either of the two firmwares**: both boot today
without a ROM dump.

> This used to say *"the obstacle is the second core"*. **That hypothesis was
> withdrawn** in commit `9d823ec76`, which modeled the COP and fixed nothing.
> The full chain is in [09-history-5g.md](09-history-5g.md).

It is still interesting for two reasons. The **`vmcs`** section is the Broadcom
firmware, which is what `lcd-video.c` looks for when the BCM isn't already
powered on — today we sidestep it by setting `GPO32_VAL` bit 14 by hand; with
the real dump we could model that path instead of short-circuiting it. (The
`vmcs.bin` itself **we already have** without dumping the NOR: `extract-vc.py`
pulls it out of the FAT16 embedded in the disk's firmware partition.) And we
would have `diskmode` and `diagmode`, which are independent bootable images and
much smaller targets than the whole retailOS.

What **doesn't** work: `ipodpatcher` only reads the disk's firmware partition,
not the NOR. iPodLinux's `getflash` decodes the `aupd` updater that Apple
leaves on the disk, which is the image Apple *would write*, not a read of the
chip (and its SVN is dead).

## Pending

> **Corrected on 2026-08-09.** This list had two entries that are no longer
> true: *"the two cores… our `sleep_core` doesn't really sleep"* (resolved in
> `9d823ec76`: there are two ARMs and both really sleep and wake up) and
> *"Apple's firmware is still stuck in a system call with the supervisor stack
> uninitialized"* (that hypothesis was **withdrawn** in `d06ce3123` — it wasn't
> a system call, it was the ARM libc panic printer — and today the retailOS
> boots and can be navigated).

- **Audio.** Neither I2S nor codec: no sound, in either firmware.
- **USB.** Not modeled.
- **The CPU-COP mailbox** at `0x60001000`.
- **Running the VideoCore firmware.** The real `vmcs.bin` is uploaded to the
  emulated chip via DMA, but only the *protocol* is modeled: nobody executes
  that image.
