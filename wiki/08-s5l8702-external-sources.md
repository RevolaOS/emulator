# S5L8702 — external findings and corrections

Research done on third-party sources (Rockbox `master`, the
freemyipod wiki, `freemyipod/qemu`, `devos50/qemu-ios`). It complements and **corrects**
[02-s5l8702-hardware.md](02-s5l8702-hardware.md), which was written solely from
the preprocessed `s5l87xx.h`.

---

## 0. The most important thing: a QEMU model of the S5L8702 already exists

`github.com/freemyipod/qemu`, branch **`s5l8702-dev`**, has a complete S5L8702
SoC *and* an `ipod-classic` machine. Last commit seen: May 2026, under
active development. It is a fork of `lemon_jesus/qemu`, which in turn is a fork of
`devos50/qemu-ios` — i.e., the same base we are using.

```
hw/arm/s5l8702.c                  SoC
hw/arm/ipod-classic.c             machine (64 MiB, arm926, NOR on SPI0, IDE HDD)
hw/arm/ipod-nano3g.c              sister machine (32 MiB, NAND)
include/hw/arm/s5l8702.h          SoC memory map
hw/misc/s5l8702-{aes,sha,chipid,clk,clickwheel,jpeg,lcd,miu,
                 nand,nand-ecc,sysic,usbotg,usbphy}.c
hw/gpio/s5l8702-gpio.c   hw/i2c/s5l8702-i2c.c   hw/ide/s5l8702-ata.c
hw/ssi/s5l8702-spi.c     hw/timer/s5l8702-timer.c  hw/watchdog/s5l8702-wdt.c
```

It already has written the three devices that in 03-reuse-qemu-ios.md we took as
"must be done from scratch": **ATA**, **click wheel** and **MPU-interface LCD**.

Invocation of its machine:

```
-M ipod-classic -cpu arm926 -m 64M \
   -M bootrom=/path/s5l8702_bootrom.bin \
   -drive if=mtd,file=nor.bin,format=raw \
   -drive if=ide,file=hdd.img,format=raw
```

Init order in `s5l8702_realize()`: CPU → VIC0/VIC1 + edge glue → CLK →
AES → SHA → GPIO → SPI0-2 → I2C0-1 → timers → LCD (+ framebuffer at
`0x0FE00000`) → JPEG → 2×PL080 → ATA → click wheel → chipid → NAND + NAND-ECC →
bootrom (+ alias at 0) → IRAM0/IRAM1 → UARTs → MIU → USB OTG → USB PHY → SYSIC.

**Recommendation: start from there instead of continuing from scratch.**

---

## 1. Corrections to the memory map

`s5l87xx.h` defines many bases inside `#if CONFIG_CPU==S5L8700 ||
CONFIG_CPU==S5L8701`. If it is preprocessed carelessly, bases that are **not**
from the S5L8702 slip in.

| What 02 said | Correct for S5L8702 | Why |
|---|---|---|
| SPI0 = `0x3CD00000` | **SPI0 = `0x3C300000`** | `0x3CD00000` is the S5L8700/01 `SPI_BASE`. For the 8702, `SPIBASE(i)` rules: `0→0x3c300000`, `1→0x3ce00000`, `2→0x3d200000`. Confirmed by the freemyipod wiki and by `S5L8702_SPI*_BASE` in the QEMU fork. |
| `0x3CE00000` = ADC | **SPI1** | `ADC_BASE` is legacy S5L8700; in the classic the ADC is inside the PMU. |
| `0x3D200000` = RTC | **SPI2** | `RTC_BASE` is legacy. The classic's real RTC is in the PCF50635 (`CONFIG_RTC RTC_NANO2G`, `rtc-6g.c` calls `pmu_read_rtc`). |
| `0x39400000` = FMC (NAND) | **NAND = `0x38A00000`** | `FMC_BASE 0x39400000` is S5L8701 only. The 8702's NAND block is at `0x38A00000` — it is the same one Rockbox calls `ATA_UNKNOWN_BASE`. On the S5L8720 the equivalent (FMSS) is also at `0x38A00000`. |
| I²C1 not listed | **I²C1 = `0x3C900000`** | `IICCON(bus) = IIC_BASE + 0x300000*bus`. Matches `I2C1_MEM_BASE 0x3C900000` of the Touch 2G. (⚠️ the freemyipod fork uses `0x3C610000`; it looks like a mistake on their part.) |
| `0x38600000` = USB host, `0x38800000` = USB function | **do not apply** | they are S5L8700/01. |
| `0x39C00000` = INT_BASE | **does not apply** | the legacy ICU is replaced by the two PL192s. |
| `0x3C600000` = `MS_BASE` (memory stick) | **does not apply** | collides with I²C0; it is legacy. |

New bases that were missing:

| Base | Block | Source |
|---|---|---|
| `0x38A02000` | NAND ECC engine | `S5L8702_NAND_ECC_BASE` |
| `0x38E02000` | VIC edge config block (`VIC0EDGE0/1`, `VIC1EDGE0/1` at +0x00/04/08/0C) | `s5l87xx.h` |
| `0x39600000` | **JPEG** decoder | freemyipod wiki |
| `0x3CD00000` | I²S1 (`I2S_BASE + 0x300000`) | `s5l87xx.h` |
| `0x3D400000` | I²S2 (`I2S_BASE + 0xA00000`) | `s5l87xx.h` |

---

## 2. Bootrom and reset — resolved

- The bootrom lives at **`0x20000000`** and is **also mapped at `0x00000000`**,
  "because that is where the ARM core expects the interrupt vectors"
  (freemyipod.org/wiki/Boot_Process).
- Size: the Nano 3G memory map gives `0x20000000–0x2000C800` = **50 KiB**.
  The freemyipod model reserves **64 KiB** (`S5L8702_BOOTROM_SIZE 0x10000`).
  They are not reconciled.
- After boot, **IRAM0 becomes mapped at `0x0`**. Rockbox says so
  explicitly in `crt0.S`: *"we do not want to execute from 0x0 as iram will be
  mapped there"*, and in `boot.lds`: *"s5l8702/s5l8720 maps address 0 to ROM, IRAM
  or DRAM"*.
- ⚠️ **The register that controls that remap does not appear in any source.** Neither
  Rockbox nor the freemyipod model programs it; the model simply leaves the
  ROM alias at 0 permanently. We can do the same for now.
- DFU payloads are loaded and executed at **`0x2200030C`** (`mks5lboot
  --mkdfu-raw`).

### Is there a public bootrom dump?

**I could not confirm any public download.**

- The freemyipod wiki publishes the SHA-256
  `9cc05fb8…bfbafb` but it is from a **Nano 4G (S5L8720)** ROM, not the 8702, and
  it does not publish the binary.
- A dump of the 8702 does in fact exist in the community: both machines in the
  freemyipod fork require the `bootrom=<file>` property and write it to
  `0x20000000`; they are being actively developed against it.
- Neither of the two repos includes the binary in the tree (I checked).
- The documented route is to dump it yourself: *"To dump the firmware of any iPod
  classic or iPod nano from version 3 on you need to run own code on the device"*
  (wiki, `Dumping_firmware`). `freemyipod/wInd3x` has `cmd_dump.go` and a path
  for the Nano 3G (same SoC), but its device table **does not include the iPod
  classic**.

**Design implication:** make the bootrom an *optional* property of the
machine and offer an alternative path that loads an already-decrypted IM3 body
directly at `IRAM1 + IM3HDR_SZ` and sets the PC there — which is exactly what
the real second stage sees (`boot.lds`: `MOVE_AREA : ORIGIN = IRAM1_ORIG + IM3HDR_SZ`).

---

## 3. Boot chain

```
Power on
 └─ BootROM @0x20000000 (alias at 0x0), ~50 KiB, mask ROM
     ├─ stacks/modes, starts PLLs, opens AES/NAND/NOR/USB clock gates
     ├─ checks GPIO straps / buttons (set by the click wheel controller)
     └─ chooses: (a) 2nd stage from NOR  ← THIS is the classic 6G path
                 (b) 2nd stage from NAND ← nano 3G
                 (c) DFU over USB        ← fail-safe / SELECT+MENU ~12 s
     └─ verifies the IMG1/IM3 image, decrypts it and jumps to its entrypoint
 └─ 2nd stage: "bootloader" (from NOR/NAND) or "WTF" (via DFU)
     ├─ EFI-based, modular; brings up DRAM, LCD, UART, the VICs, (FTL on nano)
     └─ loads and verifies the next IMG1
 └─ OSOS — Apple's RetailOS, read from the **hard disk**
```

### Classic 6G NOR layout (`norboot-target.h`)

```
 1 MiB        ┌──────────────┐
              │  "flsh" dir  │  (last 0x200 bytes)
 1MiB-0x200   ├──────────────┤
              │ Files 1..N   │  "flsh" file system
              ├──────────────┤
              │    Unused    │
 160 KiB      ├──────────────┤
              │  Original    │  IM3 image, max 128 KiB
              │  NOR Boot    │    0x000–0x800 : IM3 header
  32 KiB      ├──────────────┤    0x800–...   : EFI volume
              │    SysCfg    │
      0       └──────────────┘

NOR_SZ 0x100000   NORBOOT_OFF 0x8000   NORBOOT_MAXSZ 0x20000   SPI_PORT 0
```

`SysCfg` is a blob with magic `'SCfg'` and 9 entries on the classic 6G: `SrNm`,
`FwId`, `HwId`, `HwVr`, `Codc`, `SwVr`, `MLBN`, `Mod#`, `Regn`. A plausible one
will have to be synthesized.

The Rockbox dual bootloader is installed **in the NOR** at `NORBOOT_OFF`; Rockbox
itself is loaded from the **FAT32 hard disk**.

### IMG1 / IM3 format

```c
struct Im3Info {
  u8  ident[4];      /* "8702" */
  u8  version[3];    /* "1.0"  (the 8720 uses "2.0") */
  u8  enc_type;      /* 1 SIGNED_ENCRYPTED, 2 SIGNED,
                        3 X509_SIGNED_ENCRYPTED, 4 X509_SIGNED */
  u32 entry;         /* LE */
  u32 data_sz;       /* LE */
  union { enc12 { u8 data_sign[16]; u8 rsvd[32]; }
          enc34 { u32 sign_off; u32 cert_off; u32 cert_sz; u8 rsvd[36]; } } u;
  u8  info_sign[16]; /* AES(GID, SHA1(hdr[0:0x40])[0:16]) */
} __packed;

IM3HDR_SZ = 0x800   /* the S5L8720 uses 0x600 */
```

Verification: SHA-1 of the first `0x40` bytes, encrypted with the **GID key**,
compared against `info_sign`; then an X.509 chain over the body, with the root pinned in
the ROM (SHA-1 fingerprint `61:1e:5b:66:…:6c:60`, Apple Root CA) and the leaf serial
starting with `01:fb:01:fb` or `01:fb:00:fb`. iPod images use the GID key
for **all** AES (no "key 0x837"), and formats 1/2 decrypt
using the engine's *encryption* direction. 1.0 images sent via DFU
carry a CRC32 at the end.

AES engine keys (`crypto-s5l8702.h`): `HWKEYAES_GKEY = 1` (model key /
GID), `HWKEYAES_UKEY = 2` (device-unique key / UID). Direction:
`AESKEYLEN = 9` encrypts, `8` decrypts. You must write `AESTYPE2 = ~AESTYPE`.
It is a **bus-master** engine: `AESINADDR`/`AESOUTADDR`/`AESAUXADDR` are physical
pointers and Rockbox flushes the dcache before `AESGO = 1`.

---

## 4. Click wheel — important correction

02-s5l8702-hardware.md said *"Click wheel: Mikey controller (CD3272-like), over
SPI"*. **That is incorrect**; it mixes up two different things:

- **"Mikey"** is the controller for the **jack microphone and the headphone
  remote**, and it sits on **I²C0 at address `0x72`**. It does not exist on the
  2007 models. (`ipod6g/mikey-target.h`: *"Mikey is the internal controller
  for the headphone jack microphone and inline remote (I2C bus 0, address
  0x72)"*.)
- The **click wheel** has a **dedicated controller inside the SoC** at
  `0x3C200000` (`WHEEL_BASE`), clock gate 33, **IRQ 23**. Neither I²C nor SPI.

Registers and init sequence (`firmware/target/arm/ipod/button-clickwheel.c`):

```
0x00 WHEEL00   0x04 WHEEL04   0x08 WHEEL08   0x0C WHEEL0C
0x10 WHEEL10   0x14 WHEELINT  0x18 WHEELRX   0x1C WHEELTX
```

```c
clockgate_enable(CLOCKGATE_CWHEEL, true);
PCON14 = (PCON14 & ~0x00ffff00) | 0x00222200;  /* pins 2..5 of port 14 */
WHEEL00 = 0;            /* stop */
WHEELINT = 7;           /* clear the 3 int sources */
WHEEL10 = 1;
WHEEL00 = 0x380000;
WHEEL08 = 0x20000;
WHEELTX = 0x8000023A;   /* command to the wheel MCU */
WHEEL04 |= 1;           /* start */
```

ISR: read `WHEELINT`, write bits 4/2/1 back separately to
clear them, then read `WHEELRX`.

Packet format in `WHEELRX`: if `(status & 0x8000FFFF) == 0x8000023A`
then bit 16 = SELECT, 17 = RIGHT, 18 = LEFT, 19 = PLAY, 20 = MENU. The
absolute wheel position is **96 clicks per rotation**
(`WHEELCLICKS_PER_ROTATION 96`).

Related signals:
- **Hold switch**: read from the **PMU** (`pmu_holdswitch_locked()`), GPIO2 of the PCF50635.
- **Headphones inserted**: `PDAT(10) & (1<<6)`.
- Wheel LDO: PMU `LDO_CWHEEL = 6`.

---

## 5. UARTs — resolved

A single controller at `0x3CC00000` with **4 ports**, stride **`0x4000`**:

| Port | Base | IRQ |
|---|---|---|
| 0 | `0x3CC00000` | 24 |
| 1 | `0x3CC04000` | 25 |
| 2 | `0x3CC08000` | 26 |
| 3 | `0x3CC0C000` | 27 |

(`UARTC_N_PORTS 4`, `UARTC_PORT_OFFSET 0x4000`. IRQ 28 / port 4 is
marked as *"obsolete/not implemented on s5l8702"*, although the freemyipod
fork instantiates one anyway at `0x3CC10000`.)

Per-port register layout (Samsung S3C style, `firmware/export/uc87xx.h`),
16-byte FIFO:

```
0x00 ULCON   0x04 UCON    0x08 UFCON   0x0C UMCON
0x10 UTRSTAT 0x14 UERSTAT 0x18 UFSTAT  0x1C UMSTAT
0x20 UTXH    0x24 URXH    0x28 UBRDIV
0x2C UABRCNT 0x30 UABRSTAT   (autobaud)
0x34 UBRCONTX 0x38 UBRCONRX  (fine-tune)
```

**The serial console is port 0**, clocked by **ECLK = 12 MHz**
(`UCON_CLKSEL_ECLK`). On the classic 6G that line goes out through the **dock connector** and
by default carries Apple's accessory protocol (iAP); Rockbox
reuses it for `logf` if built with `LOGF_SERIAL`. Pin mux:
`PCON0 = (PCON0 & 0xff00ffff) | 0x00220000`. For 115200 at 12 MHz:
`UBRDIV = 6 | (0xffffff << 8)`. The mux for ports 2 and 3 is unknown.

---

## 6. Storage — the key point

The iPod classic 6G **boots from and stores the OS on a hard disk**, not NAND.
`CONFIG_STORAGE STORAGE_ATA`, `HAVE_DISK_STORAGE`, `HAVE_ATA_DMA`,
`ATA_MAX_UDMA 4`, `HAVE_LBA48`, `HAVE_ATA_SMART`. There is no NAND driver: the
nano 3G one is a stub (`nand_init() { /* TODO */ return 0; }`). The classic's board
has no NAND chip (freemyipod only lists SDRAM, the SPI NOR, the codec,
the PMU and the LTC4066). **No FTL is needed.**

There are **two** possible connections, chosen at runtime by a strap:

```c
ceata = PDAT(11) & BIT(1);   /* storage_ata-6g.c:1152 */
```

- `ceata == 0` → **parallel ATA** through the controller at `0x38700000`
  (clock gate 5, IRQ 29). 1.8" ZIF PATA disks.
- `ceata == 1` → **CE-ATA**: ATA taskfiles encapsulated in MMC commands through
  the **SDCI at `0x38B00000`** (clock gate 9, IRQ 44, `CEATA_MMC_RCA 1`).

For the model it is enough to implement the PATA path and leave `PDAT(11) bit 1` at 0.

⚠️ The ATA controller's taskfile registers are **32-bit, one per
MMIO word** — it is not a legacy 8-bit IDE block, so QEMU's generic `ide`
core does not work as is. Also, `ATA_PIO_READY & 2` gates
reads, and a prior dummy read is needed for the value to latch
(`ata_read_cbr()`).

---

## 7. Other things that were missing

**Clocks** (`CLK_BASE 0x3C500000`): classic 6G oscillators — `OSC0 = 12 MHz`
(crystal), `OSC1 = 32768 Hz` (provided by the PMU on the 6G; a crystal on the nano 3G),
`UNKOSC = 24 MHz`, `ECLK = 12 MHz` (timer reference). Rockbox clock
plan: `{cdiv 1, hdiv 2, hprat 2, hsdiv 4}` → CClk 216 / HClk 108 / PClk 54 /
SM1Clk 27 MHz. Software reboot: write `0x100000` to `WDT_BASE`.

**Timers** (`0x3C700000`): A–D are 16-bit (C and D appear unimplemented),
E–H are 32-bit at `0xA0/0xC0/0xE0/0x100` with shared status in **`TSTAT`
(`0x118`)**, and a 64-bit timer I at 5 µs at `0x80/0x84`. `USEC_TIMER` = `TECNT`
with `TEPRE = 11` and `TECON` bit 6 (ECLK).

**EIC** (`0x39A00000`): 7 groups × 32. `EIC_INTLEVEL(g) = 0x80+4g`,
`INTSTAT = 0xA0+4g`, `INTEN = 0xC0+4g`, `INTTYPE = 0xE0+4g`. GPIO pins
map to groups **6..3 in reverse order**: `EIC_GROUP(n) = 6 - (n>>5)`,
`EIC_INDEX(n) = (0x18 - (n & 0x18)) | (n & 7)`. Group 0: only bit 0 (end of the
"alive" timer) and bit 2 (USB cable). The groups go to IRQ **0,1,2,3,31,32,33**.
⚠️ the freemyipod fork puts them on IRQ 9..15 — Rockbox rules, since it is what
runs on the hardware.

**GPIO** (`0x3CF00000`): 16 groups × 8 pins = 128. Group stride **`0x20`**
(not `0x10`, that is the 8700/01). `PCON/PDAT/PUNA/PUNB/PUNC` at +0/4/8/C/10,
`GPIOCMD` at **`+0x200`**, `GPIOUNK380` at `+0x380`.

**DMA**: two PL080s. Useful request lines — DMAC0: `0x3` LCD_WR, `0xA/0xB`
I2S0 TX/RX, `0x4/0x5` SPI0, `0x6/0x7` UART0. DMAC1: `0x0/0x1` CE-ATA WR/RD.
Data addresses: LCD = `LCD_BASE + 0x40`, UARTn = `base + n*0x4000 + 0x20/0x24`.

**LCD** (`0x38300000`): **it is not a scanout framebuffer**, it is an
MPU/8080 interface to a panel with its own GRAM. Rockbox keeps a shadow buffer in
DRAM and pushes it via DMAC0 (request 3) to `LCD_WDATA` (`+0x40`). There are **four
different panels** depending on revision, and the type is read from **GPIO port 6 bits
[5:4]**: `lcd_info_list[(PDAT6 & 0x30) >> 4]`. Types 0 and 1 use an 8-bit command
set (ILI9340-like), types 2 and 3 a 16-bit one (ILI9320/ILI9326-like).
All `PAR18`. The backlight is **not** the SoC's: it goes through the PMU (`pmu_write(0x28,
brillo)`, `pmu_write(0x29, on/off)`).

**I²C0 — classic 6G devices** (8-bit addresses as Rockbox uses them,
7-bit ones in brackets):

| Addr | Device |
|---|---|
| `0xE6` [`0x73`] | NXP PCF50635 PMU |
| `0x94` [`0x4A`] | Cirrus Logic CS42L55 codec |
| `0x72` [`0x39`] | "Mikey" (jack mic + headphone remote) |

**Audio**: I²S0 at `0x3CA00000` for playback and recording. Codec reset on
**GPIO port 3 bit 3** (`PDAT(3) &= ~8`), MCLK with `CLKCON3` bit 15.
`I2STXCON = 0x0B100019`, `I2SCLKCON = 1`, `I2STXCOM = 0xE` start / `0xA` stop.
**There is no I²S model in either of the two QEMU forks** — it would have to be
written.

**USB**: DWC OTG at `0x38400000`, **9 endpoints** (`0b…111101010000000111101011`),
6 used, dedicated FIFOs (the 8701 does need `USB_DW_SHARED_FIFO`). PHY at
`0x3C400000` with `OPHYPWR 0x00`, `OPHYCLK 0x04`, `ORSTCON 0x08`, `OPHYUNK3 0x18`,
`OPHYUNK1 0x1C`, `OPHYUNK2 0x44`. Cable detection via **EIC group 0 bit 2 →
IRQ 33**.

---

## 8. Still unknown

1. The **remap register** for `0x0` (ROM → IRAM0/DRAM).
2. Exact size of the bootrom (50 KiB vs 64 KiB).
3. A public, downloadable dump of the S5L8702 bootrom.
4. The **part numbers of the four LCD panels** of the classic 6G.
5. Pin mux of UART2 and UART3.
6. Semantics of the `WHEEL00/04/08/0C/10` registers beyond the magic values.
7. Which exact register the firmware touches at `0x38A00000` during disk power-up.
8. Whether the S5L8702 has a PKE block at `0x3D000000` (the 8720 does; clock gate
   45 is described as *"related to 0x3d00_0000 (secure boot ???)"*).

## New sources

- `raw.githubusercontent.com/Rockbox/rockbox/master/firmware/export/s5l87xx.h`
  (⚠️ `firmware/target/arm/s5l8702/s5l8702.h` does **not** exist)
- `firmware/export/uc87xx.h`, `firmware/target/arm/ipod/button-clickwheel.c`,
  `firmware/target/arm/s5l8702/norboot-target.h`, `utils/mks5lboot/README`
- `github.com/freemyipod/qemu` branch `s5l8702-dev`
- `github.com/freemyipod/defs` (`memmap.py`, `clockgates.py`)
- `github.com/freemyipod/wInd3x` (`pkg/devices/devices.go`)
- freemyipod.org/wiki/: `Boot_Process`, `Bootrom`, `IMG1`, `Nano_3G/Memory_Map`,
  `Classic_6G`, `Dumping_firmware`
