# What is reused from qemu-ios

> **Planning document, historical (August 10, 2026).** It was written
> before anything was modeled. The `-M iPod-Classic` machine now boots Apple's
> retailOS, and it is worth stating **what was actually reused**, because it
> does not resemble what is below:
>
> - **Reused as-is, without touching a line**: the two VICs (`hw/intc/pl192.c`),
>   the two DMACs (`hw/dma/pl080.c`) and the UART (`hw/char/exynos4210_uart.c`).
>   Three hits from this table, and the PL080 one turned out to be **the piece
>   that unblocked booting with a disk**.
> - **Reuse was attempted and went wrong**: `ipod_touch_clock.c`. It matches the
>   S5L8702 up to `0x4c` **by coincidence**, and answered `SWRCON`, `RSTSR` and
>   `PLLMOD2` with `hw_error()`, which is not a log but an `abort()` of the process.
>   It was rewritten (`e7f8bfaf3`).
> - **Everything else was written from scratch**: I²C and PMU, ATA with DMA, LCD panel,
>   GPIO, timers, click wheel and the video compositor. Not by choice, but
>   because the S5L8702 has different ones and because **three of those blocks
>   are not in any external source** — the compositor at `0x38900000`, the
>   generator at `0x3C100000` and the LCD frame engine registers only
>   exist in Apple's code.
>
> The lesson, which is the one of the status bit that never arrives: **a borrowed
> model that decodes *almost* the same map is worse than having no model**,
> because it fails at the addresses the new map added and does so in the
> noisiest possible way.

> **Warning: a QEMU model of the S5L8702 already exists.**
>
> `github.com/freemyipod/qemu`, branch **`s5l8702-dev`**, contains a complete
> (WIP) model of the S5L8702 SoC and an `ipod-classic` machine — 64 MiB, arm926,
> NOR over SPI0, IDE disk, PCF5063x on I2C0. Latest commits from May 2026.
> Files: `hw/arm/s5l8702.c`, `hw/arm/ipod-classic.c`,
> `include/hw/arm/s5l8702.h`, plus ~19 device models under
> `hw/{misc,gpio,i2c,ide,ssi,timer,watchdog}/s5l8702-*.c`.
>
> That fork descends from the same `devos50/qemu-ios` we started from here. **If
> the Classic 6G work is picked up again, start from there instead of from scratch**,
> and use `devos50/qemu-ios@ipod_touch_2g` only as a cross-check for base addresses.
>
> We found out late: this document was written before we knew.



The `ipod_touch_2g` branch models the S5L8720. The S5L8702 is from the same family, so
a good share of the device models are usable.

**Key fact that makes it easy:** no qemu-ios device model hardcodes its base
address. They all do `memory_region_init_io(...)` at offset 0
and it is the machine that places them with `memory_region_add_subregion(sysmem,
XXX_MEM_BASE, ...)`. Relocating the map means editing the `*_MEM_BASE` table in
`include/hw/arm/ipod_classic.h`.

## Reusable almost as-is

| Model | File | Notes for the S5L8702 |
|---|---|---|
| VIC PL192 ×2 | `hw/intc/pl192.c` (stock) | Same base `0x38E00000`/`0x38E01000`. Zero changes. |
| DMAC PL080 ×2 | `hw/dma/pl080.c` (stock) | Bases `0x38200000` and `0x39900000`. |
| UART | `hw/char/exynos4210_uart.c` (stock) | Already used for 1G and 2G. Base `0x3CC00000`. |
| Clock / PLL | `ipod_touch_clock.c` | Pure register file, no logic. Base `0x3C500000`. |
| GPIO | `ipod_touch_gpio.c` | The button pins need remapping. |
| SYSIC | `ipod_touch_sysic.c` | Power gating + GPIO interrupt controller. |
| I²C | `hw/i2c/ipod_touch_i2c.c` | Classic Samsung. Base `0x3C600000`, identical. |
| SPI | `ipod_touch_spi.c` | Register layout common to the whole family. **Careful:** it selects the slave peripheral with a static global `base_addr` via `set_spi_base(N)`; it should be turned into a qdev property. |
| Chip ID | `ipod_touch_chipid.c` | Change `0x8720` to `0x8702` at offset `0x8`. |
| SHA-1 | `ipod_touch_sha1.c` | Backed by OpenSSL. Same base `0x38000000`. |
| USB PHY | `ipod_touch_usb_phys.c` | Same base `0x3C400000`. |
| USB OTG | `ipod_touch_usb_otg.c` | Synopsys DWC device-mode model. Only the four S5L8702 `GHWCFG` values need to be supplied. |
| NOR SPI | `ipod_touch_nor_spi.c` | The classic also has NOR over SPI (SST25VF080B). |
| PMU | `ipod_touch_pcf50633_pmu.c` | The classic uses **PCF50635** — same family, check registers against `ipod6g/pmu-6g.c`. |
| Audio codec | `ipod_touch_cs42l58.c` | The classic uses **CS42L55** — check against `ipod6g/cscodec-6g.c`. |
| "Mikey" | `ipod_touch_cd3272_mikey.c` | The classic has the same accessory/wheel IC. See `ipod6g/mikey-6g.c`. |

## The decode needs rewriting

| Model | Why |
|---|---|
| Timer (`ipod_touch_timer.c`) | The 2G model is very crude: it only emulates "Timer 4" at offset `0xA0` plus the 64-bit counter. The S5L8702 has 4 16-bit timers + 4 32-bit ones with a different layout. The skeleton is reusable. |
| LCD (`ipod_touch_lcd.c`) | Different base and different controller (parallel panel, not MIPI). The 10 Hz scanout skeleton is usable, though. |
| AES (`ipod_touch_aes.c`) | The 2G model **does not emulate the GID key**: it hands out five precomputed keys/IVs in call order and also patches specific iBoot physical addresses. All of that is specific to the Touch 2G firmware and has to be removed. |

## Has to be written from scratch

| Block | Base | Comment |
|---|---|---|
| **ATA** | `0x38700000` (IRQ 29) | The biggest gap. QEMU has `hw/ide/` as a base, but the S5L8702 controller has to be modeled by looking at `ipod6g/storage_ata-6g.c`. |
| **CE-ATA / SDCI** | `0x38b00000` (IRQ 44) | Alternative storage path. `ipod6g/mmcdefs-target.h` documents the commands. |
| **Click wheel** | `0x3C200000` (IRQ 23) | There is nothing equivalent in qemu-ios (the Touch uses multitouch over SPI). |
| **I²S** | `0x3CA00000` | Audio. |
| **ADC** | `0x3CE00000` | |
| **RTC** | `0x3D200000` | |

## Not applicable to the S5L8702

PKE (RSA), MIPI DSI, scaler/CSC, TV-out, MBX (GPU), SDIO/BCM4325 (WiFi), FMSS.
The NAND model that would be usable, if needed, is the one from the
`ipod_touch_1g` branch (`ipod_touch_nand.c`, FMI registers: `FMCTRL0`, `FMCTRL1`,
`CMD`, `FMADDR0/1`, `FMANUM`, `FMDNUM`, `FMCSTAT`, `FMFIFO`, `RSCTRL`), which
matches the S5L8702's `FMC_BASE`.

## How the Touch 2G machine boots (to copy the pattern)

- `mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1176")` → for us: `arm926`.
- It does not use `machine->ram`; all regions are created with
  `memory_region_init_ram()` from an `allocate_ram()` helper.
- The bootrom is loaded at physical `0x0` (128 KB region) from the
  `bootrom=` machine property; if the file does not exist, `exit(1)`.
- `ipod_touch_cpu_reset()` does `cpu_set_pc(cpu, VROM_MEM_BASE)` → it boots at `0x0`.
- **There is no `-kernel` path** nor `arm_load_kernel()`. For our first milestone we
  want to add exactly that: load a binary at `0x08000000` and jump
  there, skipping the bootrom.
