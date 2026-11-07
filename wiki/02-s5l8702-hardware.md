# S5L8702 / iPod classic 6G — hardware

> **Updated on August 10, 2026.** This document was written when nothing had
> been modeled yet. Now the `-M iPod-Classic` machine boots Apple's retailOS
> to a navigable screen, and there are **fourteen peripheral blocks with real
> behavior** (plus the PMU behind I²C): the section
> ["What is modeled"](#what-is-modeled-in--m-ipod-classic) lists them, with
> what is known about each one and what is not. Two things that this map **could
> not provide**, and they are the ones that draw and provide input:
>
> - **`0x38900000`, the video compositor.** No external source names it
>   —neither Rockbox nor freemyipod—, only Apple's code. It is where the
>   pixels come from. See [11](11-classic6g-retailos.md) §13.
> - **`0x3C100000`**, a block that Rockbox only defines for the S5L8700 and that on
>   this chip behaves as a number generator. See [11](11-classic6g-retailos.md) §2.

Primary source: Rockbox's `firmware/export/s5l87xx.h`, resolved with the
preprocessor for `CONFIG_CPU == S5L8702`:

```bash
cpp -dM -P -nostdinc -D__ASSEMBLER__ \
    -DS5L8700=8700 -DS5L8701=8701 -DS5L8702=8702 -DS5L8720=8720 \
    -DCONFIG_CPU=8702 -DMEMORYSIZE=64 s5l87xx.h | sort > s5l8702_macros.h
```

Result in `assets/ref/s5l8702_macros.h` (1564 macros).
Everything in this document comes from there except where stated otherwise.

## Board

| Component | Part | Source |
|---|---|---|
| SoC | Samsung S5L8702, Apple marking `337S3473 8702` | freemyipod |
| CPU | ARM926EJ-S, 216 MHz (`CPU_FREQ 216000000`) | freemyipod + `config/ipod6g.h` |
| SDRAM | K4X51163PE | freemyipod |
| Utility flash | SST25VF080B (NOR over SPI) | freemyipod |
| Audio codec | Cirrus Logic **CS42L55** (I²C) | freemyipod + `cscodec-6g.c` |
| PMU | NXP **PCF50635** (I²C) | freemyipod + `pmu-6g.c` |
| USB charging | LTC4066 | freemyipod |
| Click wheel | **dedicated controller inside the SoC** at `0x3C200000`, IRQ 23 | `ipod/button-clickwheel.c` |
| Jack mic + headphone remote | **"Mikey"**, I²C0 addr. `0x72` (not the click wheel) | `ipod6g/mikey-target.h` |
| Display | 320×240, RGB565 (`LCD_DEPTH 16`), 160 dpi | `config/ipod6g.h` |
| Storage | **ATA HDD + CE-ATA over SDCI** | `ipod6g/storage_ata-6g.c` |

`MODEL_NUMBER 71`, `MODEL_NAME "Apple iPod Classic/6G"`,
`CONFIG_LCD LCD_IPOD6GNANO3G4G`, `HAVE_ATA_DMA`, `ATA_MAX_UDMA 4`.

## Memory map

### RAM / ROM

| Region | Base | Size |
|---|---|---|
| SDRAM (`DRAM_ORIG`) | `0x08000000` | 64 MB |
| IRAM0 (`IRAM_ORIG`) | `0x22000000` | `0x20000` (128 KB) |
| IRAM1 | `0x22020000` | `0x20000` (128 KB) |
| Bootrom | `0x20000000`, aliased at `0x0` ([08](08-s5l8702-external-sources.md)) | 50 or 64 KiB, unreconciled |
| Last 256 B of IRAM1 | `0x2203FF00` | parameter block that the bootrom leaves for the firmware |
| TTB (page tables, set up by Rockbox) | `DRAM_ORIG + DRAM_SIZE - 0x4000` | `0x4000` |

> **The bootrom cannot be dumped over USB, and this has been verified.** After
> boot, `0x20000000` returns one constant per 16 KiB bank: it is
> locked. See [10-classic6g-emcore.md](10-classic6g-emcore.md) §4. That is why the
> machine skips it and reproduces by hand what that parameter block
> contained — and that is why `IICCON = 0x184` is an inference and not a fact.
>
> The retailOS IRAM area **is not free**: the image has scatter loading,
> `file [0, 0x0AE48) -> 0x22000000` and the rest at `0x08000000`. See
> [11](11-classic6g-retailos.md) §1.

> The Touch 2G's S5L8720 has an IRAM1 of only `0x10000`; the S5L8702 has
> `0x20000`. 256 KB of IRAM in total.

### Peripherals (`IO_BASE 0x38000000`)

| Base | Rockbox macro | Block | Same on S5L8720? |
|---|---|---|---|
| `0x38000000` | `SHA1_BASE` | SHA-1 engine | ✅ yes |
| `0x38100000` | `MIU_BASE` | Memory controller | — |
| `0x38200000` | `DMA0_BASE` | DMAC0 (PL080) | ✅ yes |
| `0x38300000` | `LCD_BASE` | LCD controller | ❌ (2G: `0x38900000`) |
| `0x38400000` | `DMA_BASE` / `OTGBASE` | USB OTG (Synopsys DWC) | ✅ yes |
| `0x38500000` | `SM1_BASE` | SM1 clock divider | — |
| `0x38600000` | `USB_HC_BASE` | USB host controller | — |
| `0x38700000` | `ATA_BASE` | **ATA controller** | ❌ (2G: DMAC1) |
| `0x38800000` | `USB_FC_BASE` | USB function controller | — |
| `0x38900000` | **none** | **Video compositor.** It is not in `s5l87xx.h` nor in the freemyipod model: the name and the register map come from Apple's code | ❌ (2G: LCD) |
| `0x38a00000` | `ATA_UNKNOWN_BASE` | ATA / FMI auxiliary block | — |
| `0x38b00000` | `SDCI_BASE` | SD/MMC — used for **CE-ATA** | ❌ (2G SDIO: `0x38D00000`) |
| `0x38c00000` | `AES_BASE` | AES engine | ✅ yes |
| `0x38E00000` | `VIC_BASE` | VIC0 (PL192); VIC1 at `+0x1000` | ✅ yes |
| `0x39000000` | `ADM_SFR_BASE` | CalmADM2E (audio DSP) | — |
| `0x39200000` | `LCDBASE` | LCD windows/framebuffer | — |
| `0x39400000` | `FMC_BASE` | Flash Memory Controller (NAND) | — |
| `0x39900000` | `DMA1_BASE` | DMAC1 (PL080) | ✅ yes |
| `0x39a00000` | `EIC_BASE` / `SYSALV_BASE` | External interrupt controller | — |
| `0x39C00000` | `INT_BASE` | Legacy interrupt controller | — |
| `0x39E00000` | `ECC_BASE` | NAND ECC | — |
| `0x3C100000` | (`LCD_BASE` **only on S5L8700**) | Unnamed block; the retailOS uses it as a **number generator**, with three registers and no enable bit | — |
| `0x3C200000` | `WHEEL_BASE` | **Click wheel interface** | ❌ classic/nano only |
| `0x3C400000` | `USB_PHY_BASE` | USB PHY | ✅ yes |
| `0x3C500000` | `CLK_BASE` | Clock / PLL | ✅ yes |
| `0x3C600000` | `IIC_BASE` / `MS_BASE` | I²C 0 | ✅ yes |
| `0x3C700000` | `TIMER_BASE` | Timers | ✅ yes |
| `0x3C800000` | `WDT_BASE` | Watchdog | — |
| `0x3CA00000` | `I2S_BASE` | I²S (audio) | — |
| `0x3CB00000` | `SPD_BASE` | S/PDIF | — |
| `0x3CC00000` | `UARTC_BASE_ADDR` | UART0 (serial console) | ✅ yes |
| `0x3CD00000` | `SPI_BASE` | SPI0 | ❌ (2G: `0x3C300000`) |
| `0x3CE00000` | `ADC_BASE` | ADC | ❌ (2G: SPI1) |
| `0x3CF00000` | `GPIO_BASE` | GPIO | ✅ yes |
| `0x3D100000` | `CHIPID_BASE` | Chip ID | ✅ yes |
| `0x3D200000` | `RTC_BASE` | RTC (and SPI2 according to `SPIBASE(2)`) | — |
| `0x3D700000` | `MIU_BASE` (variant) | — | — |
| `0x3F000000` | `ADM_BASE` | CalmADM2E, data area | — |

The three SPIs are selected with `SPIBASE(i)`: `i==2 → 0x3d200000`, and the other
two follow the `SPI_BASE` pattern.

### UARTs

`UARTC_BASE_ADDR 0x3CC00000`. On the S5L8701 the stride is `0x8000`
(`UARTC1 0x3CC08000`, `UARTC2 0x3CC0C000`); for the S5L8702 the header only
defines channel 0 — **yet to be confirmed** whether there are more.

## VIC IRQ numbers

Extracted from `s5l8702_macros.h`. They go to the PL192 pair at `0x38E00000` /
`0x38E01000` (IRQs ≥ 32 belong to VIC1).

| IRQ | Source |
|---|---|
| 0–3 | `IRQ_EXT0..EXT3` |
| 7 | `IRQ_TIMER32` |
| 8 | `IRQ_TIMER` |
| 9, 10, 11 | `IRQ_SPI0/1/2` |
| 14 | `IRQ_LCD` |
| 16, 17 | `IRQ_DMAC0/1` |
| 19 | `IRQ_USB_FUNC` |
| 20 | `IRQ_NAND` |
| 21, 22 | `IRQ_I2C0/1` |
| 23 | **`IRQ_WHEEL`** (click wheel) |
| 24–28 | `IRQ_UART0..UART4` |
| 29 | **`IRQ_ATA`** |
| 31, 32, 33 | `IRQ_EXT4/5/6` |
| 36 | `IRQ_SECBOOT` |
| 39 | `IRQ_AES` |
| 40 | `IRQ_SHA` |
| 43 | `IRQ_NANDECC` |
| 44 | **`IRQ_MMC`** (CE-ATA) |

## What is modeled in `-M iPod-Classic`

State of the tree as of August 10, 2026. "Modeled" here means *has
behavior*, not *returns zeros without complaining*. Everything that does not appear in
this table is `create_unimplemented_device()` (24 blocks) or falls to the **I/O
floor** that covers the 768 MB of the peripheral window — which is needed because
the retailOS maps that whole window as an uncached identity section, so
an unmapped address is not a harmless zero, it is a *data abort*.

| Block | Base | File | What it can do, and what it cannot |
|---|---|---|---|
| CPU + RAM | — | `ipod_classic.c` | ARM926EJ-S, 64 MB SDRAM, IRAM0/IRAM1, and the **IRAM0 alias at `0x0`**, from which the ARM926 takes its exceptions |
| VIC0 / VIC1 | `0x38E00000` / `+0x1000` | `hw/intc/pl192.c` (stock) | 64 lines. The retailOS dispatcher lives in IRAM (`0x22004DA0`), reads both banks **with no cross condition** and serves one interrupt per entry |
| Timers | `0x3C700000` | `ipod_classic_timer.c` | 8 channels; A–D 16-bit on IRQ 8, E–H 32-bit on IRQ 7. **Interval mode only**; PCLK is treated as ECLK and a warning is issued via `-d unimp` if anyone selects it |
| Clock / PLL | `0x3C500000` | `ipod_classic_clk.c` | `SWRCON` really resets, `RSTSR`, `PLLMOD2`, and `PLLLOCK` reports as locked the PLLs that `PLLMODE` enabled |
| I²C0 + PCF50635 | `0x3C600000` | `ipod_classic_i2c.c`, `ipod_classic_pmu.c` | Full transaction with **status and control in separate registers** (`IICCON[13:8]` / `IICSTA2[13:8]`), a byte every **25 virtual µs**, PMU register file, ADC, and read-to-clear INT1..6. **No alarm, no charging, no regulators** |
| ATA + DMA | `0x38700000` | `ipod_classic_ata.c` | Apple's bridge, not a direct *task file*: reading a register starts a cycle whose result appears in `ATA_PIO_RDATA`. It really reads. Only bit 0 of `ATA_IRQSTAT`; the others are `UNDETERMINED` |
| **Compositor** | `0x38900000` | `ipod_classic_disp.c` | **What draws.** Five layer descriptors at `+0x058 + k*0x18`. It stores and **does not honor** the gamma tables, the color matrix, the scaler and the rotation bit: in every measured boot they are identity |
| LCD panel | `0x38300000` | `ipod_classic_lcd.c` | 320×240 RGB565, window `0x2A`/`0x2B`, writes after `0x2C`, `qemu_console`, and the frame engine registers `+0x70..+0x8C`. Composites in `gfx_update`, which is a **modeling decision** and is stated in the code |
| DMAC0 / DMAC1 | `0x38200000` / `0x39900000` | `hw/dma/pl080.c` (stock) | IRQ 16 and 17, **system memory as the master port** because the guest programs the channels with the cached alias while the CPU runs on the uncached one. 4 KB window, so that the rest still falls to the I/O floor |
| Click wheel | `0x3C200000` | `ipod_classic_wheel.c` | IRQ 23, both packet formats, five buttons and the wheel via keyboard, with the timing the firmware's decoder demands. 4 KB window |
| GPIO | `0x3CF00000` | `ipod_classic_gpio.c` | **One modeled pin**, 86, which the storage task translates to "disk present". The others read zero |
| EIC / SYSALV | `0x39A00000` | `ipod_classic_eic.c` | `ALVCON` as a plain register (standby writes it and waits to read it back), and the seven `INTLEVEL`/`INTSTAT`/`INTEN`/`INTTYPE` groups driving VIC 0–3 and 31–33. **One pin has a source**: GPIO 4, which the wheel pulls low on a key press and which wakes the firmware from standby. `0x7b` (the PMU interrupt) has no source because the PMU model has no interrupt output. Pins without a source never set `INTSTAT` |
| Generator | `0x3C100000` | `ipod_classic.c` | LCG seeded by the guest. Identity `INFERRED`, three-register contract verified, and **no enable bit** — gating "data ready" hangs the boot |
| UART0 | `0x3CC00000` | `hw/char/exynos4210_uart.c` (stock) | IRQ 24 |
| Bootrom parameter block | `0x2203FF00` | `ipod_classic.c` | The **three** fields the boot reads: hardware id (`19`, or the firmware reboots to `"softupdt"`), DRAM base and size. The other eight are left at zero on purpose |
| ARM semihosting | — | `ipod_classic.c` | On by default. **It is how the retailOS prints its panics** |

**What is not there**: audio (I²S and CS42L55), USB (OTG, host, function and PHY), NAND
(FMC/ECC), CE-ATA over SDCI, the CalmADM2E DSP, the RTC, the ADC, the SPI and its NOR,
the SoC's own AES engine and SHA-1, and the bootrom.

## Differences from the S5L8720 (iPod Touch 2G)

What **stays the same** between the two generations and can therefore be ported
almost as is: VIC0/VIC1 (`0x38E00000`), SHA-1 (`0x38000000`), AES
(`0x38c00000`), USB OTG (`0x38400000`), USB PHY (`0x3C400000`), I²C0
(`0x3C600000`), clock (`0x3C500000`), timers (`0x3C700000`), GPIO
(`0x3CF00000`), chip ID (`0x3D100000`), UART0 (`0x3CC00000`), DMAC0
(`0x38200000`), DMAC1 (`0x39900000`).

What **changes and has to be redone**:

- **CPU**: ARM926EJ-S instead of ARM1176. This changes `default_cpu_type` and the
  semantics of the cp15 registers that the Touch model patches.
- **LCD**: different base (`0x38300000` vs `0x38900000`), no MIPI DSI, 320×240
  parallel panel.
- **Storage**: the Touch uses NAND (FMSS/FMI); the classic uses an **ATA hard
  disk** at `0x38700000` plus **CE-ATA over SDCI** at `0x38b00000`. None of this
  exists in qemu-ios.
- **Input**: click wheel at `0x3C200000` (IRQ 23) instead of multitouch.
- **SPI**: different bases.
- **Do not exist** on the S5L8702: PKE, MIPI DSI, scaler/CSC, TV-out, MBX (GPU).
- **Exist only here**: CalmADM2E (`0x39000000` / `0x3F000000`), I²S
  (`0x3CA00000`), S/PDIF (`0x3CB00000`), ADC (`0x3CE00000`), RTC (`0x3D200000`).

## Yet to be confirmed

> ⚠️ **This document has errors that are corrected in
> [08-s5l8702-external-sources.md](08-s5l8702-external-sources.md)**, which also
> resolves almost everything in this section. Read it before using the base table:
> several entries above (`SPI0 0x3CD00000`, `ADC 0x3CE00000`, `RTC
> 0x3D200000`, `FMC 0x39400000`, `USB_HC/USB_FC`, `INT_BASE`, `MS_BASE`) are
> **legacy S5L8700/8701** bases that do not apply to the S5L8702.

Resolved in doc 08:

- **Bootrom**: `0x20000000`, aliased at `0x0`. Size 50 KiB (wiki) or 64 KiB
  (freemyipod model), unreconciled. The later remap to IRAM0 exists but
  the register that controls it is not known.
- **Public bootrom dump**: not confirmed; it has to be dumped from the hardware.
  **And it cannot be done over USB**: [10](10-classic6g-emcore.md) §4 verifies it.
- **UARTs**: 1 controller, **4 ports**, stride `0x4000`. Console on port 0.
- **`WHEEL` block**: registers and init sequence documented (and `mikey-6g.c`
  has nothing to do with it — it is the headphone remote over I²C).
- **SPI NOR**: on SPI0 (`0x3C300000`), 1 MiB, SysCfg / NOR boot IM3 /
  "flsh" files layout documented. Dumped from the real device:
  `assets/classic6g/bootflash_nor_1MB.bin`, and its `HwVr` entry at offset
  `0x5c` is what confirms the hardware id `19` that the firmware demands.

And what is **still** unknown, after having booted the entire firmware:

- **What GPIO pin 86 is on the board.** What it does in Apple's
  firmware is verified; what is on the other side of the pin is not.
- **The `WHEELCON` bits one by one**, the `0x3A980` of `WHEELDIV`, bits 1
  and 2 of `WHEELPEND`/`WHEELMASK`, and why the real hardware accepts **both**
  `WHEELTX` encodings (Rockbox's and Apple's). None of them is needed.
- **The `0x3830007C` and `0x38300088` registers of the LCD block**, which the
  retailOS writes before touching anything Rockbox names.
- **Whether `LCD +0x70` latches a frame or only enables scanning.**
- **The meaning of bits 2 and 8 of `LCD_STATUS`**: no driver looks at them.
- **The rest of the `0x38900000` map**: what is documented are the
  twenty-seven places in the image that load that literal plus what
  `DISPTRACE=1` saw; the block may have more.
