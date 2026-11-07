# The real iPod classic 6G: talking to emCORE and decrypting its firmware

Session of 2026-08-09 against the user's physical device. The important
result: **Apple's `osos` is decrypted**. That was the foundational blocker
of the project, documented in [01-firmware-ipsw.md](01-firmware-ipsw.md)
(entropy 7.99 bits/byte, zero ASCII strings).

Tool written in this session: `tools/ipod_classic/emcore_s5l8702.py`,
with tests in `tools/ipod_classic/test_emcore_s5l8702.py`.
The dumps go to `assets/classic6g/`, which is **gitignored on purpose**:
they are Apple binaries and are not ours to redistribute.

## 0. What the device runs — VERIFIED

It has no hard disk. It boots into emCORE, the freemyipod debugger, which exposes
a vendor-specific USB interface.

```
Bus 001 Device 002: ID ffff:e000 freemyipod.org emCORE Debugger
```

Response to the identification command (command 1, subtype 0):

| Field | Value |
|---|---|
| Version | **v0.2.3 r859** |
| `swtypeid` | 2 = *emCORE Debugger* |
| `hwtypeid` | `0x4C435049` = `"IPCL"` in little-endian = iPod Classic |
| Packet limits | `cout`/`cin` 512 B, `dout`/`din` 262144 B |
| Malloc pool | `0x08000000` – `0x0BFC0000` (63.75 MiB of free DRAM) |

r859 (2012-01-02) is the last official *release*, so it is what
anyone who installed emCORE through the normal channel has. This matters a lot: the
protocol changed completely afterwards.

## 1. The protocol (pre-r891)

In SVN **r891** emCORE threw away its USB stack and switched to doing the debugger over
control transfers on EP0. Everything here is the **earlier** protocol,
which is the one this device speaks.

Four bulk endpoints, assigned by **order of appearance in the descriptor**
(`emcore/trunk/usb/usb.c:696-699`):

| Index | Use | On this device |
|---|---|---|
| 0 | command OUT | `0x02` |
| 1 | command IN | `0x81` |
| 2 | data OUT (DMA) | `0x04` |
| 3 | data IN (DMA) | `0x83` |

**16-byte** header, all little-endian. In the command, the first 4
bytes are the ID; in the response, the status code:
`0` invalid, `1` OK, `2` not supported, `3` busy.

Response formats that were needed and that are pinned down in the tests:

```
cmd 1/0 (version)      <I I B B B B I   status, revision, major, minor, patch, swtypeid, hwtypeid
cmd 1/1 (packetsize)   <I H H I I       status, coutmax, cinmax, doutmax, dinmax
cmd 1/2 (malloc pool)  <I I I I         status, lower, upper, unused
```

Memory read: command **4** over the command pipe (maximum
`cin` − 16 = 496 B per transaction) or command **6** over the data pipe with DMA
(up to 256 KiB). The DMA path requires 16-byte alignment, so
requests are split into (head / aligned body / tail) — it is
`libemcore`'s `_alignsplit`, replicated and with its own test.

Measured: **~8 MiB/s** end to end writing, encrypting and reading.

### The official clients do not work as is — VERIFIED

| Client | What happens |
|---|---|
| HEAD | Only accepts `ff/00/00` interfaces; the device is `ff/ff/ff`, so it does not even enumerate it (`libemcore.py:1172`). And it would use EP0, which this emCORE does not implement. |
| r859 | Calls `dev.write(ep, data, interface, timeout)`. The `interface` parameter disappeared in pyusb 1.0.0b1 → `TypeError` with any modern pyusb. |
| both | `misc.py:364` uses `inspect.getargspec`, removed in Python 3.11. |

That is why `emcore_s5l8702.py` speaks the protocol directly and does not depend on the
emCORE tree. It only implements read commands plus the bare minimum
for AES; it does **not** implement bootflash writing (23), `execimage` (21),
`runfirmware` (24) or anything for storage/NAND. Memory writes are
rejected outside the malloc pool.

## 2. Decrypting the firmware — DONE, VERIFIED

The GID key lives in the silicon and cannot be extracted. What can be done is
to ask the device itself to decrypt: command **25**, which exposes the S5L8702's AES
engine.

```
offset 0  (4B) ID = 25
offset 4  (1B) 0 = decrypt, 1 = encrypt
offset 5  (1B) zero
offset 6  (2B) key index
offset 8  (4B) buffer address (in place)
offset 12 (4B) size
```

Key indices (Rockbox, `firmware/target/arm/s5l8702/crypto-s5l8702.h:83-87`):
**1 = GKEY (the GID, per model)**, 2 = UKEY (unique per device).

Which one is right was confirmed experimentally, on 4 KiB of the `osos`:

| keyidx | Resulting entropy | Words with `cond=0xE` | Verdict |
|---|---|---|---|
| — (encrypted) | 7.953 | — | — |
| **1 (GID)** | **5.679** | **73.9 %** | **ARM code** |
| 2 (UID) | 7.954 | 6.0 % | noise |

### The detail needed for chunking: the IV is reset

`hwkeyaes()` sets `AESIV[0..3] = 0` **at the start of every call**
(`emcore/target/ipodnano3g/hwkeyaes.c`, identical in Rockbox). It is AES-128-CBC
with IV = 0 per invocation.

Naive chunking gets the **first block of each chunk** wrong. Since in CBC
`P[i] = D(C[i]) ^ C[i-1]`, the seam repair is exact:

```
P[0]_good = P[0]_returned ^ (last ciphertext block of the previous chunk)
```

Checked against the device: decrypting `[0,0x2000)` in one go, versus
decrypting `[0,0x1000)` and `[0x1000,0x2000)` with the repair, gives **byte for byte
the same**. There is also an offline test with software AES.

The size must be a multiple of 16; leftover bytes at the end are copied
as is (the `osos` is `0x976F7C` long, which leaves a 12-byte tail).

### Result

```bash
python3 tools/ipod_classic/emcore_s5l8702.py decrypt \
        assets/images/1_osos.payload assets/classic6g/osos_decrypted.bin
```

| File | Size | Entropy before → after | SHA-1 of the plaintext |
|---|---|---|---|
| `osos_decrypted.bin` | 9 924 476 | 8.000 → **6.005** | `18d09b8928f4b0b6f1d500ab5a31bdf76f294635` |
| `aupd_decrypted.bin` | 1 164 356 | — → **4.529** | `52e0cc6d868c962e8b23103d6bf0942130c29303` |

The 9.9 MB took **1.2 seconds**. (giek's guide talks about ~2 hours: that
is the wInd3x route via DFU, which is something else. Here the device is already running
emCORE and the AES engine goes through DMA.)

That the plaintext really is Apple's retailOS, and not a statistical fluke:

- ARM vectors at `0x0`: eight `b` branches, with coherent targets
  (`0x8834` the reset, and `0x33E8`–`0x3410` the exception handlers).
- `Copyright 2001-2008 Apple Inc. All rights reserved.`
- The literal interface: `Cover Flow`, `Playlists`, `Artists`, `Albums`,
  `Compilations`, `Shuffle Songs`, `Now Playing`.
- Apple class names: `TSilverMediaListCntlr_Playlists`,
  `TPhotosSettingsCntlr`, `HandleNikePlusSelected`.
- `RTXC v3.2b for ARM and Thumb - ARM ADS 1.0.1` — the RTOS.
- OpenSSL, FreeType and Eric Young licenses.
- 42 014 ASCII strings of ≥8 characters, versus **0 real ones** before.

And in the `aupd`, Apple's own build paths:

```
c:\bwa\N25FirmwareWin-435\srcroot\Firmware\BootLoader\btMm.c
/cygdrive/c/bwa/ImosEFIWin-120/srcroot/BuildResults/N25/Release/ARM/SecCore.stripped
```

`N25` is the iPod classic's code name.

## 3. The SPI NOR — VERIFIED

Command **22** (`bootflashread`) copies flash → RAM. It issues SPI opcode 3
(READ DATA): it cannot modify anything. 1 MiB dumped,
SHA-1 `46aa43136275f7abfe65b95c6752f358d7f0c835`.

From `0x090000` onward everything is `0xFF` (erased).

### `SCfg` configuration block at `0x0000`

20-byte entries. The 4-character tag is stored **reversed**,
because in the code it is a little-endian `uint32` constant: in the file it
reads `gfCS` and the tag is `SCfg`. The *values* are in normal order.

| Tag | Meaning | Value on this device |
|---|---|---|
| `SCfg` | header | size `0xCC`, 9 entries |
| `SrNm` | serial number | *(identifying — it is in the dump)* |
| `FwId` | firmware id | `0x13118B17`, `0x000A2700` |
| `HwId` | hardware id | `0x82021685` |
| `HwVr` | hardware version | `0x00130000` |
| `Codc` | audio codec | `"SB"` |
| `SwVr` | software version | `"1.0"` |
| `MLBN` | logic board no. | *(identifying — it is in the dump)* |
| `Mod#` | Apple model | `"MB147"` |
| `Regn` | region | `0x00020001` |

### The emCORE directory at `0x1000`

16-byte entries: 8-character name, offset, size. It is the user's emCORE
installation, not Apple material.

| Name | Offset | Size |
|---|---|---|
| `libboot` | `0x02000` | `0x002CE` |
| `libpng` | `0x03000` | `0x0131E` |
| `umsboot` | `0x05000` | `0x02362` † |
| `emcore` | `0x0A000` | `0x0AE02` † |
| `libui` | `0x15000` | `0x018C9` |
| `libmkf32` | `0x17000` | `0x00339` |
| `bootmenu` | `0x18000` | `0x24091` |
| `rockbox` | `0x3D000` | `0x4C72E` † |

† these have bit 31 set in the size field. **I do not know what it means** — I
have not verified it against the code.

### The `8702` image at `0x8000`: decrypted, and **it is not Apple's**

It falls in the gap between `umsboot` (ends at `0x7362`) and `emcore` (`0xA000`), and
**it is not in the emCORE directory**. The header is the same `8702`/`1.0`
container as the `.ipsw`'s `osos`, but of type `0x01` instead of `0x03`:

```
@0x008000  magic='8702' ver='1.0' enc_type=0x01  entry=0x00000000  data_sz=0xE10 (3600)
```

`0x8000` is not just any place: it is `NORBOOT_OFF`, the NOR boot slot
(`assets/ref/firmware__target__arm__s5l8702__norboot-target.h:81`), and the
map in a comment of that same file (lines 30-70) labels it
*"Original NOR Boot"*, with the IM3 header at `+0x000` and body at `+0x800`.

> **The first version of this page said, marked as INFERRED, that it was
> Apple's original NOR loader. That is FALSE.** It is decrypted and says
> literally `emCORE Loader v0.1.0 r859`. The inference was reasonable and it was
> wrong; what killed it was measuring instead of reasoning.

#### The header was misparsed — `fwtool.py` only works for `enc_type` 3/4

The correct structure is `struct Im3Info`, in
`assets/ref/firmware__target__arm__s5l8702__crypto-s5l8702.h:50-70`:

| Offset | Field | In this image |
|---|---|---|
| `0x00` | `ident[4]` | `"8702"` |
| `0x04` | `version[3]` | `"1.0"` |
| `0x07` | `enc_type` | `0x01` |
| `0x08` | `entry[4]` LE | `0x00000000` |
| `0x0C` | `data_sz[4]` LE | `0xE10` |
| `0x10` | union: `enc_type` 1/2 → `data_sign[16]` + 32 B reserved;<br>`enc_type` 3/4 → `sign_off`, `cert_off`, `cert_sz` + 36 B | `39d2c14f a34171ce 424a6a0b e2b9b51c` |
| `0x40` | `info_sign[16]` | `db239026 2759c80a 6a8330d3 2bd33b61` |

`tools/ipod_classic/fwtool.py:67-107` implements **only** the 3/4 variant and
calls `unknown_08` what is the entry point. On the `osos` it gets it right
because the `osos` is `enc_type=0x03`; on this image it gave
`body_size=0x4fc1d239`, which is garbage — it was the first four bytes of the
signature. New, correct tool: `tools/ipod_classic/norboot_im3.py`.

#### Which key: it is not guessed, it is checked — VERIFIED

`im3_sign` (`crypto-s5l8702.c:117-123`) is
`AES_encrypt(keyidx, SHA1(data)[0:16])`, AES-128-CBC with IV = 0. That is a
**checkable predicate**: we compute the SHA-1 on the PC, ask the device
to encrypt the 16 bytes with each key index, and compare with the stored
signature. There is no need to try and see what "looks like code".

| What is signed | Key 1 (GKEY/GID) | Key 2 (UKEY, unique to the device) |
|---|---|---|
| `Im3Info[0:0x40]` → `info_sign` | `309eb6cf…` no | `db239026…` **matches** |
| body plaintext → `data_sign` | `28cc742d…` no | `39d2c14f…` **matches** |

That is: decrypting with key 2 produces a plaintext whose truncated SHA-1,
encrypted by the device's own engine, reproduces **byte for byte** the 16 bytes
that Apple/emCORE left at `+0x10`. That is not an aesthetic judgment about a
disassembly: it is a cryptographic check.

And it identifies the author along the way: **UKEY is unique per device**, so an
image signed with it cannot come from the factory. It was written by the emCORE
installer on this particular iPod. It is exactly what `im3_crypt`
(`crypto-s5l8702.c:126-133`) does, whose comment says *"only supports enc_type 1
and 2 (UKEY)"*.

```bash
python3 tools/ipod_classic/norboot_im3.py \
        assets/classic6g/bootflash_nor_1MB.bin assets/classic6g 0x8000
```

| File | Size | SHA-1 |
|---|---|---|
| `assets/classic6g/norboot_emcoreldr_decrypted.bin` | 3 600 | `130b9b21f9b2531de01966d259622d6bfb047599` |

Entropy 7.948 → **6.458**; 54.9 % of words with `cond = 0xE`; and the strings
`emCORE Loader v0.1.0 r859`, `Loading emCORE...`, `File not found!`, plus the
8-character file name `emcore  ` that it looks for in the directory at
`0x1000`.

#### Apple's NOR loader is NO LONGER on this device — negative finding

`assets/ref/giek/architecture_BOOT_CHAIN.md` says there is a primary copy at
`0x08000` and a **backup copy at `0x22000`**. Measured on the real dump:
in the entire megabyte of NOR there is **a single** occurrence of the `8702` signature,
the one at `0x8000`. `0x22000` falls inside `bootmenu` (`0x18000`+`0x24091`), so
the emCORE installer overwrote it. There is no ONB to recover this way.

It is not in the `.ipsw` either: `aupd_decrypted.bin` and `osos_decrypted.bin` do not
contain any `8702` header (searched: 1 occurrence in `aupd`, which does not
have `"1.0"` after it, and 0 in `osos`).

#### What the loader does, instruction by instruction

The bootrom loads the IM3 body at `IRAM0_ORIG = 0x22000000` and jumps there
(`assets/ref/bootloader__ipod-s5l87xx.c:306-318`), which matches
`entry = 0`. Disassembled with that base:

```bash
arm-none-eabi-objdump -D -b binary -m arm --adjust-vma=0x22000000 \
    assets/classic6g/norboot_emcoreldr_decrypted.bin
```

It is hand-written, very compressed assembly: it loads its constants with
`add sp, pc, #N` followed by `pop {…}`, not with pool `ldr`. The first table
is at `+0x23C` and the second at `+0x260`.

**`+0x00`–`+0x34`, what the bootrom leaves done.** The loader starts with
`msr CPSR_c, #0xD3` (SVC, IRQ and FIQ masked), `mrc`/`bic #1`/`mcr` on
CP15 c1 — that is, it **turns off the MMU** —, invalidates the ICache (`c7,c5,0`) and TLB
(`c8,c7,0`), sets the FCSE PID to 0 (`c13,c0,0`) and runs the loop
`mrc p15,0,pc,c7,c14,3` (DCache *test-clean-and-invalidate*) until
`Z` comes out. INFERRED, and with whatever force defensive code carries: the bootrom hands it
control **with the MMU on and the caches dirty**.

**`+0x38`–`+0x64`, its own MMU.** `fp = 0x2203C000` is the L1 table (16 KiB,
aligned); DACR = `0xFFFFFFFF` (all domains in *manager*); the initial
descriptor is `ip = 0x00000C1E` = section, `AP = 11`, domain 0, `C = 1`, `B = 1`.
The loop writes one identity section per MiB, and at `0x38000000`
(`biccs ip, ip, #12`) it clears `C` and `B` — that is, a **flat 1:1 map of
`0x00000000`–`0x3FFFFFFF`, cacheable below and uncached from `0x38000000`
up**. It stops upon reaching `0x40000000` (`tst ip, #0x40000000`). Then
`mcr p15,0,sl,c1,c0,0` with `sl = 0x0005107D`: MMU on, DCache on, *write
buffer* on, ICache on, alignment off, *little-endian*.

A tricky detail: it only writes 1024 entries (4 KiB) of a table that occupies
16 KiB, and right after that it uses `0x2203D000` — inside the same table — for something
else. The entries for VA ≥ `0x40000000` are left uninitialized and then
clobbered. It is deliberate and it works because nothing looks up there.

**`+0x68`–`+0x8C`, and this is where the interesting part is: the pixels.** The loop writes
groups of four words with `stmia fp!, {r6, r7, r8, r9}` — which is
exactly a PL080 LLI `{src, dst, next, control}` — with:

| Register | Value | What it is |
|---|---|---|
| `r6` | `0x22010000` | source: framebuffer in IRAM; advances `r4 << 1` → 16 bits per item |
| `r7` | `0x38300040` | fixed destination: **`LCD_WDATA`** |
| `r8` | `0x2203D000` | next LLI; `+16` each iteration; `0` in the last one |
| `r9` | `0x74240000` | control word, with the size put in via `orr` |
| `r4` | `0xFFF` | maximum per LLI |
| `r5` | `0x12C00` | **76 800 items = 320 × 240** |

`0x38300040` is `LCD_WDATA` per `assets/ref/s5l87xx.h:1486`
(`LCD_BASE + 0x40`, with `LCD_BASE = 0x38300000` on line 1473). And one does not have
to rely on that alone: `assets/ref/firmware__target__arm__s5l8702__dma-s5l8702.h:86`
defines `S5L8702_DADDR_PERI_LCD_WR (LCD_BASE + 0x40)`, that is, that same
address **is the one Rockbox uses as the DMA destination for the LCD**, and line
43 assigns it the request line `S5L8702_DMAC0_PERI_LCD_WR = 0x3`.

The control word `0x74240000` breaks down like this (PL080 `DMACCxControl`):
`I = 0`, `Prot = 7`, `DI = 0` (destination **not** incremented → it is a FIFO),
`SI = 1` (source is), `DWidth = 001` = 16 bits, `SWidth = 001` = 16 bits,
bursts of 1, and bits 11:0 (`TransferSize`) are set by `orr r9, r9, r4`.

76 800 16-bit items = 153 600 bytes = **one 320 × 240 RGB565 frame**,
which is the resolution of the classic 6G panel. 76 800 / 4095 → 19 LLIs.

> **VERIFIED: on the S5L8702 the framebuffer goes out via PL080 DMA to
> `LCD_WDATA`, not via CPU writes.** This answers, at least for
> emCORE, the question that [11-classic6g-retailos.md](11-classic6g-retailos.md)
> §3 left open as `UNDETERMINED` ("where the pixels go through"), and
> promotes candidate no. 4 of its §6 —*DMAC0, which is programmed with descriptors
> in DRAM and never completes*— from "to be ruled out" to **prime suspect**.
> That the retailOS uses the same path is `INFERRED`, not measured; see below.

**`+0x90`–`+0x158`, clocks and timer.** A second 13-register `pop`
from `+0x260` leaves `r0 = 0x3C500000`, which is `CLK_BASE` (`s5l87xx.h:147`).
It writes `+0x48 = 0xFDFFFFE5` and `+0x4C = 0xFFFFEFC9` (clock gates),
zero to `+0x00`, `+0x04` and `+0x60`, `+0x28 = 0x01002401`, `+0x38 = 0x00007E90`,
`+0x44 = 0x00040034`, and then **spins waiting for bit 2 of `+0x40`** —
PLL lock — before setting `+0x04 = 0x00404101` and `+0x00 = 0x3000`.
Then `+0x08` and `+0x0C = 0x80008000`, `+0x10` and `+0x14 = 0x8000`.
`r6 = 0x38501000` (`SM1_BASE + 0x1000`, `s5l87xx.h:514`) receives a `3`.
`r1 = r0 | 0x200000 = 0x3C700000` is `TIMER_BASE`: `+0xA0 = 0x440`,
`+0xB0 = 0x0B`, `+0xA8 = 0xFFFFFFFF`, `+0xA4 = 3`.

> For the QEMU machine this is an explicit contract: **bit 2 of
> `CLK_BASE + 0x40` has to rise on its own**, or any firmware that boots
> through this path keeps spinning. It is pattern 1 of
> [09-history-5g.md](09-history-5g.md).

**`+0x118`–`+0x158`, GPIO.** `r3 = 0x3CF00000` = `GPIO_BASE` (`s5l87xx.h:1370`).
A 16-iteration loop with a 32-byte stride (`PCON(i)` is at
`GPIO_BASE + (i << 5)`, `s5l87xx.h:1378-1381`) writes a constant to `+0x00`
(`PCON`) and zeros to `+0x0C` (`PUNB`) and `+0x10`, until the address reaches
`0x…200` = 16 × 32.

The rest (≈ `+0x160` onward) is the body of the loader: routines
`bl 0x22000980` and `bl 0x220009A0` that look like the I²C interface to the PMU
(constants `0x16`, `0x0C`, `0x19`, `0x4B` as registers), the 5 × 8
pixel font at `+0x5C0`–`+0x790` and the three text messages at `+0xDCC`.
`UNDETERMINED` in detail; I have not traced it fully.

#### An unidentified remainder at `0x9610`

Right behind the IM3 body (`0x8800 + 0xE10 = 0x9610`) and up to `0x9E10` there are
exactly `0x800` bytes that **are not in the emCORE directory** and start
with the signature `emCO` + `exec`, which is emCORE's image format:

```
4f436d65 63657865 00000001 00000030 000326a0 00000e80
00001000 00000000 000326d0 00000145 00000001 4f020571
   emCO     exec     ver     hdr_sz
```

Saved in `assets/classic6g/norboot_trailer_emco_0x9610.bin`
(SHA-1 `7d5e37b50dbd8287491e58544401eef3ecc89a7b`). Decrypting it with GKEY and with
UKEY leaves entropy 7.90 and 7.91 and ~5 % `cond=0xE`, i.e. **noise with both**:
it is not encrypted with any fused key. INFERRED: it is a compressed payload
(emCORE compresses its images), not encrypted. What it is and what it is for:
`UNDETERMINED`.

## 4. The bootrom CANNOT be read — negative finding, VERIFIED

`0x20000000` does not return the bootrom. It returns four constants, one for each
16 KiB bank, and the pattern repeats every 64 KiB:

| Range | Value returned | As an ARM instruction |
|---|---|---|
| `0x20000000`–`0x20003FFF` | `0xE3A01A01` | `mov r1, #0x1000` |
| `0x20004000`–`0x20007FFF` | `0x2000C094` | (a pointer within the window itself) |
| `0x20008000`–`0x2000BFFF` | `0xE3520004` | `cmp r2, #4` |
| `0x2000C000`–`0x2000FFFF` | `0x00000000` | — |

Verified that **it is not a client artifact**: reading 4 bytes at consecutive
addresses returns the same value over and over, while the same
method on the SRAM at `0x22000000` returns emCORE's real vector table.
Only bits 14-15 of the address have an effect; everything else is ignored.

Interpretation (INFERRED): the bootrom is **locked after boot** and the
bus delivers a residual value per bank. That two of the four values are
valid ARM instructions and the third a pointer into the range itself suggests they
are real remnants of its contents, but **they are not a usable dump**.

This explains why no public dump of the S5L8702 bootrom exists.
Getting it out would need another route (DFU + a wInd3x-style exploit), not emCORE.

## 5. How I hung the device — avoid

Reading **`0x20100000`** (just above the mirrored bootrom window)
brought down the monitor thread. The device stays enumerated on USB — the descriptor
reads perfectly — but it does not respond to any command: everything gives
`USBTimeoutError`. Almost certainly a data abort in the read handler.

emCORE has abort handlers (the strings `Data abort at %08X!` and
`Prefetch abort at %08X!` are in the SRAM dump), but they do not save the
monitor.

**Rule of thumb: do not read undocumented addresses.** The map known to be
good is DRAM `0x08000000`, SRAM `0x22000000`, and the peripherals that appear
in `firmware/export/s5l87xx.h`.

It is recovered with a manual reset of the device (MENU+SELECT). I did not do it:
I do not touch the user's device beyond reading.

## 6. What was read, and what remains

In `assets/classic6g/` (gitignored):

| File | Size | What it is |
|---|---|---|
| `osos_decrypted.bin` | 9 924 476 | **Apple's retailOS in the clear** |
| `aupd_decrypted.bin` | 1 164 356 | the updater in the clear |
| `bootflash_nor_1MB.bin` | 1 048 576 | the entire SPI NOR |
| `sram_22000000.bin` | 245 760 | the 240 KiB of SRAM (emCORE running) |
| `probe_20000000_64k.bin` | 65 536 | the proof of the locked bootrom |
| `norboot_emcoreldr_decrypted.bin` | 3 600 | **the NOR loader in the clear** (emCORE Loader r859) |
| `norboot_trailer_emco_0x9610.bin` | 2 048 | the unidentified `emCOexec` remainder |

Pending:

- ~~Decrypt the `8702` image at `0x8000`~~ **done** (§3). Result: it is
  emCORE's loader, not Apple's.
- **Measure whether the retailOS programs DMAC0 towards `LCD_WDATA`.** It is the direct
  continuation of the §3 finding and does not need the device. `0x38200000` (DMAC0)
  is today a simple register *stub* in `hw/arm/ipod_classic/ipod_classic.c:259`,
  so it is enough to:
  ```bash
  ./qemu-system-arm -M iPod-Classic -kernel ../assets/classic6g/osos_decrypted.bin \
      -display none -monitor none -serial null -drive if=ide,format=raw,file=/tmp/ipod.img \
      -d unimp,guest_errors -D /tmp/u.log
  grep -c 's5l8702.dmac0' /tmp/u.log
  ```
  The acid test: look for a write of `0x38300040` to
  `DMAC0 + 0x104 + n*0x20` (the `DstAddr` of channel *n* of a PL080). If it appears,
  the path to the retailOS pixels is identified. **I did not measure it**:
  another session was rebuilding the binary at the same time (`Text file busy`) and I
  did not want to clobber its link step.
- The `.ipsw` does not ship Apple's NOR loader and this device no longer has it
  (§3). If needed, it has to be pulled from an iPod classic **without** emCORE
  installed, or via wInd3x-style DFU.
- The bootrom is still not dumped and **is not reachable by this route**.
- The chip ID at `0x3D100000` (Rockbox's `CHIPID_BASE`) returns zeros.
  INFERRED: the block has its clock gated off. Turning it on would mean writing to
  a clock gate register — volatile, but it is a write, so I did not do it.
- Load `osos_decrypted.bin` in QEMU's `ipod-classic` machine. This is what
  unlocks the project's original goal.

## Sources

- [EmCORE Monitor Protocol](https://freemyipod.org/wiki/EmCORE_Monitor_Protocol)
- [EmCORE Releases](https://freemyipod.org/wiki/EmCORE_Releases)
- [github.com/freemyipod/freemyipod](https://github.com/freemyipod/freemyipod) — SVN mirror. r859 = commit `92b156a3b52f73f39153c1b24177b28e3c910cdd`
- `assets/rockbox/firmware/target/arm/s5l8702/crypto-s5l8702.{c,h}` — `struct Im3Info`
  (`.h:50-70`), `im3_sign` and `im3_crypt` (`.c:117-133`)
- `assets/ref/firmware__target__arm__s5l8702__norboot-target.h:30-81` — the NOR
  map and `NORBOOT_OFF`
- `assets/ref/firmware__target__arm__s5l8702__dma-s5l8702.h:43,86` —
  `S5L8702_DMAC0_PERI_LCD_WR` and `S5L8702_DADDR_PERI_LCD_WR`
- `assets/ref/s5l87xx.h:147,514,1370-1381,1473,1486` — `CLK_BASE`, `SM1_BASE`,
  `GPIO_BASE`/`PCON`, `LCD_BASE`, `LCD_WDATA`
- `assets/ref/bootloader__ipod-s5l87xx.c:306-318` — what the bootrom does with the
  NOR's IM3 image
- `assets/ref/giek/architecture_BOOT_CHAIN.md` — the backup copy at
  `0x22000` that turned out not to exist on this device
- `assets/ref/giek/DECRYPTION_GUIDE.md`
