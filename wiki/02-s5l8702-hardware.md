# S5L8702 / iPod classic 6G — hardware

> **Actualizado el 10 de agosto de 2026.** Este documento se escribió cuando no
> se había modelado nada. Ahora la máquina `-M iPod-Classic` arranca el retailOS
> de Apple hasta una pantalla navegable, y hay **catorce bloques de periférico
> con comportamiento de verdad** (más el PMU detrás del I²C): la sección
> [«Qué está modelado»](#qué-está-modelado-en--m-ipod-classic) los lista, con lo
> que se sabe de cada uno y lo que no. Dos cosas que este mapa **no podía
> traer** y son las que dibujan y dan entrada:
>
> - **`0x38900000`, el compositor de vídeo.** No lo nombra ninguna fuente
>   externa —ni Rockbox ni freemyipod—, sólo el código de Apple. Es de donde
>   salen los píxeles. Ver [11](11-classic6g-retailos.md) §13.
> - **`0x3C100000`**, un bloque que Rockbox sólo define para el S5L8700 y que en
>   este chip se comporta como un generador de números. Ver [11](11-classic6g-retailos.md) §2.

Fuente primaria: `firmware/export/s5l87xx.h` de Rockbox, resuelto con el
preprocesador para `CONFIG_CPU == S5L8702`:

```bash
cpp -dM -P -nostdinc -D__ASSEMBLER__ \
    -DS5L8700=8700 -DS5L8701=8701 -DS5L8702=8702 -DS5L8720=8720 \
    -DCONFIG_CPU=8702 -DMEMORYSIZE=64 s5l87xx.h | sort > s5l8702_macros.h
```

Resultado en `assets/ref/s5l8702_macros.h` (1564 macros).
Todo lo de este documento sale de ahí salvo donde se indique otra cosa.

## Placa

| Componente | Pieza | Fuente |
|---|---|---|
| SoC | Samsung S5L8702, marcado Apple `337S3473 8702` | freemyipod |
| CPU | ARM926EJ-S, 216 MHz (`CPU_FREQ 216000000`) | freemyipod + `config/ipod6g.h` |
| SDRAM | K4X51163PE | freemyipod |
| Flash de utilidad | SST25VF080B (NOR por SPI) | freemyipod |
| Códec de audio | Cirrus Logic **CS42L55** (I²C) | freemyipod + `cscodec-6g.c` |
| PMU | NXP **PCF50635** (I²C) | freemyipod + `pmu-6g.c` |
| Carga USB | LTC4066 | freemyipod |
| Click wheel | controlador **dedicado dentro del SoC** en `0x3C200000`, IRQ 23 | `ipod/button-clickwheel.c` |
| Mic del jack + mando de auriculares | **"Mikey"**, I²C0 dir. `0x72` (no es el click wheel) | `ipod6g/mikey-target.h` |
| Pantalla | 320×240, RGB565 (`LCD_DEPTH 16`), 160 dpi | `config/ipod6g.h` |
| Almacenamiento | **HDD ATA + CE-ATA sobre SDCI** | `ipod6g/storage_ata-6g.c` |

`MODEL_NUMBER 71`, `MODEL_NAME "Apple iPod Classic/6G"`,
`CONFIG_LCD LCD_IPOD6GNANO3G4G`, `HAVE_ATA_DMA`, `ATA_MAX_UDMA 4`.

## Mapa de memoria

### RAM / ROM

| Región | Base | Tamaño |
|---|---|---|
| SDRAM (`DRAM_ORIG`) | `0x08000000` | 64 MB |
| IRAM0 (`IRAM_ORIG`) | `0x22000000` | `0x20000` (128 KB) |
| IRAM1 | `0x22020000` | `0x20000` (128 KB) |
| Bootrom | `0x20000000`, con alias en `0x0` ([08](08-s5l8702-fuentes-externas.md)) | 50 o 64 KiB, sin reconciliar |
| Últimos 256 B de IRAM1 | `0x2203FF00` | bloque de parámetros que la bootrom deja al firmware |
| TTB (tablas de páginas, lo pone Rockbox) | `DRAM_ORIG + DRAM_SIZE - 0x4000` | `0x4000` |

> **La bootrom no se puede volcar por USB, y está comprobado.** Tras el
> arranque, `0x20000000` devuelve una constante por banco de 16 KiB: está
> bloqueada. Ver [10-classic6g-emcore.md](10-classic6g-emcore.md) §4. Por eso la
> máquina se la salta y reproduce a mano lo que ese bloque de parámetros
> contenía — y por eso `IICCON = 0x184` es una inferencia y no un dato.
>
> El área de IRAM del retailOS **no es libre**: la imagen tiene carga dispersa,
> `fichero [0, 0x0AE48) -> 0x22000000` y el resto a `0x08000000`. Ver
> [11](11-classic6g-retailos.md) §1.

> El S5L8720 del Touch 2G tiene IRAM1 de solo `0x10000`; el S5L8702 tiene
> `0x20000`. Total 256 KB de IRAM.

### Periféricos (`IO_BASE 0x38000000`)

| Base | Macro Rockbox | Bloque | ¿Igual en S5L8720? |
|---|---|---|---|
| `0x38000000` | `SHA1_BASE` | Motor SHA-1 | ✅ sí |
| `0x38100000` | `MIU_BASE` | Controlador de memoria | — |
| `0x38200000` | `DMA0_BASE` | DMAC0 (PL080) | ✅ sí |
| `0x38300000` | `LCD_BASE` | Controlador LCD | ❌ (2G: `0x38900000`) |
| `0x38400000` | `DMA_BASE` / `OTGBASE` | USB OTG (Synopsys DWC) | ✅ sí |
| `0x38500000` | `SM1_BASE` | Divisor de reloj SM1 | — |
| `0x38600000` | `USB_HC_BASE` | USB host controller | — |
| `0x38700000` | `ATA_BASE` | **Controlador ATA** | ❌ (2G: DMAC1) |
| `0x38800000` | `USB_FC_BASE` | USB function controller | — |
| `0x38900000` | **ninguna** | **Compositor de vídeo.** No está en `s5l87xx.h` ni en el modelo de freemyipod: el nombre y el mapa de registros salen del código de Apple | ❌ (2G: LCD) |
| `0x38a00000` | `ATA_UNKNOWN_BASE` | Bloque auxiliar ATA / FMI | — |
| `0x38b00000` | `SDCI_BASE` | SD/MMC — usado para **CE-ATA** | ❌ (2G SDIO: `0x38D00000`) |
| `0x38c00000` | `AES_BASE` | Motor AES | ✅ sí |
| `0x38E00000` | `VIC_BASE` | VIC0 (PL192); VIC1 en `+0x1000` | ✅ sí |
| `0x39000000` | `ADM_SFR_BASE` | CalmADM2E (DSP de audio) | — |
| `0x39200000` | `LCDBASE` | Ventanas/framebuffer del LCD | — |
| `0x39400000` | `FMC_BASE` | Flash Memory Controller (NAND) | — |
| `0x39900000` | `DMA1_BASE` | DMAC1 (PL080) | ✅ sí |
| `0x39a00000` | `EIC_BASE` / `SYSALV_BASE` | Controlador de int. externas | — |
| `0x39C00000` | `INT_BASE` | Controlador de interrupciones legacy | — |
| `0x39E00000` | `ECC_BASE` | ECC de NAND | — |
| `0x3C100000` | (`LCD_BASE` **sólo en S5L8700**) | Bloque sin nombre; el retailOS lo usa como **generador de números**, con tres registros y sin bit de habilitación | — |
| `0x3C200000` | `WHEEL_BASE` | **Interfaz del click wheel** | ❌ solo classic/nano |
| `0x3C400000` | `USB_PHY_BASE` | PHY USB | ✅ sí |
| `0x3C500000` | `CLK_BASE` | Reloj / PLL | ✅ sí |
| `0x3C600000` | `IIC_BASE` / `MS_BASE` | I²C 0 | ✅ sí |
| `0x3C700000` | `TIMER_BASE` | Timers | ✅ sí |
| `0x3C800000` | `WDT_BASE` | Watchdog | — |
| `0x3CA00000` | `I2S_BASE` | I²S (audio) | — |
| `0x3CB00000` | `SPD_BASE` | S/PDIF | — |
| `0x3CC00000` | `UARTC_BASE_ADDR` | UART0 (consola serie) | ✅ sí |
| `0x3CD00000` | `SPI_BASE` | SPI0 | ❌ (2G: `0x3C300000`) |
| `0x3CE00000` | `ADC_BASE` | ADC | ❌ (2G: SPI1) |
| `0x3CF00000` | `GPIO_BASE` | GPIO | ✅ sí |
| `0x3D100000` | `CHIPID_BASE` | Chip ID | ✅ sí |
| `0x3D200000` | `RTC_BASE` | RTC (y SPI2 según `SPIBASE(2)`) | — |
| `0x3D700000` | `MIU_BASE` (variante) | — | — |
| `0x3F000000` | `ADM_BASE` | CalmADM2E, área de datos | — |

Los tres SPI se seleccionan con `SPIBASE(i)`: `i==2 → 0x3d200000`, y los otros
dos siguen el patrón de `SPI_BASE`.

### UARTs

`UARTC_BASE_ADDR 0x3CC00000`. En S5L8701 el stride es de `0x8000`
(`UARTC1 0x3CC08000`, `UARTC2 0x3CC0C000`); para S5L8702 solo se define el
canal 0 en el header — **pendiente de confirmar** si hay más.

## Números de IRQ del VIC

Extraídos de `s5l8702_macros.h`. Van al par de PL192 en `0x38E00000` /
`0x38E01000` (los IRQ ≥ 32 son del VIC1).

| IRQ | Fuente |
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

## Qué está modelado en `-M iPod-Classic`

Estado del árbol a 10 de agosto de 2026. «Modelado» aquí significa *tiene
comportamiento*, no *devuelve ceros sin quejarse*. Todo lo que no aparece en
esta tabla es `create_unimplemented_device()` (24 bloques) o cae al **suelo de
E/S** que cubre los 768 MB de la ventana de periféricos — que hace falta porque
el retailOS mapea toda esa ventana como una sección identidad sin caché, así que
una dirección sin mapear no es un cero inofensivo, es un *data abort*.

| Bloque | Base | Fichero | Qué sabe hacer, y qué no |
|---|---|---|---|
| CPU + RAM | — | `ipod_classic.c` | ARM926EJ-S, SDRAM 64 MB, IRAM0/IRAM1, y el **alias de IRAM0 en `0x0`**, de donde el ARM926 toma sus excepciones |
| VIC0 / VIC1 | `0x38E00000` / `+0x1000` | `hw/intc/pl192.c` (de serie) | 64 líneas. El despachador del retailOS vive en IRAM (`0x22004DA0`), lee los dos bancos **sin condición cruzada** y sirve una interrupción por entrada |
| Timers | `0x3C700000` | `ipod_classic_timer.c` | 8 canales; A–D de 16 bits en IRQ 8, E–H de 32 bits en IRQ 7. **Sólo modo intervalo**; PCLK se trata como ECLK y se avisa por `-d unimp` si alguien lo selecciona |
| Reloj / PLL | `0x3C500000` | `ipod_classic_clk.c` | `SWRCON` reinicia de verdad, `RSTSR`, `PLLMOD2`, y `PLLLOCK` reporta bloqueados los PLL que `PLLMODE` habilitó |
| I²C0 + PCF50635 | `0x3C600000` | `ipod_classic_i2c.c`, `ipod_classic_pmu.c` | Transacción completa con **estado y control en registros distintos** (`IICCON[13:8]` / `IICSTA2[13:8]`), byte de **25 µs virtuales**, fichero de registros del PMU, ADC, e INT1..6 de lectura-limpia. **Sin alarma, sin carga, sin reguladores** |
| ATA + DMA | `0x38700000` | `ipod_classic_ata.c` | El puente de Apple, no un *task file* directo: leer un registro arranca un ciclo cuyo resultado aparece en `ATA_PIO_RDATA`. Lee de verdad. Sólo el bit 0 de `ATA_IRQSTAT`; los demás son `INDETERMINADO` |
| **Compositor** | `0x38900000` | `ipod_classic_disp.c` | **Lo que dibuja.** Cinco descriptores de capa en `+0x058 + k*0x18`. Guarda y **no honra** las tablas de gamma, la matriz de color, el escalador y el bit de rotación: en todos los arranques medidos son la identidad |
| Panel LCD | `0x38300000` | `ipod_classic_lcd.c` | 320×240 RGB565, ventana `0x2A`/`0x2B`, escritura tras `0x2C`, `qemu_console`, y los registros `+0x70..+0x8C` del motor de trama. Compone en `gfx_update`, que es **decisión de modelado** y está dicho en el código |
| DMAC0 / DMAC1 | `0x38200000` / `0x39900000` | `hw/dma/pl080.c` (de serie) | IRQ 16 y 17, **memoria del sistema como puerto maestro** porque el huésped programa los canales con el alias cacheado mientras la CPU corre sobre el no cacheado. Ventana de 4 KB, para que el resto siga cayendo al suelo de E/S |
| Click wheel | `0x3C200000` | `ipod_classic_wheel.c` | IRQ 23, los dos formatos de paquete, cinco botones y la rueda por teclado, con el ritmo que exige el decodificador del firmware. Ventana de 4 KB |
| GPIO | `0x3CF00000` | `ipod_classic_gpio.c` | **Un pin modelado**, el 86, que la tarea de almacenamiento traduce a «hay disco». Los demás leen cero |
| EIC / SYSALV | `0x39A00000` | `ipod_classic_eic.c` | `ALVCON` as a plain register (standby writes it and waits to read it back), and the seven `INTLEVEL`/`INTSTAT`/`INTEN`/`INTTYPE` groups driving VIC 0–3 and 31–33. **One pin has a source**: GPIO 4, which the wheel pulls low on a key press and which wakes the firmware from standby. `0x7b` (the PMU interrupt) has no source because the PMU model has no interrupt output. Pins without a source never set `INTSTAT` |
| Generador | `0x3C100000` | `ipod_classic.c` | LCG sembrado por el huésped. Identidad `INFERIDA`, contrato de tres registros verificado, y **sin bit de habilitación** — condicionar el «dato listo» cuelga el arranque |
| UART0 | `0x3CC00000` | `hw/char/exynos4210_uart.c` (de serie) | IRQ 24 |
| Bloque de parámetros del bootrom | `0x2203FF00` | `ipod_classic.c` | Los **tres** campos que el arranque lee: id de hardware (`19`, o el firmware se reinicia a `"softupdt"`), base y tamaño de DRAM. Los otros ocho quedan a cero a propósito |
| Semihosting de ARM | — | `ipod_classic.c` | Encendido por defecto. **Es como el retailOS imprime sus panics** |

**Lo que no está**: audio (I²S y CS42L55), USB (OTG, host, function y PHY), NAND
(FMC/ECC), CE-ATA sobre SDCI, el DSP CalmADM2E, el RTC, el ADC, el SPI y su NOR,
el motor AES y el SHA-1 del propio SoC, y la bootrom.

## Diferencias respecto al S5L8720 (iPod Touch 2G)

Lo que **se mantiene** entre las dos generaciones y por tanto se puede portar
casi tal cual: VIC0/VIC1 (`0x38E00000`), SHA-1 (`0x38000000`), AES
(`0x38c00000`), USB OTG (`0x38400000`), USB PHY (`0x3C400000`), I²C0
(`0x3C600000`), reloj (`0x3C500000`), timers (`0x3C700000`), GPIO
(`0x3CF00000`), chip ID (`0x3D100000`), UART0 (`0x3CC00000`), DMAC0
(`0x38200000`), DMAC1 (`0x39900000`).

Lo que **cambia y hay que rehacer**:

- **CPU**: ARM926EJ-S en vez de ARM1176. Cambia `default_cpu_type` y la
  semántica de los registros cp15 que el modelo del Touch parchea.
- **LCD**: base distinta (`0x38300000` vs `0x38900000`), sin MIPI DSI, panel
  paralelo de 320×240.
- **Almacenamiento**: el Touch usa NAND (FMSS/FMI); el classic usa **disco duro
  ATA** en `0x38700000` más **CE-ATA sobre SDCI** en `0x38b00000`. Nada de esto
  existe en qemu-ios.
- **Entrada**: click wheel en `0x3C200000` (IRQ 23) en vez de multitouch.
- **SPI**: bases distintas.
- **No existen** en el S5L8702: PKE, MIPI DSI, scaler/CSC, TV-out, MBX (GPU).
- **Existe solo aquí**: CalmADM2E (`0x39000000` / `0x3F000000`), I²S
  (`0x3CA00000`), S/PDIF (`0x3CB00000`), ADC (`0x3CE00000`), RTC (`0x3D200000`).

## Pendiente de confirmar

> ⚠️ **Este documento tiene errores corregidos en
> [08-s5l8702-fuentes-externas.md](08-s5l8702-fuentes-externas.md)**, que además
> resuelve casi todo lo de esta sección. Léelo antes de usar la tabla de bases:
> varias entradas de arriba (`SPI0 0x3CD00000`, `ADC 0x3CE00000`, `RTC
> 0x3D200000`, `FMC 0x39400000`, `USB_HC/USB_FC`, `INT_BASE`, `MS_BASE`) son
> bases **legacy del S5L8700/8701** que no aplican al S5L8702.

Resuelto en el doc 08:

- **Bootrom**: `0x20000000`, con alias en `0x0`. Tamaño 50 KiB (wiki) o 64 KiB
  (modelo de freemyipod), sin reconciliar. El remap posterior a IRAM0 existe pero
  no se conoce el registro que lo controla.
- **Volcado público de la bootrom**: no confirmado; hay que volcarla del hierro.
  **Y no se puede por USB**: [10](10-classic6g-emcore.md) §4 lo comprueba.
- **UARTs**: 1 controlador, **4 puertos**, stride `0x4000`. Consola en el puerto 0.
- **Bloque `WHEEL`**: registros y secuencia de init documentados (y `mikey-6g.c`
  no tiene nada que ver — es el mando de los auriculares por I²C).
- **NOR SPI**: en SPI0 (`0x3C300000`), 1 MiB, layout SysCfg / NOR boot IM3 /
  ficheros "flsh" documentado. Volcada del aparato real:
  `assets/classic6g/bootflash_nor_1MB.bin`, y su entrada `HwVr` en el offset
  `0x5c` es lo que confirma el id de hardware `19` que el firmware exige.

Y lo que **sigue** sin saberse, después de haber arrancado el firmware entero:

- **Qué es el pin 86 del GPIO en la placa.** Lo que hace en el firmware de
  Apple está verificado; lo que hay al otro lado del pin, no.
- **Los bits de `WHEELCON` uno a uno**, el `0x3A980` de `WHEELDIV`, los bits 1
  y 2 de `WHEELPEND`/`WHEELMASK`, y por qué el hardware real acepta **las dos**
  codificaciones de `WHEELTX` (la de Rockbox y la de Apple). Ninguno hace falta.
- **Los registros `0x3830007C` y `0x38300088` del bloque de LCD**, que el
  retailOS escribe antes de tocar nada que Rockbox nombre.
- **Si `LCD +0x70` engancha un cuadro o sólo habilita el barrido.**
- **El significado de los bits 2 y 8 de `LCD_STATUS`**: ningún driver los mira.
- **El resto del mapa de `0x38900000`**: lo que hay documentado son los
  veintisiete sitios de la imagen que cargan ese literal más lo que
  `DISPTRACE=1` vio; el bloque puede tener más.
