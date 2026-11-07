# HLE del DSP de audio del retailOS: especificación e inventario de incógnitas

Documento de entrega de **dos investigaciones de ingeniería inversa** (una de
desensamblado estático, otra de traza en ejecución) que perseguían el mismo
objetivo: **especificar exactamente qué tiene que hacer nuestro modelo del DSP
para que el retailOS de Apple reproduzca audio en el emulador**, de forma que el
siguiente workflow pueda implementar el HLE sin volver a investigar.

Es la continuación directa de [17-classic6g-dsp-retailos.md](17-classic6g-dsp-retailos.md)
(el plan de partida) y de [14-classic6g-audio.md](14-classic6g-audio.md) (el
camino de audio aguas abajo, ya modelado). **Corrige** la premisa de [17](17-classic6g-dsp-retailos.md)
con medida.

Direcciones VA del firmware = offset en `osos_decrypted.bin` + `0x07FF51B8`.
Marcas usadas en todo el documento:

- **VERIFICADO** — medido en ejecución o leído del desensamblado.
- **INFERIDO** — razonado desde algo verificado; se indica la fuerza.
- **DESCONOCIDO** — no se sabe, y no se finge. Se propone cómo averiguarlo.

---

## 0. Veredicto que cambia el marco (leer esto primero)

**La premisa de [17](17-classic6g-dsp-retailos.md) — "el retailOS referencia el
DSP CalmADM2E MUCHÍSIMO (245 y 213 literales) y decodifica el MP3 ahí" — NO se
sostiene.** Las dos investigaciones, por caminos independientes (desensamblado y
traza), llegaron a lo mismo:

1. **El retailOS no toca `0x39000000` (ADM_SFR) ni `0x3F000000` (ADM) en ningún
   estado alcanzable.** VERIFICADO por dos medidas separadas: 0 accesos MMIO en
   arranque en reposo, navegación completa y decenas de intentos de PLAY.
2. **Ese "recuento de referencias" era un artefacto.** El literal dominante
   `0x3F000000` es el **float `0.5f`** IEEE-754 (matemática de ganancia), y
   `0x39000000` aparece como la constante FourCC `0x39530000` y como datos
   (píxeles ARGB, tablas de bytes). **Ni una sola carga de puntero** a ninguna de
   las dos bases, ni un solo acceso `base+offset` al mapa de registros del
   CalmADM2E. VERIFICADO por desensamblado.
3. **El decodificador de audio del retailOS es el framework de Apple "MeCCA" con
   códecs cargables (`AudioCodecs`), decodificando en software** (cadenas
   `MeCCADecode`, `MeCCA_MediaPlayer`, `MeCCA_StreamCache`, `AudioCodecs`,
   `alac: bit depth = %d`, `MP3ExampleTask`, `ERROR: unknownCodec loaded !!!`).
   INFERIDO (fuerte).
4. **El verdadero muro no es "modelar el DSP", es que la reproducción es
   inalcanzable:** los botones no registran sobre la **lista de canciones**
   (`wiki/14 §5`), así que el camino de `play` **aborta antes** de tocar ningún
   periférico de decodificación. Sin `play` sostenido no hay protocolo que
   trazar. VERIFICADO.

**Consecuencia para el HLE:** que el mecanismo de audio del retailOS sea el
CalmADM2E vía las ventanas MMIO de Rockbox está **NO VERIFICADO — y la evidencia
disponible lo contradice.** Construir un HLE ahí a ciegas sería inútil. Este
documento entrega (a) el mapa de registros *tal como quedaría si la hipótesis
CalmADM2E se confirmara* (de Rockbox, sin uso comprobado), (b) lo que de verdad
está verificado, y (c) un **plan de implementación en árbol de decisión** cuyo
primer nodo es una medida barata que decide dónde va el HLE.

---

## 1. Reconciliación de las dos investigaciones

Las dos coinciden en lo esencial (§0). Difieren en un punto que hay que dejar
anotado con honestidad:

| Punto | Investigación de código (desensamblado) | Investigación de traza (runtime) |
|---|---|---|
| ¿Se alcanzó "Now Playing"? | **Sí** (por métrica de píxeles: `diff=28160` sobre lista `diff=38030`), y al pulsar PLAY capturó **18 accesos a `0x39300000`** (PCs `0x080a23e4`, `0x0814051c–58`), con un **bucle de polling** en pc~`0x08140540` | **No**: el SELECT/PLAY **no registra** sobre la lista; 30 rondas sin cambio de pantalla, 0 accesos a ningún bloque no modelado |
| ¿Accesos a `0x39/0x3F` (ADM)? | **0** | **0** |
| ¿Audio? | `play.wav` = **0 muestras no nulas** | silencio (no llegó a play) |

**Reconciliación (INFERIDO):** una investigación observó actividad en
`0x39300000` durante un intento de PLAY; la otra ni siquiera enganchó PLAY. Lo
compatible con ambas es que **los accesos a `0x39300000` pertenecen al compositor
de la pantalla Now Playing (escalado de la carátula), no al audio** — coinciden
con el escalador 2D `MeCCA` (ver §2b). Es decir: aunque una corrida llegara a
dibujar Now Playing, el **audio siguió siendo silencio y el ADM siguió sin
tocarse**. Ninguna de las dos observó un periférico de *decodificación de audio*.
El bloque `0x39300000` es, con evidencia fuerte, **vídeo/imagen**, no el DSP de
audio — es una **pista falsa**.

---

## 2. Pregunta 1 — El MAPA de registros

Hay que separar dos cosas que [17](17-classic6g-dsp-retailos.md) mezclaba: el
mapa *candidato* (CalmADM2E, de Rockbox, **sin uso comprobado en el retailOS**) y
el mapa *realmente conducido* (`0x391/2/3`, vídeo).

### 2a. Mapa candidato: CalmADM2E (de Rockbox `s5l87xx.h:64-137) — NO VERIFICADO EN USO

Reproducido para que el implementador lo tenga a mano **si** el nodo de decisión
del §6 confirma que el audio pasa por aquí. **Advertencia: no se ha medido ni un
solo acceso del retailOS a estos registros.** Dos vistas del mismo bloque.

**El buzón (mailbox) host ↔ DSP:**

| Registro | `ADM_BASE=0x3F000000 +` | `ADM_SFR_BASE=0x39000000 +` | Rol (según Rockbox) |
|---|---|---|---|
| `CONFIG0` / `CONFIG1` | `0x00` / `0x02` (16b) | `0x00` (`ADM_CONFIG`, 32b) | configuración/control |
| `COMMUN` | `0x04` (16b) | `0x04` (32b) | **control de comunicación** (el "timbre": el ARM da la orden aquí) |
| `DDATA0..7` | `0x06..0x14` (16b) | `0x10..0x2C` (32b) | **datos host → DSP** (8 palabras: argumentos del comando) |
| `UDATA0..7` | `0x16..0x24` (16b) | `0x30..0x4C` (32b) | **datos DSP → host** (8 palabras: respuesta) |

**Los punteros a buffers en DRAM** (el host le dice al DSP dónde están sus
regiones; el HLE los leería para saber de dónde leer el comprimido y dónde
escribir el PCM):

| Registro | `ADM_BASE +` (H/L, 16b c/u) | `ADM_SFR_BASE +` (32b) | Rol |
|---|---|---|---|
| `IBASE` | `0x26`/`0x28` | `0x50` | inicio del **firmware del DSP** (código CalmRISC). El HLE lo **ignora** — no ejecuta el Calm |
| `DBASE` | `0x2A`/`0x2C` | `0x54` | inicio del **área de datos** del CalmRISC |
| `XBASE` | `0x2E`/`0x30` | `0x58` | área X del MAC |
| `YBASE` | `0x32`/`0x34` | `0x5C` | área Y del MAC |
| `S0BASE` | `0x36`/`0x38` | `0x60` | inicio del **buffer secuencial 0** (INFERIDO: entrada o salida) |
| `S1BASE` | `0x3A`/`0x3C` | `0x64` | inicio del **buffer secuencial 1** (INFERIDO: el otro) |

**Estado/control de los buffers secuenciales (solo en la vista `ADM_BASE`, 16b):**

| Registro | `ADM_BASE +` | Rol |
|---|---|---|
| `CACHECON` / `CACHESTAT` | `0x3E` / `0x40` | control/estado de caché del Calm |
| `SBFCON` | `0x42` | **control** de los buffers secuenciales (arranca/para el modo anillo) |
| `SBFSTAT` | `0x44` | **estado** de los buffers secuenciales (candidato a "bloque listo") |
| `SBL0OFF` H/L | `0x46`/`0x48` | offset actual del anillo 0 (cuánto consumido/producido) |
| `SBL1OFF` H/L | `0x4A`/`0x4C` | offset actual del anillo 1 |
| `SBL0BEGIN` H/L | `0x4E`/`0x50` | inicio del anillo 0 |
| `SBL1BEGIN` H/L | `0x52`/`0x54` | inicio del anillo 1 |
| `SBL0END` H/L | `0x56`/`0x58` | fin del anillo 0 |
| `SBL1END` H/L | `0x5A`/`0x5C` | fin del anillo 1 |

Reloj del DSP: `DSPCLKMD` en `CLK_BASE+0x38` = `0x3C500038`. **VERIFICADO que el
retailOS NO lo carga como literal** (investigación de traza) — otra señal de que
el DSP no se arranca por esta vía.

**Qué es cada tipo, para el HLE (si se confirmara):** `COMMUN` = timbre/comando;
`DDATAn` = argumentos de entrada; `UDATAn` = resultados; `*BASE` = punteros a
regiones DRAM; `SBFCON/SBFSTAT`+`SBLxOFF/BEGIN/END` = los dos anillos entrada/
salida y su avance; `CONFIG0/1` = control global. **Cuál de S0/S1 es entrada y
cuál salida es DESCONOCIDO** (nunca se programaron).

### 2b. Mapa realmente conducido al reproducir: `0x39100000 / 0x39200000 / 0x39300000` — VERIFICADO, es vídeo/imagen (INFERIDO fuerte)

Esto es lo que la traza SÍ capturó. **Driver en `0x08140000–0x08141cc8`** (más
satélites `0x080543e0`, `0x08072810`, `0x080998a4`, `0x080a23dc`, `0x080af960`),
con **33 cargas `ldr rX,=0x391/2/3 0000`** y **0** de `0x39000000`.

Mapa de registros por bloque (de `admscan2.py`, 443 accesos), común a los tres:

| Offset | Rol (INFERIDO del uso) |
|---|---|
| `+0x00` | **control/estado**: bit0 = run/enable, bit1 = idle/done (VERIFICADO por el patrón parar→esperar) |
| `+0x04` / `+0x06` | cabecera de 16 bits, escrita **byte-swapped (big-endian)** |
| `+0x08` | word de cabecera (32b, big-endian) |
| `+0x3c` | modo (nibble bajo) |
| `+0x044..0x0d0`, `+0x080..0x158`, `+0x180..0x1c0`, `+0x200..0x26c` | bancos de config/coeficientes |
| `+0x280` = 1, `+0x284` bit0 | **enable** del bloque |
| `+0x3c0..0x3d4` | reset/clear |
| (bloque `0x39200000` llega hasta `+0x800`) | |

Secuencia verificada (desensamblado): **enable** (`0x080a23dc`:
`str #1,[base+0x280]; [base+0x284] |= 1`) → **cabecera byte-swapped** a
`+0x00/04/06/08` (`0x080a23fc`) → **run** (`+0x00` bit0) → **poll idle**
(`+0x00` bit1, hasta 10 veces con delay `bl 0x80c4e48`) → **teardown**
(`0x081404c4`: `+0x3c &= ~0xF; +0x00 &= ~1`).

**Por qué es vídeo/imagen y no audio (INFERIDO fuerte):** la función de setup
(`0x08140140`) carga `r7=0x39100000` y calcula **dos ratios de escalado
independientes por dimensión** — `r7+0x64 = (dstW<<12)/srcW >> 3` y
`r7+0x68 = (dstH<<12)/srcH >> 4` (división vía `bl 0x800f990`). **Dos** ratios 2D
= escalado de imagen (la carátula), no remuestreo de audio (1D). Concuerda con
las cadenas `MeCCAVideoDecode`, `MeCCA_VideoBufferMgr`, `MPEG4`. **Recomendación:
no construir el HLE de audio aquí.** (Confirmable: reproducir *vídeo* debería
activarlos; *audio sin carátula* no — ver §7.)

---

## 3. Pregunta 2 — La SECUENCIA que el retailOS espera

### Para el audio (CalmADM2E): **DESCONOCIDO.**

- No hay **ninguna** escritura a `IBASE/DBASE/XBASE/YBASE/S0BASE/S1BASE` ni a
  `CONFIG0/COMMUN/DDATAn` medida en ningún estado alcanzable (0 accesos,
  VERIFICADO). Por tanto **no se conoce** ni la carga de firmware, ni el arranque,
  ni el lazo "dame un bloque → aquí tienes PCM", ni valores reales.
- **Causa del vacío (VERIFICADO):** el camino de `play` aborta aguas arriba (muro
  de la lista, §0.4). El instrumento está validado (dispara con un `xp` de
  control), así que el 0 es un **negativo real**, no una sonda rota.

### Lo que SÍ está verificado (bloques de vídeo `0x391/2/3`, para referencia):

enable (`+0x280`/`+0x284`) → cabecera big-endian (`+0x00/04/06/08`) → run
(`+0x00` bit0) → **poll de `+0x00` bit1** hasta done → teardown (`+0x3c`, `+0x00`).
Es **polling**, no interrupción. Útil como plantilla de estilo del firmware
(cabeceras big-endian, control por bit0/bit1), pero **no es el lazo de audio**.

---

## 4. Pregunta 3 — El FORMATO exacto (entrada y salida)

- **Entrada del DSP (qué decodificar, de qué buffer, tamaño): DESCONOCIDO.** No
  se programó ningún `S0BASE/S1BASE`, así que no hay dirección ni tamaño de buffer
  de entrada que volcar. Se ignora incluso si al DSP se le pasarían frames MP3
  crudos o algo pre-parseado por el ARM. INFERIDO (débil) del framework: los
  códecs son cargables (`AudioCodecs`, ALAC/MP3), lo que sugiere entrada = stream
  comprimido del contenedor.
- **Salida (PCM): parcialmente VERIFICADO aguas abajo.** El formato que consume el
  I²S ya está modelado y probado con Rockbox: **PCM 16-bit estéreo entrelazado a
  44100 Hz** (48000 según pista; el retailOS fija `I2SCLKDIV`), leído por el DMA
  `IIS0_TX` → FIFO del I²S en `0x3CA00010` (ver [14](14-classic6g-audio.md)). **A
  qué buffer de DRAM escribiría el PCM el DSP: DESCONOCIDO** (sería el otro anillo
  S0/S1, no programado).
- **Endianness:** los bloques de vídeo `0x391/2/3` esperan cabeceras **big-endian**
  (VERIFICADO por el byte-swap en `0x080a23fc`); si el ADM siguiera el mismo estilo
  de firmware, sus cabeceras también lo serían (INFERIDO débil, no aplicable hasta
  confirmar la vía).
- **Medida de control:** todo el PCM capturable hoy es **silencio** (`play.wav` =
  0 muestras no nulas; el "empuje de 6144 muestras a 0" del arranque es el
  **silencio de init** del I²S, no un buffer de decodificación).

---

## 5. Pregunta 4 — La INTERRUPCIÓN o señal que cierra el ciclo

- **Para el audio (CalmADM2E): DESCONOCIDO.** No se observó ninguna IRQ ni señal
  de "bloque decodificado", porque el lazo nunca arrancó. El candidato natural en
  el mapa candidato sería `SBFSTAT`/`UDATAn` + una línea de IRQ del ADM, pero **no
  hay evidencia** de cuál ni de cómo se arma.
- **Lo observado en los bloques de vídeo es POLLING, no IRQ** (`+0x00` bit1). Es
  posible (INFERIDO débil) que el audio también use polling de un bit de estado en
  vez de interrupción; habría que confirmarlo cuando se alcance `play`.
- **Contexto de IRQ ya modelado aguas abajo:** el cierre del ciclo de *reproducción*
  (pedir el siguiente bloque) hoy lo da el **DMA de audio** (DMAC0, línea
  `IIS0_TX`, `IRQ_DMAC0 = 16`, con pacing `AUDIOPACE`), que **ya funciona** con
  Rockbox. Si el decode fuera en ARM (MeCCA sw), esa es la única IRQ que cierra el
  ciclo y **no hace falta modelar ninguna IRQ de DSP**.

---

## 6. Pregunta 5 — El PLAN de implementación HLE (árbol de decisión)

**No empezar escribiendo un `MemoryRegion` en `0x39000000`.** La evidencia dice
que sería trabajo muerto. El plan tiene un **prerrequisito** y un **nodo de
decisión barato** antes de comprometer arquitectura.

### Paso 0 (PRERREQUISITO, bloqueante) — Romper el muro de la lista de canciones

Sin `play` sostenido no hay protocolo que trazar; las dos investigaciones se
estrellaron aquí. El síntoma (VERIFICADO): la rueda mueve el resaltado en la lista
`Música → Canciones`, pero **SELECT/PLAY no registran** *sobre la lista* (sí
registran en los menús anteriores). Vía de ataque: comparar cómo el firmware
consume los paquetes de botón en un **menú** (funciona) vs. en el **widget de
lista** (no) — misma rueda, distinto consumidor. Es un muro de UI aguas arriba del
audio (ver [14 §5](14-classic6g-audio.md), [15](15-muros-classic6g.md)).

### Paso 1 (NODO DE DECISIÓN, barato) — Trazar quién llena el buffer PCM

En cuanto haya `play`, re-aplicar la instrumentación **DSPTRACE** (ya escrita y
validada; ver §8) **más** un breakpoint/watch sobre la fuente del DMA de audio:
el DMA `IIS0_TX` lee de una dirección DRAM → **poner un watchpoint de escritura en
esa dirección** y ver **quién la escribe**:

- **Rama A — la escribe código ARM** (PC en el rango del framework MeCCA, p.ej.
  cerca de `MeCCADecode`/`AudioCodecs`, o el driver PCM en IRAM `0x22008xxx`):
  entonces **el retailOS decodifica en el ARM** y *no hay DSP que emular*. El audio
  ya fluiría en cuanto el decodificador ARM corra sobre PCM real; el HLE se reduce
  a asegurar que el códec software no dependa de un periférico ausente. **Este es
  el resultado más probable** (INFERIDO fuerte, por ALAC/MP3ExampleTask/MeCCA sw).
- **Rama B — aparecen escrituras del retailOS a `0x39000000`/`0x3F000000`**:
  entonces la hipótesis CalmADM2E se confirma y se implementa el HLE del §6-detalle.
- **Rama C — la llena otro periférico** (ni ARM ni ADM): ahí está el "DSP" real; su
  base saldrá en el `io_floor` con el PC del huésped. Se modela ese, no el ADM.

**Coste:** una sola corrida con `play`. Decide toda la arquitectura. **No escribir
código de modelo antes de este paso.**

### Paso 6-detalle — El HLE del CalmADM2E, SOLO si el Paso 1 cae en la Rama B

Plantilla: `hw/arm/ipod_video/ipod_video_vc.c` (HLE del VideoCore del 5G) y
`hw/arm/ipod_classic/ipod_classic_disp.c` (estilo de dispositivo del Classic).

1. **`MemoryRegion`** de `0x39000000` (SFR, 32b) y `0x3F000000` (ADM, 16b) —dos
   vistas del mismo estado interno— registradas con prioridad **por encima** de
   los `create_unimplemented_device` actuales (`ipod_classic.c:343` y `:358`), como
   ya hace la traza. Handlers `read`/`write` que mantengan un `struct` con
   `CONFIG`, `COMMUN`, `DDATA[8]`, `UDATA[8]`, `IBASE..S1BASE`, `SBFCON/SBFSTAT`,
   `SBLx*`.
2. **Handler de `COMMUN` (write)** = el disparador. Cuando el ARM escriba el valor
   de "decodifica" (a determinar por traza en la Rama B), el modelo:
   a. lee `S?BASE` (el de entrada) + `SBLxOFF/BEGIN/END` para localizar el frame
      comprimido en DRAM (`cpu_physical_memory_read`);
   b. **decodifica en el host** con **libmad** (MP3) o **ffmpeg/libavcodec** (MP3+
      AAC/ALAC), enganchado aquí — es el punto de inserción del decodificador;
   c. escribe el PCM resultante al `S?BASE` de salida
      (`cpu_physical_memory_write`), avanza `SBLxOFF`, marca `SBFSTAT`/`UDATAn`;
   d. **avisa** al ARM: poner el bit de estado que espera y, si la Rama B mostró
      IRQ, `qemu_irq_raise` de la línea del ADM (número a determinar).
3. **Convivencia con I²S/DMA (ya modelado, no tocar):** el HLE solo produce PCM en
   DRAM. Desde ahí, el camino existente lo lleva solo: DMAC0/`IIS0_TX` con
   `AUDIOPACE` → FIFO I²S `0x3CA00000` → CS42L55 → altavoces/WAV. **No se toca**
   `ipod_classic_i2s.c` ni `pl080.c` ni el códec.
4. **Firmware del DSP:** el HLE **ignora** `IBASE` y el código CalmRISC. Esa es la
   ventaja del HLE: no hay que tener ni ejecutar el firmware del Calm.

### Riesgos y dependencias

- **Riesgo mayor (probable):** que la Rama B **no ocurra** y todo el §6-detalle sea
  inaplicable. Por eso el Paso 1 va primero. VERIFICADO que hoy la evidencia
  apunta a Rama A.
- **Dependencia de build:** libmad/ffmpeg en el árbol QEMU (flags de `meson`,
  licencia). Solo relevante en Rama A o B.
- **Riesgo de UI:** el muro de la lista (Paso 0) puede ser no trivial; sin él, nada
  de lo demás es medible.
- **Riesgo de batería:** el reset "Connect to Power" a ~46 s (`wiki/15`) puede
  cortar la captura; entrar en `play` antes o mitigar el modelo de batería.
- **No romper:** la instrumentación debe ser temporal (prioridad por encima,
  lecturas que devuelven 0 igual que el `unimp` actual) para no regresionar Rockbox
  ni `navtest` 6G/5G. Ambas investigaciones dejaron el árbol byte-idéntico al
  baseline; repetir esa disciplina.

---

## 7. Lo que sigue DESCONOCIDO y cómo averiguarlo

| Incógnita | Cómo cerrarla |
|---|---|
| **El mecanismo de audio real (ARM sw vs ADM vs otro periférico)** | Paso 1 del §6: watchpoint sobre la fuente DRAM del DMA `IIS0_TX`, ver el PC que la escribe. Una corrida con `play`. **Es la pregunta madre.** |
| **El muro de la lista de canciones** (bloquea todo) | Paso 0 del §6: comparar el consumo de paquetes de botón menú vs. widget de lista |
| **Si `0x391/2/3` son vídeo o (menos probable) audio** | Reproducir **vídeo** (deberían activarse) vs. **audio sin carátula** (no deberían). Cierra la clasificación |
| **Comando de decode, buffers E/S, IRQ, formato de entrada** (solo si Rama B) | Traza de `COMMUN`/`DDATAn`/`S?BASE` con la instrumentación DSPTRACE ya lista, en cuanto haya `play` |
| **Fuentes externas del protocolo ADM** | Revisar si **freemyipod/qemu** (rama `s5l8702-dev`) modela el ADM, y si **giek** documenta MeCCA/AudioCodecs. (Nota: `giek/architecture_MEMORY_MAP.md:31` etiqueta `0x39000000` como "GPIO" —baja confianza, se contradice con el modelo en funcionamiento— pero refuerza que "estas ventanas = decodificador" **no está probado**.) |

---

## 8. Tabla de confianza consolidada

| Afirmación | Marca | Evidencia |
|---|---|---|
| `0x39000000`/`0x3F000000` nunca se cargan como puntero ni se acceden por MMIO | **VERIFICADO** | `ldrscan.py`/`osana.py lit` = 0 LDR; 0 accesos en boot + navegación + 30 rondas de PLAY (dos investigaciones); control `xp` prueba que el instrumento dispara |
| `0x3F000000` en código = float `0.5f` (matemática de ganancia) | **VERIFICADO** | cada `mov #0x3f000000` → `bl 0x8374b18` (soft-float); vecino `0x3f800000`=1.0f |
| `0x39000000` en código = constante `0x39530000` (FourCC) + datos ARGB | **VERIFICADO** | `0x0810626c`: `orr`+`orr`; barrido de palabras = datos |
| HW realmente conducido al reproducir = `0x39100000/200000/300000` | **VERIFICADO** | `io_floor` en runtime (PCs `0x080a23e4`, `0x0814051c–58`); 33 `ldr`-literales |
| Esos bloques `0x391/2/3` son escaladores de imagen (vídeo/carátula), no audio | **INFERIDO** (fuerte) | dos ratios `(dst<<12)/src` por dimensión; cadenas `MeCCAVideo`/`MPEG4` |
| Secuencia de los bloques de vídeo: enable→cabecera BE→run→poll idle→teardown | **VERIFICADO** | desensamblado `0x080a23dc`, `0x080a23fc`, `0x081404c4` |
| Audio del retailOS = silencio en todo lo alcanzable | **VERIFICADO** | `play.wav` 0 no nulos; sin periférico de audio en el `io_floor` |
| El decode de audio es MeCCA/AudioCodecs en software (probablemente ARM) | **INFERIDO** (fuerte) | cadenas ALAC/`MP3ExampleTask`/`MeCCADecode`/`AudioCodecs`; ausencia de HW de audio conducido |
| La reproducción es inalcanzable por el muro de la lista (SELECT no registra) | **VERIFICADO** | 30 rondas sin cambio de pantalla; reproduce `wiki/14 §5` |
| Mapa de registros del CalmADM2E (§2a) | **VERIFICADO como def. de Rockbox / NO VERIFICADO en uso** | `s5l87xx.h:64-137`; 0 accesos medidos del retailOS |
| Secuencia de decode de audio, buffers E/S, comando, IRQ, formato de entrada | **DESCONOCIDO** | bloqueado por no alcanzar `play` sostenido |
| Salida PCM aguas abajo = 16-bit estéreo entrelazado 44100 Hz por I²S | **VERIFICADO** | modelado y probado con Rockbox ([14](14-classic6g-audio.md)) |

---

## 9. Resumen de la especificación (para el implementador con prisa)

1. **No** modeles `0x39000000`/`0x3F000000` todavía: no hay evidencia de que el
   retailOS los use para audio, y sí evidencia de que no.
2. **Primero** rompe el muro de la lista de canciones (§6 Paso 0) para conseguir
   `play`.
3. **Luego**, una corrida barata (§6 Paso 1): watchpoint sobre la fuente DRAM del
   DMA `IIS0_TX` → mira el PC que escribe el PCM. Eso decide todo:
   - PC en ARM/MeCCA → **decode en ARM, no hay DSP que emular** (más probable);
   - escrituras a `0x39/0x3F` → implementa el HLE del CalmADM2E (§6-detalle, mapa
     §2a);
   - otro periférico → modela ese.
4. Sea cual sea el productor de PCM, **aguas abajo ya está todo hecho** (I²S+DMA+
   CS42L55+captura). El HLE solo tiene que dejar PCM real en DRAM.

---

## 10. Referencias y artefactos

- [17-classic6g-dsp-retailos.md](17-classic6g-dsp-retailos.md) — el plan de
  partida (cuya premisa este documento corrige con medida).
- [14-classic6g-audio.md](14-classic6g-audio.md) — el camino de audio aguas abajo
  (I²S/DMA/CS42L55), ya modelado, y la medida del silencio.
- [15-muros-classic6g.md](15-muros-classic6g.md) — los muros del Classic, incluido
  el de la lista y el reset de batería; y la doctrina de validar el instrumento.
- [16-classic6g-rockbox.md](16-classic6g-rockbox.md) — el camino de audio que **sí**
  suena (Rockbox decodifica en el ARM).
- `assets/rockbox/firmware/export/s5l87xx.h:64-137` — definición del mapa CalmADM2E
  (fuente del §2a; sin uso comprobado en el retailOS).
- `hw/arm/ipod_video/ipod_video_vc.c` — **el precedente de HLE** (VideoCore del
  BCM2722): modelar el protocolo, no la ISA. Patrón a repetir si se confirma la
  Rama B.
- Instrumentación DSPTRACE (temporal, revertida en ambas investigaciones; lista
  para re-aplicar): patrón de `DISPTRACE` en `ipod_classic_disp.c`, mapeada por
  encima de los `create_unimplemented_device` de `ipod_classic.c:343`/`:358`.
- Herramientas de RE usadas: `ldrscan.py`/`osana.py lit` (cargas de literales,
  correctas donde `findlit.py` falla), `admscan.py`/`admscan2.py` (taint por base),
  `dumpctx.py`; desensamblado base VA `--adjust-vma=0x07FF51B8`.

---

## 11. El arranque del audio (continuación): el reproductor SÍ arranca; tres muros de RELOJ caídos; el decode sigue mudo

Con los arreglos del PMU (SELECT fiable + sin reset de batería, [15](15-muros-classic6g.md))
y el tracer no invasivo `pctrace`, se atacó el "Paso 0" del §6. Todo lo de abajo
está **VERIFICADO** por traza de PC (sin parar la CPU) o lectura de memoria en
runtime. VA = offset en `osos_decrypted.bin` + `0x07FF51B8`.

### 11.1 El reproductor SÍ arranca — refuta el encuadre "el handler no se despacha ni falla"

SELECT sobre **Shuffle Songs** (menú principal):
- `HandleShuffleSongsSelected` (0x081fa9e4) **se despacha**.
- Pasa su puerta: singleton `*0x089117fc` → `+0x3f4` = 4 (nº de canciones; `HandleMusicSelected`
  lee el mismo campo), 4≠0, no aborta.
- Corre el arranque completo: `ShufLAUNCH` (0x081faa34) → `setup20` (0x0826b858) →
  `StartPlayback` (0x08150d84) → `playA` (0x08024798), que **encola las 4 pistas**
  (itera el accesor de playlist `0x0802fe08`).
- `ShufLAUNCH` **completa**: vuelve de setup20, invoca `GotoNowPlaying` (0x081faa90),
  el handler **retorna** (0x081faaa4). El controlador Now Playing corre (región
  0x081a0000–0x081b2000, 618 bloques distintos).

Toda la maquinaria **síncrona** del arranque corre. El muro está aguas abajo, en el
pipeline MeCCA **asíncrono** — no en el despacho del handler.

### 11.2 Tres muros de RELOJ de audio (polls de bits de hardware que el modelo no ponía)

**Muro A — I2S CLKCON bit 1 (medido: 1.6e8 lecturas).** El driver de audio del
retailOS en IRAM (0x22008808–0x2200881c) **deshabilita** el reloj I2S
(`str 0,[CLKCON]`) para reconfigurarlo y luego **gira** `ldr CLKCON; tst #2; beq`
esperando el bit 1 (estado "reloj parado"). El modelo devolvía el 0 escrito → spin
infinito. **Arreglo** (`hw/arm/ipod_classic/ipod_classic_i2s.c`, read de CLKCON):
bit 1 = "reloj parado", puesto cuando el bit 0 (enable) está a 0.

**Muro B — SM1 (0x38500000) registro de identidad.** El init del reloj de audio
guarda 0x38500000 en su tabla de handles (setter `0x08083df0`, tabla @0x08ade488) y
luego lee **SM1+0xc80** (getter `0x08075d64`), lo compara con el magic `0x004e7309`
(literal `*0x080821cc`) y hace **`b .`** (cuelgue eterno en 0x080819b8) si no
coincide. SM1 estaba sin implementar (0) → cuelgue. VERIFICADO en runtime: en el
cuelgue SM1+0xc80 leía 0. **Arreglo** (`hw/arm/ipod_classic/ipod_classic.c`): SM1
pasa de `create_unimplemented_device` a un device mínimo que devuelve `0x004e7309`
en +0xc80.

**Muro C — SM1 bit "reloj listo".** Tras la identidad, el init **pollea SM1+0xa98**
esperando el bit 2 (`tst r0,#4; beq` en 0x08081af8) — el flag "reloj corriendo/lock".
SM1=0 → spin en 0x080a7658. **Arreglo**: SM1+0xa98 devuelve el bit 2.

**SM1 (0x38500000)**, etiquetado "clock divider" en el mapa, es en realidad el
**controlador de reloj de audio**: el retailOS hace un handshake de varios registros
(identidad +0xc80, estado +0xa98, escrituras a +0x824/+0xc48). El device modela SÓLO
el handshake que el arranque necesita, no el controlador completo (anotado en el
código).

### 11.3 Estado actual: la SALIDA se arma, el DECODE sigue mudo

Con los tres arreglos **+ `AUDIOPACE=1`** (necesario: el pacing del DMA es lo que hace
avanzar el pipeline; sin él la tarea de audio se duerme antes del init del reloj), la
**salida** se inicializa por completo (medido en runtime): I2S `CLKCON=0x1`,
`TXCON=0x0b100019`, **`TXCOM=0x6` (bit de run ACTIVO)**, reloj de audio armado.

PERO el **decodificador software MeCCA sigue sin producir PCM**: el mem-watch no
invasivo sobre los buffers ping-pong (0x22008c5c/0x22009c5c) mide **0 escrituras**
tras el SELECT; el I2S corre pero drena SILENCIO (buffer a cero); AUDIOWAV sale a
cero; el sistema entra en WFI idle; Now Playing no se dibuja. La tarea de decode está
**BLOQUEADA** esperando un evento (candidatos: la entrada comprimida desde disco vía
StreamCache que no completa; o un semáforo "buffer listo" que no se postea).

Resumen: **cayeron los muros de la SALIDA (reloj I2S/SM1); el muro que queda es el del
DECODE** — el núcleo de la Rama A. El decode MeCCA no arranca a producir PCM aunque la
salida ya esté lista para consumirlo.

### 11.4 Herramienta: mem-watch no invasivo en `pctrace`

`contrib/plugins/pctrace.c` extendido con `memw=0xLO-0xHI` (+`memcap=N`): registra cada
STORE de la CPU en el rango con su PC, sin parar la CPU (callback de memoria en el hilo
de la vCPU) — el watchpoint de la Rama A: en cuanto el decode escriba PCM en 0x22008c5c
saldrá el PC del decodificador. **Coste**: registra callback en cada instrucción →
ralentiza mucho; NO usarlo durante el boot completo (el huésped pierde deadlines y el
modelo de batería resetea). Usar ventana corta o ya en reproducción.

### 11.5 Lo que sigue

- **El decode MeCCA bloqueado** es el muro restante. Averiguar el punto de bloqueo
  RTXC. `pctrace` de QEMU 8.2 no lee registros, así que el LR del bloqueo necesita otra
  vía: stack de la tarea RTXC, o un plugin con la API de lectura de registros de QEMU
  9.x. Candidatos: lectura del MP3 (ATA/StreamCache) que no completa; semáforo no
  posteado.
- **SM1 (0x38500000)**: modelar el controlador de reloj de audio como es debido, no
  sólo el handshake mínimo; es probable que el decode dependa del reloj REAL, no de los
  bits "listo" fingidos.
- **AUDIOPACE**: el arranque del audio del retailOS lo necesita. Evaluar activarlo por
  defecto (seguro para Rockbox según [16](16-classic6g-rockbox.md)) o auto-activarlo al
  arrancar el I2S del retailOS.
- Sin regresión: `navtest` 6G pasa (05-select 19498 px, rueda dos mitades; 09-into-music
  es la flakiness conocida del SELECT, no regresión; 0 resets) y Rockbox sigue navegando.
  Los arreglos de CLKCON/SM1 sólo se ejercen en el camino de audio del retailOS.

### 11.6 SM1 modelado como dispositivo con estado (sustituye los bits fingidos)

Los tres arreglos de reloj de §11.2 se sustituyeron por un **dispositivo QEMU con
estado**, `hw/arm/ipod_classic/ipod_classic_sm1.c` (+ `.h`), registrado en la
máquina en vez del `create_unimplemented_device`. Modela el handshake COHERENTE
(el estado deriva de las escrituras del huésped, no constantes a mano):
- **+0xc80 (ID)** devuelve el magic `0x004e7309`.
- **sub-bloques** (`base + {idx*0x20; 0x100/0x180/0x200/0x230}`): al escribir un
  comando en `sub+0x10`, `sub+0x14 & 7` pasa a leer **7** ("alcanzó estado"), que
  es lo que el poll de 10 intentos en `0x080ba768` espera.
- **+0xa98 bit2 (clock-ready)** y **+0x400 (run latch)** se derivan de un flag
  `powered` que se pone con la primera escritura de bring-up (comando de canal,
  `+0x824`, `+0x400`, `+0xc48`).
- **+0xa48 (posición de reproducción)** se deriva de `i2s->tx_samples/2`.
- Todo lo demás es latch (se guarda lo escrito, lee 0 al reset). `SM1TRACE=1`
  imprime cada acceso con PC.

**MEDIDO (traza `SM1TRACE`, disco `~/6g/Emulti.img`, `AUDIOPACE=1`):** en el play
de Shuffle Songs el handshake corre **UNA vez, 33 accesos, y COMPLETA sin spin**:
`rd +0xc80 -> 0x004e7309`; `wr +0x110 = 4`; `rd +0x114 -> 7`; `wr +0x110 = 16`;
`wr +0x110 = 96`; config de códec `+0x93x..+0x97x` (una tabla desde `0x080b68b4`);
al final `rd +0xa48 -> 0x002a9000` (posición avanzando). **Conclusión medida: el
modelo SM1 está bien y NO es el muro** — el handshake que fingíamos ahora se
satisface de verdad, y aun así no suena. (Confirma el aviso del auditor por la vía
opuesta: modelar SM1 "de verdad" era correcto pero insuficiente.)

### 11.7 La premisa "0 escrituras al ping-pong" era un artefacto; el muro es el DECODE

Re-medido con `pctrace memw`: el buffer se escribe **millones** de veces (driver
IRAM `0x22000294/0x22000298` a ritmo de audio, casi todo silencio) y el **mixer SW
`0x080fb2b0`** mete exactamente los **clics de UI** (`AUDIOWAV`: ~9 360 tramas no
nulas, pico 11444, L==R, ~15 ráfagas aisladas — clics, NO música). El **tubo de
salida funciona de punta a punta**; lo que falta es que la MÚSICA decodificada
entre en él. (Coincide con el segundo informe de la sesión previa.)

### 11.8 El camino consumidor, mapeado entero (traza + gdb) — el copy SALTA por falta de PCM

Con `pctrace range=` disparado en `StartPlayback` (0x08150d84, `hits=1`) se capturó
la huella del arranque del decode: **~10 787 bloques distintos en ~367 ms**, luego
**cero bloques nuevos** — el sistema se asienta en un lazo estable (vuelve al event
loop de UI `0x08105a1c`/`0x0812xxxx`). Regiones tocadas: media player `0x0815`,
StreamCache `0x081f`, códec ALAC `0x0808`, tabla de códec `0x0832` (75 bloques).

La **cadena consumidora** (reconstruida con `pctrace pc=` y **gdbpoke** — pila +
registros en el copy, sin corromper la entrada porque el copy corre DESPUÉS de que
el SELECT despachó):

```
  tarea de audio 0x07ffb2d0 (entry de tarea RTXC; sin bl directo)
    -> veneer 0x07ff89f8 -> motor de audio 0x0808170c   (bring-up SM1 + switch de códec)
      -> mixer 0x0809a74c   (itera lista de streams: ldr r4,[r4,#4]; llama al render por stream)
        -> render 0x08016f14 -> 0x08016f68
          -> COPY PCM 0x08016e44   (llama al getter de posición SM1+0xa48 en 0x08091710)
```

El **copy 0x08016e44** corre **UNA vez** (`copyFn hits=1`) y toma la rama **SKIP**
(`copySKIP 0x08016ecc hits=1`, `copy32 0x08016eb4 hits=0`). La puerta: `ldrb
r1,[r7,#5]; cmp r1,#1; bne skip` en `0x08016e88`. **gdbpoke midió en runtime**
(estable entre corridas): `[r7+5] = 0` (la puerta exige 1), `[r4] = 0` (ruta de 32
bits), fuente `r0 = 0x08889098`, descriptores `r4=0x0bb12d54`, `r7=0x0bb12eb4`. El
volcado de la **fuente 0x08889098 NO es PCM** (punteros a sí misma + relleno
`0x5da35da3`); los descriptores están **medio-inicializados** (relleno `0x5da3`,
que en toda la imagen aparece 2 veces → valor de heap, no PCM). El copy **hace bien
en saltar**: no hay muestras válidas que copiar.

### 11.9 H1 CONFIRMADO por medida: el decodificador NO escribe ni una muestra

`pctrace memw=0x08888000-0x0888c000` (la región de la fuente 0x08889098) durante el
play: **8 192 stores, de sólo 2 PCs — `0x22000198` y `0x220001a0`, que son el
memset/clear de IRAM. CERO escrituras del decodificador.** Es decir, el buffer de
stream se **limpia** pero **nunca se rellena con PCM**. Esto zanja H1 vs H2:

> **El decodificador MeCCA nunca produce PCM (H1). No es un problema de ruteo/mute
> (H2): no hay muestras que rutear.** El motor `0x0808170c` corre una vez (por eso
> el handshake SM1 es único), mezcla el stream vacío, el copy salta, y la tarea se
> bloquea. La tarea PRODUCTORA (decodificador), aguas arriba del stream, está
> parada antes de la trama 0.

Datos colaterales: `media_dispatch_chain 0x081f87f8` corre 248 veces y `strcmp`
131 240 veces en la corrida (framework MeCCA muy activo por búsquedas con clave de
cadena). El códec (región ALAC `0x0808`, tabla `0x0832`) **sí** hace setup, así que
**no** es "códec no encontrado" tipo el muro de las fuentes (§2): el códec se
inicializa pero se bloquea antes de decodificar.

Mecanismo RTXC relevante (ya documentado en `ipod_classic.c:634-641`): las tareas
hacen `KS_wait` SIN timeout sobre el semáforo de un canal DMAC0 y sólo las despierta
el ISR de terminal-count (IRQ 16). Ese mecanismo **funciona** (el almacenamiento
monta, la salida drena). El pacing (AUDIOPACE) sólo afecta la línea IIS0_TX; el DMA
mem-a-mem del decodificador NO es paced. Así que el bloqueo del decode **no** es un
IRQ/pacing que falte — es un semáforo/evento de software aguas arriba.

### 11.10 El obstáculo exacto que queda y cómo cerrarlo

El decodificador MeCCA (tarea RTXC separada, aguas arriba del stream) se bloquea en
un `KS_wait` antes de la trama 0 y nunca rellena `[r5+12]` (la fuente del mixer). Su
PC/LR de bloqueo sigue **DESCONOCIDO**: la cadena de mixer/render/copy está mapeada
entera (consumidor), pero la tarea PRODUCTORA no se localizó por estático (despacho
por vtable + entradas de tarea sin `bl` directo). Para cerrarlo:
- **gdb ya funciona en el play** (no corrompe la entrada si el breakpoint cae tras
  el despacho del SELECT): poner un breakpoint en el primitivo `KS_wait` de RTXC y,
  cuando la tarea de audio se bloquee durante el setup, leer `bt`/LR → la función de
  decode y qué semáforo espera. Falta localizar `KS_wait` (mapear la tabla de tareas
  RTXC / el TCB de la tarea de audio).
- Herramienta nueva reutilizable: `gdbpoke.py --memreg IDX[+OFF]:LEN` (volcado de
  memoria relativo a registro) — lo que hizo posible leer la fuente y los
  descriptores del copy sin adivinar direcciones. `pctrace` ganó `memwpc=` (destino
  de stores en un PC concreto).

### 11.11 La caza RTXC en vivo: DOS hipótesis descartadas por medida, y el muro re-encuadrado

Sesión de traza en vivo con `gdb-multiarch` (SÍ está en el WSL, 15.1) + `pctrace`.
Play arrancado por **曲をシャッフル (Shuffle Songs)**, último ítem del menú principal
(japonés en este firmware), disco `~/6g/Emulti.img`, `AUDIOPACE=1`. Todo lo de abajo
**VERIFICADO** salvo lo marcado.

**Técnica que funciona (anotar):** `gdb.execute("continue")` desde el API Python de
gdb corre ASÍNCRONO contra el gdbstub de QEMU y falla ("Cannot execute while
running"); un **fichero de comandos `.gdb`** con `continue` de CLI (y bucles
`while`) SÍ bloquea. Todo el trabajo en vivo se hizo con `-batch -x script.gdb`.
Recetas en `~/dbg/` (WSL): `enum_bt.py` (volcado + backtrace de las 56 tareas de un
tiro, un solo attach), `catch.gdb` (caza el motor y lo sigue hasta su bloqueo),
`catch4.gdb` (lee el estado del HW de audio). **Un solo attach por captura** — el
ciclado attach/detach es lo que inducía el spin EIC (ver abajo).

**A) El spin EIC (0x22002e24) NO es el muro — DESCARTADO por medida no invasiva.**
La rutina de reloj/energía `0x22002dc0` (`iPodPowerProfile.txt`) reprograma un PLL
(`r0=CLK 0x3C500000`, `r1=EIC 0x39A00000` = `r0-0x2b00000`) y pollea EIC bit0
(`0x22002e24`) y bit12 (`0x22002e48`) con IRQs enmascaradas; EIC sin implementar lee
0 → spin eterno. **`pctrace pc=0x22002dc0`/`pc=0x22002e20` en un play limpio (sin
gdb): hits=0.** La rutina NUNCA se entra durante la reproducción. El spin que
capturó la sesión anterior era 100% artefacto del ciclado de gdb (un IRQ perdido en
un detach deja la rutina de reloj a medias). The EIC is not part of the playback path (it is modelled for standby; see
`ipod_classic_eic.c`). El estado estable
limpio es WFI idle en `0x22003b0c–0x22003b30`, cpsr con IRQs HABILITADAS.

**B) El muro NO es un periférico que falte ni uno que devuelva mal — DESCARTADO.**
`-d unimp,guest_errors` durante el play: los ÚNICOS accesos no modelados son 43
**escrituras** de config a GPIO `+0x200` (setup de IRQ de rueda/botones, no lecturas
que condicionen). **Cero** accesos a AES / SHA / ADM(`0x39/0x3F`) / SM1-window-alto
/ io-floor. El decode no alcanza ningún bloque sin modelar. `pctrace` de la cadena:
`audio_engine 0x0808170c`, `mixer 0x0809a74c`, `copyPCM 0x08016e44` corren **exactamente
1 vez**; `copy32 (PCM real) 0x08016eb4` = **0**. `KS_ISRSignal 0x22001ca4` = **7621
veces** (las ISR SÍ disparan). SM1: `SM1TRACE` en play = **0 accesos tras el SELECT**
(SM1 no se pollea durante el streaming); `catch4.gdb` en idle: SM1 `STAT(+0xa98)=0`,
`RUN(+0x400)=0` — ni siquiera "powered". SM1 es irrelevante en el camino de play.

**C) La salida de bajo nivel FUNCIONA — idéntica a Rockbox (VERIFICADO por registros).**
`catch4.gdb` en el idle de play leyó el HW de audio: **DMAC0 canal 1** activo —
`SRC=0x22009c5c` (ping-pong IRAM), `DEST=0x3ca00010` (FIFO del I2S), `LLI=0x22010020`
(cadena continua), `CTRL=0x84249800` (bit I=terminal-count IRQ), `CONF=0x8a81`
(E=1 habilitado, `dest_id=0xA`=IIS0_TX, flow=1 mem→peri, ITC=1). I2S `TXCOM=6` (RUN),
`CLKDIV=0x110`. Es **exactamente** el mecanismo que usa Rockbox (LLI-chain +
terminal-count IRQ 16) y que ya suena. DMAC1 sin usar. **El muro NO está aguas abajo
del ping-pong.** La `SM1 POS` avanzó a 0x27d800 (≈2.6M): el I2S drena silencio de
punta a punta desde el arranque (por eso `SM1pos` en `StartPlayback` ya era 1.97M ≈
44 s de silencio de boot).

**D) El motor de audio corre en la TAREA 0x25 (VERIFICADO por breakpoint).** `catch.gdb`
rompió en `0x0808170c`: corre en task `0x25` (worker MeCCA, entry `0x080833d0`),
llamado desde el driver IRAM de audio (`lr=0x22006304`, IRAM `0x22006xxx` = shadow
DRAM `0x07ffbxxx`; la "tarea de audio 0x07ffb2d0" de §11.8 es IRAM `0x22006118`).
Tras su ÚNICA corrida, task `0x25` bloquea en **buzón/exchange 0x52** vía servicio
**opcode 14** (wrapper `0x22004028`, un `KS_receive` de mailbox; el handler
`0x22000950` indexa la tabla de buzones en **`0x0897783c` stride 32**, distinta de la
tabla de semáforos opcode-1 `0x0897fb1c`). El copy `0x08016e44` corre 1 vez y SALTA
por `[r7+5]=0` (PCM no listo) — no por la posición SM1.

**E) Mapa de tareas RTXC en runtime (56 TCB, VERIFICADO).** `enum_bt.py`: array de TCB
`0x0897ff70` stride 52 (`+12` entry, `+16` savedSP, `+28` estado, `+32` tasknum; el
TCB no guarda puntero al nombre). Estados vistos: `0x40`=espera-semáforo (opcode 1,
resumePC `0x2200433c`), `0x08`/`0x20`/`0x04`/`0x01`=otras esperas (opcodes 14/4/…),
`0x00`=libre/corriendo. ~30 workers genéricos `0x080833d0`. Piezas clave:
- **task 0x20** = tarea de StreamCache/media (bt `0x08171500→0x081f39a4→0x081d0a2c→…→
  0x081fed34`), **idle esperando RECIBIR en exchange 6** (opcode 4). Es un consumidor
  ocioso, no "el productor bloqueado" que creía §11.10.
- **task 0x25** = motor/driver de audio, bloqueado en exchange 0x52 tras 1 corrida.
- Muchos workers parados en **`0x080c8998`** (retorno de `bl 0x8033d8c` = el `KS_wait`
  del RPC síncrono `0x080c8968`): hacen una petición y esperan la respuesta. El RPC
  `0x080c8968` es genérico (get-event `0x8033b70` → post `0x80cc0e4` → signal
  `0x8033af8` → wait reply): corre ~3728 veces en 18 s de play, NO es específico de
  audio.

**F) El disco NO es el problema.** La pista de `Emulti.img` es un **M4A/ALAC válido**
(`ftyp`@0x818004, `moov`, `mdat`@0x824824, fourcc `alac`) — contenedor MPEG-4 bien
formado, no corrupto. El códec ALAC se inicializa (§11.9); el "ID3" hallado en el
scan es ruido coincidente.

**RE-ENCUADRE DEL MURO (medido, honesto):** en el punto de bloqueo el sistema es un
**deadlock de mensajería MeCCA**: TODAS las tareas están bloqueadas en exchanges/
buzones inter-tarea, y NINGUNA está polleando un periférico. Por tanto **ningún
cambio del modelo de un periférico puede desbloquearlo directamente** — lo que falta
es un *mensaje* (un `KS_send`/`KS_signal` de tarea a tarea) o un paso del setup
SÍNCRONO que debió despachar el primer decode y no lo hizo. Las ISR que sí disparan
(tick de timer + DMA IRQ 16) despiertan al driver de salida de bajo nivel (rellena
silencio) pero **no** disparan la tarea de decode. El decode nunca produce la trama 0.
El deadlock es DURO (§11.8: 0 bloques nuevos tras 367 ms), no un timeout lento.

**El siguiente nodo, preciso:** trazar el setup SÍNCRONO de reproducción desde
`StartPlayback 0x08150d84` (media player `0x0815xxxx` → apertura del M4A → init del
códec ALAC → **el punto donde debería encolar/señalar el primer decode**) y hallar la
rama que NO despacha esa petición. Pista de no-determinismo: la sesión anterior
alcanzó el handshake SM1 (33 accesos, §11.6) y ésta no (0 accesos) — el mismo play
se atasca en puntos distintos → **hay una carrera/timing** (candidato: el instante
del primer IRQ 16 respecto al setup). Método sugerido: breakpoint en el `KS_receive`
del worker de decode y en `0x080c8968`/`0x8033af8` filtrando por el exchange que toca
el media player; o comparar el árbol de `pctrace range=` de un play que progresa vs.
uno que se atasca.

**Experimento de timing PROBADO y DESCARTADO (esta sesión):** hipótesis "el primer
IRQ 16 del DMA de audio llega demasiado pronto y el setup del decode pierde/atropella
la señal". Se añadió temporalmente un knob `AUDIOPACE_START_MS` que retrasa el primer
tick de drain (y con él el primer terminal-count IRQ 16) N ms; probado con 1500 ms.
Resultado: **no confirma** — no cambia el muro (`copy32=0` sigue), y peor, retrasar la
salida ~1.5 s tiende a **inducir resets** (el retailOS parece vigilar el avance de la
reproducción y se resetea si se estanca). Coherente con el diagnóstico: el decode NO
espera el IRQ 16 (la salida de bajo nivel va sola, §C) sino un MENSAJE de tarea aguas
arriba. El knob se **revirtió** (árbol limpio). El timing del IRQ 16 no es la palanca;
el siguiente esfuerzo va al setup síncrono del decode, no al pacing del DMA.

**Higiene:** cero cambios en fuentes del modelo (sólo lecturas/breakpoints gdb y
`pctrace`, ya en el árbol). Árbol byte-idéntico al baseline. Rockbox/navtest intactos.
Instrumentación de sesión en `~/dbg/` del WSL (fuera del repo).

### 11.12 El muro re-caracterizado por MEDIDA: NO es un evento de hardware — es un deadlock de rendezvous puro (y correcciones a §11.6/§11.8/§11.F)

Sesión de medición dedicada al grafo de espera y al setup del decode. Play limpio de
`~/6g/Emulti.img`, `AUDIOPACE=1`, disparado por Shuffle Songs. Todo lo de abajo
**VERIFICADO** por lectura/breakpoint gdb (no invasivo) o instrumentación temporal del
modelo (revertida). Recetas en `~/dbg/` (`probe6.sh`=wgraph, `probe7.sh`=HW+timers,
`probe8.sh`=codec-setup, `probe9.sh`=tasas de ISR; `catch5.gdb`, `catch_setup.gdb`).

**A) La pista es AAC-LC, NO ALAC (corrige §11.F). Y el códec SÍ se configura (refuta la
hipótesis "AAC → rama default").** Las 6 pistas de `Emulti.img` son `mp4a` en el `stsd`
(esds objectTypeIndication `0x40`, ASC `0x1210` = AAC-LC 44100/estéreo), medido con
`ffprobe`+volcado del `stsd`. La cadena "alac" que vieron sesiones previas es la tabla
fourcc del demuxer + bytes de `mdat`, no la sample entry. **CORRECCIÓN de dirección:** el
getter `0x08015860`→IRAM `0x22006078` devuelve la struct de estado en **`0x220106dc`**
(no `0x22008c4c`, que es sólo un latch de init interno); el tipo de códec vive en
`0x22010734` (+0x58) y el gate en `0x22010723` (+0x47). Con breakpoints de baja
perturbación (`probe8.sh`) se MIDIÓ en vivo durante el setup: `CODEC_SETUP`
(IRAM `0x22006574`) corre con **type=0x01**, `GATE_SET` (`0x220065ac`) pone gate=1, y el
motor `0x0808170c` lee **gate=1, type=1** → toma el **caso 1** del switch (`0x0808182c`),
que SÍ cablea el pipeline. **El tipo 1 es un caso manejado; el motor se configura bien.**
(Las lecturas snapshot que daban type=0xff/gate=0 son *post-teardown*: el estado se
reinicializa cuando la reproducción falla — un espejismo. Medir en vivo, no en el atasco.)

**B) El grafo de espera exacto (regime A, deadlock estable, resets=0, `wgraph.py`).**
- **t2a** (decodificador AAC, cluster `0x08123xxx`) BLOQUEADO recibiendo en **buzón 0x09**
  (`0x08977964`); `M09.seq≈52` → StreamCache le sirvió ~52 trozos. Está **hambriento**
  (esperando input), no fallando al decodificar.
- **t25** (motor de audio) BLOQUEADO recibiendo en **buzón 0x52** (`0x08978284`);
  `M52.seq≈18-20`. Corrió su setup UNA vez (type 1) y espera comandos.
- **t20** (StreamCache) BLOQUEADO recibiendo en **exchange 6** (`sema[6]=0x20`,
  `0x0897fb34`). Es el SERVIDOR: dueño de los buzones 0x09/0x52/0x4e.
- t20 sirvió ~52/18 veces y luego se durmió en exchange 6 esperando el disparo #53. El
  punto de atasco es **reproducible/determinista** (M09≈52-53, M52≈18-20 en 4 corridas).

**C) NO hay evento de hardware perdido ni mal temporizado — DESCARTADO por medida directa
(`catch5.gdb` en el atasco).** VIC0 `RawIntr=0` (CERO IRQ pendiente), `IntEnable=0x21a7010f`
(irq16 DMAC0 e irq29 ATA habilitados). DMAC0 `RawIntTC=0`, **ch0 (almacenamiento) IDLE
(E=0)** — el disco ya está leído, no hay DMA colgado; ch1 (audio) sigue E=1 dando vueltas
a su LLI. ATA quiescente (transfer completo). **No hay ninguna interrupción pendiente que
el guest no esté sirviendo.**

**D) Las ISR que importan SÍ disparan, continuamente, DENTRO del atasco — DESCARTA "tick
muerto"/"IRQ perdida" (instrumentación temporal `IRQDBG`, `probe9.sh`).** El **tick RTXC
de 1 kHz** (timer B, 16-bit, ECLK/2, DATA0=6000, INT_EN, IRQ 8) dispara **~1000/s** sin
parar durante el deadlock. La **IRQ 16 de audio (DMAC0 ch1)** dispara **~50/s** (tempo
real, ~43 ráfagas/s de 2048 muestras). El almacenamiento (ch0) está parado (disco leído).
Es decir: el scheduler RTXC recibe su tick 1000 veces por segundo y el driver de salida su
IRQ 50 veces por segundo, **y aun así nadie señala el exchange 6.**

**RE-ENCUADRE FINAL (medido, honesto):** el muro **NO** encaja en ninguno de los tres casos
de "arreglo en el modelo por evento de periférico": no hay evento externo mal temporizado
(C: sin IRQ pendiente), no es que el tick/DMA falte (D: ambos vivos), y no hay latencia de
hardware que reproducir (el disco/ATA/SM1 están correctos y ociosos). Es un **deadlock de
rendezvous inter-tarea PURO**: el `KS_send`/`signal` al exchange 6 que despertaría a
StreamCache proviene de una tarea del **camino de datos** que está ella misma bloqueada
(t2a hambrienta ↔ t20 en ex6 ↔ t25 en buzón 0x52). El primer dominó —la PRIMERA muestra de
PCM (`copy32 0x08016eb4` = 0 siempre)— nunca se produce, así que el ciclo
productor↔consumidor nunca arranca, y ni el tick de 1 kHz ni la IRQ de audio lo rompen
(sus manejadores no tocan el exchange 6 mientras el buffer drenado sea silencio).

**Lo que queda DESCONOCIDO (el obstáculo exacto):** QUIÉN debería señalar el exchange 6 en
el arranque en frío y por qué su tarea no corre. Es un problema de protocolo del framework
MeCCA (bootstrap del pipeline), no de un registro/temporización de periférico. Como la
carrera regime-A/regime-B es sensible a la velocidad del guest (instrumentación pesada →
regime B), atrapar el `send`-a-exchange-6 en vivo exige una sonda de MUY baja perturbación
(los watchpoints sobre la página de datos del kernel `0x0897xxxx`, que es caliente, empujan
a regime B). Vía sugerida: localizar el veneer del primitivo `send-exchange` de RTXC (opcode
del KS, tabla en `0x22003620`) y ponerle un breakpoint condicionado al índice 6 —
suficientemente raro para no perturbar— para leer el LR del emisor de los ~52 señales que SÍ
ocurren antes del atasco, y ver por qué se detiene la #53.

**Higiene:** instrumentación `IRQDBG` (contadores en `hw/dma/pl080.c` y
`hw/arm/ipod_classic/ipod_classic_timer.c`) **revertida**; árbol byte-idéntico al baseline;
recompila y enlaza limpio. Cero PCM inyectado, cero parche de firmware. `pctrace` intacto.
Rockbox/navtest sin tocar (los caminos medidos son sólo lecturas del retailOS).

### 11.13 El lado CONSUMIDOR re-caracterizado: el motor NUNCA recibe disparo de render; el muro es un bootstrap circular simétrico del primer ciclo

Sesión con la lectura de registros no invasiva ya operativa (`sendex=`/`regpc=`,
§ commit `0a0ea0365`). Play limpio de `~/6g/Emulti.img`, `AUDIOPACE=1`, Shuffle
Songs. Todo **VERIFICADO** por lectura/breakpoint gdb no invasivo o por contadores
`pctrace pc=` (cero perturbación medida: `resets=0`). Recetas nuevas en `~/dbg/`
(fuera del repo): `runt2a.sh`+`t2a_probe.py` (grafo+pila unwound), `runcounts.sh`
(perfil del decode), `runmbox.sh` (atribución de emisores por LR/sp), `runtree.sh`
(árbol del decode por `range=`), `t2a_probe2/3.py`. Árbol del modelo **sin tocar**.

**A) El MOTOR de audio (t25, consumidor) NUNCA recibe un disparo de render — VERIFICADO.**
La espera de t25 es un `recv` de buzón 0x52 CON watchdog de 3 s: pila
`0808dfa8→22002f84→22006824→[recv 0x22006814]→0x080b3e78→0x805c518`, timeout
`r1=0xbb8=3000ms`. Sonda `sendex=0x22003f7c:0:0x52` (gatea el reg r0==0x52 en la
cabecera-TB del primitivo *select-mailbox* `0x22003f7c`, hardcodeado con el nº de
buzón en r0): **22 matches en el play, TODOS son el propio `recv` de t25**
(`lr=0x805c578`, `r1=0xbb8`) disparándose **cada ~3.00 s exactos** (timeout) y
re-esperando. **CERO envíos.** Nadie manda jamás un comando de render al motor.
Tras 3 s el motor hace **teardown** del códec (`type[+0x58]→0xff`): ése es el
"espejismo post-teardown" de §11.12-A y la fuente del no-determinismo entre
sesiones (según ataques antes o después del watchdog de 3 s se ve el estado limpio
o el desmontado).

**B) La ÚNICA corrida del motor (setup) intenta renderizar y sale en vacío — VERIFICADO.**
El motor `0x0808170c` (caso 1, gate=1, type=1 de §11.12-A) llama al mixer
`0x0809a74c` **8 veces** (`0x8081a9c..0x8081cb0`), pero **todos los `copy` SALTAN**
(`[r7+5]=0`, PCM no listo) → `copy32 0x08016eb4 = 0`. Su primer render corre
**demasiado pronto** (el decoder aún no ha producido la trama 0) y **no hay segundo
render** (sin disparo). `copy32=0` es invariante en todas las corridas.

**C) El DECODER (t2a) es un actor de mensajes; su decode de datos NUNCA se dispara — VERIFICADO.**
`pctrace` del cluster: el *dispatcher* `0x08123dcc` conmuta por `[msg+4]`;
el manejador **tipo-12** `0x8123970` decodifica (`bl 0x8123bfc`) **sólo si**
`[msg+8]==obj+84` (el puerto de decode) **y** `[[obj+52]+96]!=0`. Contadores en un
play completo: **`h12 (0x8123970) = 0`** (el disparo "buffer de salida disponible /
render-pull" NUNCA llega), `dstep (0x8123284) = 1`, `dwork (0x8123bfc) = 1`. Y el
`range=` de esa única corrida del decode **NO toca AAC-math**: los bloques
`0x08324xxx/0x08325xxx` que parecían "códec" son **I2C0 (`0x3C600000`)** hablando con
el chip **CS42L55** (`0x08325634`: spin en `[I2C0+0x10]`, `[I2C0+0x20]|=0x3f00`),
NO IMDCT/huffman. **Corrección:** el decode SW de AAC **no llega a correr** (H1 de
§11.9 se mantiene); lo que corre una vez es la config del chip códec por I2C, que
**completa** (mismo camino que usa Rockbox). El decode-gate `[[obj+52]+96]` se
midió **=1** en un snapshot (entrada lista) — el decoder está listo pero sin disparo.

**D) Mapa KS/mensajería consolidado (VERIFICADO por desensamblado).** Tabla de
opcodes del dispatcher KS `@0x2200064c`: **op1**=WAIT-sema, **op2**=SIGNAL-tag,
**op4**=RECV-exch, **op5**=SEND-exch, **op14**=RECV-mbox. *Select-mailbox*
`0x22003f7c` (r0=nº buzón, es cabecera-TB → `sendex`-able). Veneers del lazo de
render IRAM: `0x22003840→0x0808170c` (motor), `0x22003868→0x080b3e78` (su recv con
timeout). Dispatcher del decoder `0x08123dcc` (tipo-12=`0x8123970` decode-gated,
tipo-19=`0x8123cfc` switch sobre códigos `0x52800000+n`). La familia de verbos de
buzón-9 del decoder (`0x8025ac4` send, `0x8025a04` recv, …) está hardcodeada a mbox 9.

**E) EL MURO, re-caracterizado (medido): un DEADLOCK CIRCULAR SIMÉTRICO del primer ciclo.**
Las dos mitades se bloquean mutuamente:
- El motor renderiza PCM real **⟸** disparo de render (mbox 0x52) **⟸** la salida
  consume PCM real (devuelve crédito) **⟸** el motor renderiza PCM real.
- El decoder decodea la trama 0 **⟸** render-pull/buffer-de-salida (tipo-12) **⟸**
  el motor renderiza **⟸** el decoder decodea la trama 0.

Ningún lado está **cebado** para arrancar sin el otro. El conductor (StreamCache
t20) manda datos al decoder (mbox 9) y comandos al motor (mbox 0x52) **a medida que
fluye el crédito**; t20 está bloqueado en exchange 6 esperando crédito de su
upstream (t0e←t16), y ese crédito sólo vuelve cuando la salida consume PCM real.
**El decoder está BLOQUEADO, no lento** — tiempo infinito no lo arranca. `copy32=0`
de punta a punta.

**F) Por qué NO se halló arreglo de modelo (re-confirmado, honesto).** En el atasco
**ninguna tarea pollea hardware** (grafo de espera: todas en mbox/exchange). Los
ÚNICOS toques de HW del camino de setup — **posición SM1** (avanza, correcta) y la
**config I2C del CS42L55** (completa, idéntica al camino que Rockbox usa y suena) —
son **correctos**. Cero periférico sin modelar (`-d unimp` = sólo 43 writes GPIO,
§11.11-B); IRQ16 (~50/s) y tick de 1 kHz disparan de continuo (§11.12-D). Por tanto
**ningún valor de un periférico modelado ni evento de HW en el lazo del deadlock
puede desbloquearlo**. Un valor de HW sólo podría importar si hiciera **divergir una
rama AGUAS ARRIBA en el cebado**; se buscó (SM1, I2S, DMA, I2C, timers) y **no se
localizó** ese valor en 6 sesiones — el cebado que rompe el ciclo vive dentro del
**protocolo asíncrono de buffers/mensajes de MeCCA**, no en un registro.

**VEREDICTO HONESTO (esta sesión): NO suena.** El muro es un **deadlock de bootstrap
circular del pipeline MeCCA**, confirmado por medida como libre de todo valor/evento
de HW ausente o erróneo *dentro del lazo*. Esta sesión **no** halló un arreglo de
modelo localizado (coherente con las 5 previas), pero **afinó el mecanismo**: el
primer dominó es el **CONSUMIDOR** (el motor nunca se dispara; watchdog de 3 s →
teardown), no sólo el decoder; y es **simétricamente circular** (ninguna mitad
cebada). Para cerrarlo haría falta: **(a)** localizar el paso de **cebado** exacto de
MeCCA — quién debe emitir el primer render-pull (tipo-12) al decoder y/o el primer
comando de render (mbox 0x52) al motor **sin** esperar crédito — probablemente contra
una **referencia de HW** o el estado que deja el **bootloader** real (nuestro
`-kernel` lo salta); **(b)** si tal cebado resultara depender de un valor que el
firmware lee mal al arranque, corregir **ese** valor (empujón de modelo legítimo,
aún no hallado); **(c)** en ausencia de (a)/(b), el muro es **intrínseco** a una
emulación fiel de este bootstrap concurrente y sólo lo abriría un **HLE del
decoder/motor** o un modelo de **timing de instrucción** (icount) que reordene el
scheduling — con la salvedad del auditor de que el determinismo por sí solo no basta.

**Higiene:** cero cambios en fuentes del modelo; árbol byte-idéntico salvo esta
entrada de wiki. `pctrace` intacto (ya en el árbol desde `0a0ea0365`).
Instrumentación de sesión en `~/dbg/` del WSL. Rockbox/navtest/5G sin tocar.

### 11.14 El cebado, cerrado por medida: el lado de ENTRADA está sano; el muro es SÓLO el disparo de SALIDA; y el candidato SM1+0xa98 bit12 REFUTADO

Sesión de cierre del nodo de cebado. Se ejecutó el experimento que §11.12 dejó
propuesto y que nadie había corrido (la atribución de emisores del **exchange 6**),
se desensambló el lazo de render del motor a nivel de instrucción, y se **probó y
descartó por medida** un candidato de valor de HW nuevo. Todo **VERIFICADO** por
`pctrace` no invasivo (`sendex=`/`pc=`, disco `Emulti.img`, `AUDIOPACE=1`, Shuffle
Songs). Recetas nuevas en `~/dbg/`: `runex6.sh`, `runsm1v3.sh`. Árbol byte-idéntico.

**A) La atribución de emisores del exchange 6 (NUEVO — cierra la incógnita de §11.12/§11.13-E).**
Sonda `sendex=0x22004138:3:0x6` (el *post* KS `0x22004138` lleva `r3`=índice de
exchange, `r2`=opcode; VERIFICADO desensamblando el verbo del decoder `0x8025ac4`:
`mov r3,#6; mov r2,#5; bl 0x8015740→0x22004138`). Resultado de un play:
**57 posts al exchange 6, TODOS `r2=5` (op5 SEND-exch), TODOS desde la familia de
verbos del decoder** `0x08025xxx` (por LR: `0x8025b18`×27, `0x8025a5c`×25,
`0x80258a4`×3, `0x8025960`×1, `0x8025904`×1). **Ni un solo post** viene de una ISR,
de una tarea de disco *upstream*, ni del camino de consumo de salida.

⇒ **CORRIGE §11.13-E:** no existe "crédito de upstream (t0e←t16)" que fluya por el
exchange 6. El exchange 6 es SÓLO el canal del **decoder → StreamCache** (peticiones
de entrada; StreamCache responde por mbox 9). El lazo de ENTRADA
decoder↔StreamCache es **autónomo y está SANO** (57 intercambios, coincide con
`M09.seq≈52`). Se llena hasta su tope y se detiene esperando que el consumidor
(el motor) tire — que nunca ocurre. **El muro NO está en la entrada.**

**B) El muro es categóricamente el DISPARO DE SALIDA — re-confirmado.** En el mismo
play: `sendex=0x22003f7c:0:0x52` = **25 matches, todos el propio `recv` del motor**
(cero envíos a mbox 0x52); `copy32 0x08016eb4 = 0`; `engine=1, mixer=1, copyfn=1,
disp=6, h12=0`. Idéntico a §11.13. **Nadie manda jamás el comando de render (mbox
0x52) al motor**, y el lado de entrada demuestra que **no es por falta de datos**.

**C) El lazo de render del motor, a nivel de instrucción (VERIFICADO por desensamblado).**
La tarea 0x25 (driver de audio, IRAM) llama al motor **una vez** en el setup
(`0x22006300: bl 0x22003840→0x0808170c`) y luego **espera**:
`0x22006814: recv(obj=0x220106d0, timeout=0xbb8=3000ms)` vía `0x22003868→0x080b3e78`;
en timeout retorna −1 y el driver hace **teardown**. El buzón del motor es un campo
de la struct `0x220106d0`; **nadie postea a él** (§B). El disparo de render es un
**mensaje de tarea a tarea** (StreamCache t20 → motor t25), cuya rama de envío en
StreamCache **nunca se alcanza** (§11.13-B: 0 `select` de mbox 0x52 para enviar).

**D) Candidato de valor de HW NUEVO — PROBADO y REFUTADO por medida.** El cuerpo de
setup `0x0808af58` lee **SM1+0xa98** (getter `0x080a7638`, `r1=0` → `[obj+0xa98]`,
`obj` = `handleTable[0x08ade488]` = base SM1) y **gatea sobre el bit 12 (0x1000)** un
bloque que escribe **SM1+0x804=1** (`0x80a35a0`) y **SM1+0x858=1** (`0x809a72c`,
estrobos de "enable" del motor de audio). Nuestro modelo SM1 sólo pone el **bit 2**
(clock-ready) en +0xa98 → bit 12 siempre 0 → el bloque de enable se salta. Candidato
plausible que las sesiones previas (centradas en el bit 2) no habían tocado.
**Medida (flag `SM1_ENGINE=1` que fuerza el bit 12, revertida):**
`copy32 = 0` (sin cambio) **y** `setupbody 0x0808af58 hits = 0` — el lector del bit 12
**no está en el camino de play** (es init de arranque del subsistema de audio, no el
cebado por-reproducción); además los dos writes gateados son **latches inertes** en el
modelo (nada del render depende de SM1+0x804/+0x858). **No es el arreglo.** Revertido;
árbol byte-idéntico (`git diff` vacío, recompila y enlaza limpio).

**VEREDICTO (esta sesión): NO suena — muro ESTRUCTURAL, cerrado por medida.** El
cebado del primer ciclo MeCCA **no depende de ningún valor/registro/evento de HW ni
de estado dejado por el bootloader**: es un **mensaje inter-tarea** (el comando de
render mbox 0x52 de StreamCache al motor, y simétricamente el render-pull tipo-12 al
decoder) cuya rama emisora está gateada por la condición **circular** "salida consumió
PCM real / hay trama decodificada" — y `copy32=0` la mantiene falsa de punta a punta.
La atribución del exchange 6 (§A) elimina la última hipótesis de "crédito externo":
la entrada está sana; **falta exclusivamente el disparo de salida en frío**, y ese
disparo no lee nada corregible. Coherente con las 6 sesiones previas y afinado.

**La vía real (documentada, no es de modelo):** un **HLE del cebado** que emita en
frío **ambos** disparos —el primer mbox 0x52 al motor (`recv obj 0x220106d0`) **y** el
primer tipo-12 al decoder (`0x08123970`, gate `[[obj+52]+96]!=0` ya =1)— para romper
la simetría; el PCM lo produciría entonces el decoder del huésped. Es un shim del
protocolo MeCCA, **no** un valor de HW: por eso no cabe como "arreglo de modelo". Un
modelo de `icount` (QEMU 9.x) podría además explorar la carrera regime-A/B (§11.11)
por si el orden concurrente real difiere, pero el determinismo por sí solo no garantiza
que aparezca el emisor ausente.

**Higiene:** experimento SM1 bit12 **revertido**; árbol byte-idéntico al baseline
(`git diff` vacío) salvo esta entrada de wiki; recompila limpio. `pctrace` intacto.
Recetas de sesión en `~/dbg/` (`runex6.sh`, `runsm1v3.sh`), fuera del repo.
Rockbox/navtest/5G sin tocar (los caminos medidos son sólo lecturas del retailOS).

### 11.15 El shim HLE del tipo-12: IMPLEMENTADO y DISPARADO, pero el tipo-12 NO es el muro (corrección medida a §11.13/§11.14)

Sesión de **implementación** del shim HLE que las siete previas dejaron
especificado. Se construyó el disparo del **tipo-12 render-pull** al decoder y se
midió su efecto con navegación FIABLE (la pieza que faltaba). VA = off +
`0x07FF51B8`. Todo **VERIFICADO** por `pctrace` (contadores `pc=`/`sendex=` +
`memw`) y por la traza del propio shim. Disco `Emulti.img`, `AUDIOPACE=1`.

**Lo implementado (queda en el árbol, GATEADO, build por defecto byte-idéntico).**
`hw/arm/ipod_classic/ipod_classic_hle_audio.c` (+ `.h`), cableado en la máquina y
disparado por el bit *run* del I2S. Con `HLE_AUDIO_KICK` sin poner **no instala
nada** (init retorna antes de tocar estado; el hook del I2S es un no-op). Con
`HLE_AUDIO_KICK=1`: un timer de reloj virtual vigila la puerta render-pull del
decoder (`decoder_obj` = heap `0x0ba9a8a4`, VALIDADO por su vtable `[obj+0]==
0x088d1948`; gate `[[obj+0x34]+0x60]`); cuando la puerta es !=0 (primed) y la CPU
está en el **idle loop RTXC** (WFI, `0x22003b0c–0x22003b30`, para no pisar el
contexto de ninguna tarea viva), redirige el contexto idle a llamar al
**dispatcher del huésped `0x08123dcc`(decoder_obj, msg)** con un mensaje de 12
bytes (`[msg+4]=12`, `[msg+8]=obj+0x54`) escrito bajo la pila idle y LR de vuelta
al idle. Cero PCM inyectado; el mensaje es exactamente el que el handler
`0x08123970` exige. (`HLE_AUDIO_KICK_FORCE=1` = knob de depuración que dispara aun
con gate=0, para ejercitar la entrega; `HLE_AUDIO_KICK_TRACE=1` traza.)

**A) EL DECODER SÍ SE CEBA — y hacía falta NAVEGACIÓN FIABLE (corrige §11.11–14).**
Las sesiones previas medían con un SELECT flojo (además, el `grep StartPlayback`
casa con la línea *watch* que `pctrace` imprime al arrancar → falso positivo → una
sola pulsación de SELECT). Eso las dejaba en **régimen B (infra-progresado)**:
`disp=2`, gate=0, el decoder ni se configura. Con SELECT FIABLE (`iremote press
ret -u -w q,e --region right`, reintenta hasta que cambia la pantalla) el play
**arranca de verdad y el decoder se configura ENTERO**: MEDIDO en varias corridas
`disp=7`, `ex6≈61–134` (entrada cargada), y la puerta render-pull
`[[obj+0x34]+0x60]` mide **`0x4` (!=0 = primed)**. El cebado del decoder **existe
y es alcanzable** — la premisa "el decoder está listo" de §11.13 se confirma.

**B) EL FIRMWARE EMITE EL TIPO-12 ÉL MISMO (refuta "nadie lo emite" de §11.13/§11.14).**
En cuanto el play progresa de verdad, MEDIDO sin shim (HLE off, nav fiable):
`h12 (0x08123970) = 1`, `gateconsume (0x081239bc, el str que consume la puerta,
sólo alcanzable en la rama de decode) = 1`, `dwork (0x08123bfc) = 3`. Es decir: el
retailOS **SÍ despacha el render-pull tipo-12** y **entra en la rama de decode**.
El `h12=0` de las 7 sesiones era un **artefacto del régimen B** (bajo
navegación floja el pipeline no llega ahí). El shim, con gate=`0x4`, también
dispara (traza "emitted type-12 ... from idle pc=0x22003b20"); pero su tipo-12 es
**redundante** con el que el firmware ya emite.

**C) EL MURO REAL: el tipo-12 es INSUFICIENTE — no sale PCM aunque se decode
(hallazgo decisivo).** Con el decode disparado (por el firmware o por el shim,
`h12=1`, `gateconsume=1`, `dwork=3`), MEDIDO con `memw=0x08888000-0x0888c000` (la
fuente PCM que lee el copy, §11.8): **8192 stores, TODOS del memset
(`0x22000198`/`0x220001a0`), CERO del decoder.** `copy32 (0x08016eb4) = 0`
invariante; `mixer (0x0809a74c) = 1` (sólo el render de bring-up, que salta por
`[r7+5]=0`). **El tipo-12 NO produce PCM.** El `dwork 0x08123bfc` NO decodifica
inline: **postea** un mensaje async (`0x8105c14` tipo `0x3a98` → `0x8105524`) al
puerto de decode `obj+0x54`; los 3 posts se acumulan pero **la tarea de decode
AAC aguas abajo nunca produce PCM**. El muro está **un nivel más abajo del
tipo-12**, no en el tipo-12.

**VEREDICTO HONESTO (esta sesión): el retailOS NO suena.** El `hle_out.wav`
capturado sigue siendo el **clic de UI** (L==R 100%, ~20k muestras no nulas en
ráfagas — no música). Lo aportado, todo MEDIDO:
1. **Shim HLE del tipo-12 implementado, gateado, seguro** (no rompe el build por
   defecto; `resets` observados = watchdog del propio retailOS ante la
   reproducción estancada, no del shim) y con su **mecanismo de entrega validado**
   (dispara el dispatcher desde el idle sin corromper tareas vivas).
2. **El decoder SÍ se ceba** (gate `0x4`) con navegación fiable — el cebado no era
   el muro.
3. **El tipo-12 NO es el muro**: el firmware lo emite solo (`h12=1`), el decode
   entra en su rama (`gateconsume=1`, `dwork=3`) y **aun así `copy32=0` y la
   fuente del mixer sólo recibe memset**. Corrige la hipótesis central de las 7
   sesiones ("falta el tipo-12"): faltaba **navegación fiable** para verla, y una
   vez vista, el tipo-12 resulta **presente pero insuficiente**.

**El muro exacto que queda (para la próxima sesión):** la **tarea de decode AAC
async** —receptora de los posts de `0x08123bfc` al puerto `obj+0x54`— **no corre
o no escribe PCM**. Es un tercer nivel de rendezvous, aguas abajo del tipo-12 que
las 7 sesiones creían el último. Vías: (a) localizar esa tarea (quién recibe en
`obj+0x54`, su TCB/estado en el deadlock) y ver en qué se bloquea; (b) el shim ya
tiene el mecanismo de entrega genérico (redirección desde idle a una función del
huésped) — reutilizarlo para cebar esa tarea, o el render del motor (mbox 0x52,
cuyo payload sigue DESCONOCIDO, §mensaje). El PCM del huésped no aparece porque el
productor real (decode AAC) está parado, no porque falte el tipo-12.

**Higiene:** `HLE_AUDIO_KICK` off por defecto → build byte-idéntico al baseline;
Rockbox/navtest/5G intactos (el shim es inerte sin la env). Recetas de sesión en
`~/dbg/` (`runhle_*.sh`, `cap_hle.py`), fuera del repo.

### 11.16 `obj+0x54` es un TIMER (no una tarea AAC); la caza de la "tarea async" era un fantasma; el muro es el disparo de SALIDA en frío — VEREDICTO cerrado por examen VIVO del decodificador real

Sesión de resolución. Objetivo del workflow: "cazar la tarea AAC aguas abajo que
recibe los posts a `obj+0x54` y no produce PCM" (§11.15). Se **midió de nuevo con
navegación fiable** (4 capturas decode-confirmadas) y se **desensambló entera** la
cadena `dwork`. Resultado: la premisa del workflow es una **misidentificación**.
Todo **VERIFICADO** por desensamblado directo (`~/6g/osos.dis`, VA = off +
`0x07FF51B8`) o lectura gdb no invasiva de un play (disco `Emulti.img`,
`AUDIOPACE=1`, Shuffle Songs, SELECT fiable). Recetas nuevas en `~/dbg/`:
`runmeas.sh`, `runtrace.sh`, `runt2aexam.sh`, `t2a_examine.py`. Árbol
byte-idéntico al baseline (sólo lecturas; cero cambios de fuentes).

**A) `obj+0x54` es un TEMPORIZADOR de 15 s, no un canal de decode — VERIFICADO por desensamblado (confirma Report-1; REFUTA §11.15-C y la interpretación "actor drainer" de Report-2).**
`dwork 0x08123bfc` hace: `r1=0x3a98; r0=obj+0x54; bl 0x08105c14; b 0x08105524`.
Desensamblado de los dos destinos:
- `0x08105c14 → 0x08051b94`: `str r5,[r4,#4]` ⇒ **`port[+4] = 0x3a98`** (el
  INTERVALO, no un "tipo de mensaje").
- `0x08105524`: pone `port[+0x20]="run "` (`0x72756e20`) → `0x080578fc`, que:
  `NOW = 0x08033a40()`; `deadline = NOW + interval·125·8 = NOW + interval·1000`;
  `str deadline,[port+8]`; **inserta `port` en una lista enlazada ORDENADA POR
  DEADLINE** (cabecera en el bloque de control `0x08911488`; lazo de inserción
  `0x0805795c` que compara deadlines con `0x0826480c`); `port[+0x1c]=1` (encolado);
  y **señala el semáforo de la rueda** (`0x0806a8ac`, sema `0x08970050`).
Esto es una **inserción de rueda de temporizadores de libro**. `0x3a98 = 15000`
= intervalo en ms = **15 s**. `dwork` **ARMA un watchdog de 15 s**; no postea
ningún mensaje de decode. La "tarea que recibe en `obj+0x54`" es **t04**, el
**servicio de la rueda** (entry `0x080833d0`, resumePC `0x22004394` op1b-WAITsema,
parkeada en sema `0x08970050`). **La tarea AAC async que este workflow fue creado
para cazar NO EXISTE** — `obj+0x54`/`0x3a98` es un falso rastro. (§11.15-C y
Report-2 malinterpretaron el arme del watchdog como un post async; Report-1
acertó.)

**B) La contradicción h12=0 (Report-1) vs h12=1 (§11.15) = la carrera regime-A/B MÁS el watchdog de 15 s — RESUELTA por medida.**
Con navegación fiable, MEDIDO (`runmeas.sh`, `pctrace pc=`/`sendex=`):
- **Corrida A (régimen A):** `disp=7, h12=1, gateconsume=1, dwork=3, ex6=57`,
  `copy32=0`. La **traza temporal** revela lo decisivo: setup a `+74.7 s` (disp
  #1-2, dwork #1-2 arman watchdogs) y un primer render-pull tipo-12 con **gate=0**
  (`h12else`), luego un **STALL de 15.13 s EXACTOS**, y a `+89.87 s` **el watchdog
  vence** → ráfaga (disp #3-6, `h12 #1` con gate ya puesto, `gateconsume`, dwork
  #3) → el motor corre su bring-up (`engine=1, mixer=1`) a `+90.2 s` → **`copy32=0`**.
- **Corrida B (régimen B):** `disp=6, h12=0, dwork=2, ex6=87`, `copy32=0`, y el tag
  del timer pasa a `"stop"` (`0x73746f70`, teardown).
⇒ **`h12=1` SÍ se alcanza (§11.15 acertó), pero SÓLO tras el stall+watchdog de
15 s**; las capturas de Report-1 (`<15 s`, como el propio Report-1 anotó) medían
ANTES del vencimiento → `h12=0`. Y **el resultado es INVARIANTE en ambos
regímenes: `copy32=0`, cero PCM.** La carrera no cambia el desenlace. El watchdog
de 15 s (y su teardown) SON el mecanismo de **recuperación-de-estancamiento del
propio retailOS**: su existencia y disparo **prueban que el pipeline no arranca en
frío** — el empujón inmediato de la primera trama que el HW real provee está
ausente, y ni siquiera el watchdog cierra el lazo.

**C) PRIMER examen VIVO del decodificador AAC REAL (t2a) y del consumidor/conductor — deadlock de rendezvous PURO, confirmado en la propia tarea (`t2a_examine.py`).**
Snapshot pre-watchdog (régimen A, gate primado): `decoder_obj=0x0ba9a8a4`
(vtable `0x088d1948`), `ctx=[obj+0x34]=0x0ba90c50`, **gate `[ctx+0x60]=0x4`
(primado)**. Estado de las tres tareas del lazo (todas worker entry `0x080833d0`):
- **t2a (DECODER AAC):** `st=08`, resumePC `0x22004028` = **op14-RECVmbox en
  buzón 9 (ENTRADA)**. Cadena de retorno `0x08025b04 → 0x081232a4 (dstep) →
  0x08123da8 (tipo-19) → 0x08123e20`: está **dentro de `dstep 0x08123284`**,
  habiendo pedido entrada por exchange 6, **bloqueado esperando el chunk en
  buzón 9**. NUNCA llega al decode por-trama.
- **t25 (MOTOR de audio):** `st=08`, op14-RECVmbox en **buzón 0x52 (comando de
  render)**, cadena `0x0805c578 → 0x080b3e80 (recv con timeout 3 s) → 0x22006824`.
  **Nadie le manda jamás el comando de render.**
- **t20 (StreamCache):** `st=40`, op4-RECVexch en **exchange 6** (sirviendo las
  peticiones de entrada del decoder), cadena `0x08171500 → 0x081f39a4 → 0x081d0a2c`.
Las TRES en rendezvous inter-tarea; **NINGUNA polleando hardware**; gate primado;
`copy32=0`. Snapshot **post-watchdog**: t2a y t25 pasan a `st=00` (**desmontadas**)
y los buzones 9/0x52 a cero — el teardown del watchdog. Es **exactamente** el
deadlock de §11.12/§11.13, ahora confirmado EN la propia tarea decodificadora.

**D) El decode SÍ hace trabajo de códec, pero NUNCA escribe PCM a su buffer de salida — VERIFICADO (`runtrace.sh` range + §11.9).**
`range=0x08080000-0x080b0000` disparado en setup + watchdog: **672 bloques
distintos de códec** corren (región `0x080axxxx`: bring-up del motor + `ParseASC
0x080a3cd0`, config). Pero la fuente del copy `0x08889098` (región
`0x08888000-0x0888c000`) sigue recibiendo **sólo el memset** (§11.9/§11.15) y
`copy32=0`. El motor rinde su bring-up y **su copy SALTA** (`[descr+5]=0`,
"PCM no listo"). El decode-a-PCM por-trama (que escribiría `0x08889098` y pondría
la bandera de trama-lista) **nunca se dispara**, porque está gateado tras el
viaje-redondo de render que está en deadlock.

**E) Determinación de ARREGLO DE MODELO: NINGUNO — cerrado por medida.**
El elemento que falta es **el comando de render en frío (buzón 0x52) al motor
t25**. Es un **mensaje inter-tarea**, no un evento de HW: las tres esperas del lazo
son rendezvous (buzón 9 / buzón 0x52 / exchange 6), ninguna es un poll de
periférico (confirmado VIVO en §C; coherente con `-d unimp`=sólo 43 writes GPIO
§11.11-B, IRQ16+tick vivos §11.12-D, salida idéntica a Rockbox §11.13-C). Por
tanto **ningún periférico, timer, valor de HW ni estado inicial puede satisfacer
esa espera** — coherente con las 8 sesiones. La carrera regime-A/B es
timing-sensible pero **el desenlace es invariante** (`copy32=0` en A y B), así que
tampoco un cambio de timing la desbloquea.

**F) La SALIDA WAV en régimen A lo confirma de forma independiente — es SÓLO el
clic de UI, cero música (`runwav.sh`, `AUDIOWAV`).** Captura de 72.6 s con decode
confirmado (`h12=1` a `+15.17 s` tras el watchdog): **`nonzero=0.25 %` (99.75 %
silencio), `L==R=100 %` (fake-mono, `rms(L-R)=0.0`), 13 ráfagas IDÉNTICAS de 624
muestras con pico 11444** (una por pulsación de SELECT en la navegación). Música
real sería `nonzero≈100 %`, `L==R≪100 %`, `rms(L-R)≫0`. No hay contenido con que
correlacionar el AAC. (Higiene: el fichero `~/6g/EVIDENCE_realaudio.wav` de una
sesión previa mide EXACTAMENTE esa firma —11444/624/L==R—, así que es **el clic mal
etiquetado como "realaudio"**, no prueba de audio. El discriminador fiable es
`copy32>0`/`memw` del decoder, no la amplitud del WAV.)

**VEREDICTO (medido, honesto): el retailOS NO suena. Deadlock de bootstrap
circular INTRÍNSECO del pipeline MeCCA, sin arreglo de modelo.** El productor
(t2a) y el consumidor (t25) se bloquean mutuamente en frío: el motor rinde PCM
⟸ comando de render (buzón 0x52) ⟸ la salida consumió PCM real ⟸ el motor rinde;
y el decoder produce la trama 0 ⟸ viaje-redondo de render ⟸ el motor rinde. Ninguna
mitad está cebada. La entrada está sana (ex6=57–87, t20 sirviendo), el decoder
primado (gate=0x4), y aun así `copy32=0` de punta a punta. El watchdog de 15 s del
propio firmware es la prueba de que el arranque en frío no cierra.

**Correcciones al registro que aporta esta sesión (todas VERIFICADAS):**
1. `obj+0x54` = **watchdog de 15 s** (desensamblado de la rueda), no un canal de
   decode → **la "tarea AAC async" del §11.15 es un fantasma**; el frente de este
   workflow descansaba en una misidentificación.
2. `h12=1` **es alcanzable** (§11.15 acertó) pero **sólo vía el watchdog de 15 s**;
   `h12=0` de Report-1 era medir antes del vencimiento. `copy32=0` invariante.
3. **Primer examen vivo de t2a** (el decoder real): bloqueado en la entrada
   (buzón 9), nunca decodifica; el muro está en el **disparo de SALIDA** (buzón
   0x52 al motor), no en `obj+0x54`.

**La vía real (no es de modelo, documentada):** un **HLE del cebado en frío** que
emita el **primer comando de render (buzón 0x52) al motor t25** para arrancar al
consumidor —que tirará de la trama (render-pull tipo-12) y hará que el decoder del
HUÉSPED produzca el PCM—. Es MÁS que el shim tipo-12 de §11.15 (que ahora se
demuestra insuficiente: h12 sólo consume el gate y **arma el watchdog**, no
decodifica): hay que cebar el LADO DEL MOTOR, y la vinculación del buffer de salida
(descriptor `0x0bb12d54`, bandera `[descr+5]`) debe salir del render propio del
huésped. Alternativa: `icount` (QEMU 9.x) para reordenar el scheduling y explorar
si el orden concurrente real difiere — con la salvedad del auditor de que el
determinismo por sí solo no hace aparecer al emisor ausente del buzón 0x52.

**Higiene:** cero cambios en fuentes del modelo; `git diff` vacío; build
byte-idéntico. Sólo lecturas gdb no invasivas + `pctrace` (ya en árbol).
Rockbox/navtest/5G sin tocar. Recetas de sesión en `~/dbg/` (`runmeas.sh`,
`runtrace.sh`, `runt2aexam.sh`, `t2a_examine.py`), fuera del repo.

### 11.17 El shim del TIMBRE de render (`HLE_RENDER_KICK`): IMPLEMENTADO y su MECANISMO VALIDADO, pero el motor es un CONSUMIDOR guiado-por-trabajo — tocar su timbre NO produce render sin PCM decodificado (refuta la hipótesis "cebar el buzón 0x52")

Sesión de **implementación** del cebado del LADO DEL MOTOR que §11.16 dejó
especificado (y que las dos investigaciones de cierre — formato del buzón 0x52 +
la cascada a PCM — detallaron). Se construyó el disparo del **timbre de render**
al motor t25 y se midió su efecto con navegación FIABLE. VA = off + `0x07FF51B8`;
IRAM en `0x22xxxxxx`. Todo **VERIFICADO** por `pctrace` (contadores `pc=`/`sendex=`
+ `memw`), la traza del propio shim, y snapshot gdb no invasivo. Disco
`Emulti.img`, `AUDIOPACE=1`, SELECT fiable (`iremote press ret -u -w e --region
right -r 15`).

**Corrección de marco que confirma la investigación (leer primero).** El "buzón
0x52" NO es un buzón KS con payload: el motor t25 se bloquea en `recv()` sobre un
**fast-semaphore SIN payload** `0x220106d0` (el "timbre"). VERIFICADO: el wrapper
de recv de t25 `0x22006814` carga `r0=0x220106d0, r1=0xbb8` (3 s) y llama a
`0x22003868`. El engobj es `0x220106dc` (= ring+12; `[engobj+0]=vtable 0x22008980`
puesto por el ctor `0x220068e4`). La "orden de render" es: armar la puerta
`[engobj+71]=1` (el motor sale por `beq 0x8082020` si es 0, `0x808171c` `ldrb
r0,[engobj+71]`) + tocar el timbre (V() del fast-sema). El emisor propio del
firmware es el stub IRAM `0x2200683c` (arma `[engobj+71]=1`, limpia `[+68]/[+70]/
[+72]`, incrementa `0x22008c1c`, señala) — pero su precondición `[engobj+70]!=0`
está **falsa en frío** (`bxeq lr` @`0x2200684c`) y tiene **0 llamadores**: por eso
NADIE toca el timbre en frío. (Confirma §11.16-C: t25 esperando el disparo de
salida.)

**A) Lo implementado (GATEADO, build por defecto byte-idéntico).**
`hw/arm/ipod_classic/ipod_classic_hle_audio.c` (+`.h`), cableado en la máquina y
disparado por el bit *run* del I2S. Con `HLE_RENDER_KICK` sin poner **no instala
nada** (init retorna antes de tocar estado; el hook del I2S es no-op). Con
`HLE_RENDER_KICK=1`: un timer de reloj virtual, cuando t25 está encolado en el
timbre (`[0x220106d0+4]!=0`, waiter presente) **y** la CPU está en el idle loop
RTXC (`pc∈[0x22003b0c,0x22003b34)` o `halted` en WFI), redirige el contexto idle
a: (1) escribir `[0x220106dc+71]=1` (la puerta de render, como el emisor propio),
y (2) llamar al **stub propio del huésped `0x22006870`** (`r0=0x220106d0; r1=0; b
0x22003670→V() 0x080a1858`), con LR de vuelta al idle. Cero PCM inyectado; se
toca el timbre EXACTO que el HW tocaría, y el desencolar+despertar los hace el
propio KS del huésped bajo sus locks. Reutiliza el mecanismo de entrega-desde-idle
validado en §11.15 (commit `dd068aa1e`), apuntando al MOTOR en vez del decoder.

**B) EL MECANISMO FUNCIONA — VERIFICADO.** Traza del shim: "rang render doorbell
#N to engine t25 (ring=0x220106d0 waiter=0x0bb1306c [engobj+71]=1) from idle
pc=0x22003b20". El timbre se toca desde el idle (pc=`0x22003b20`, exactamente el
pc del snapshot de §11.16), **9 791–11 244 veces** por corrida, despertando el
recv de t25 (waiter `0x0bb1306c` = nodo en la pila de t25, el mismo de la
investigación). Ninguna tarea viva se perturba (`resets`=1–2 = el watchdog de 15 s
del propio retailOS, idéntico a baseline). El snapshot vivo confirma con el shim
activo: `[engobj+71]=01` (armado por el shim), t25 `state=40` parkeada en el timbre
(RET `…0805c558 080b3e80 22006824 22002f84`), decoder primado (`gate[ctx+0x60]=
0x4`).

**C) HALLAZGO DECISIVO: el timbre NO produce render — el motor es un CONSUMIDOR
guiado-por-TRABAJO, no por el timbre.** Instrumentado el camino de t25 tras el
wake (MEDIDO, corrida v2):
```
recv    @0x22006814  hits=11244     <- t25 despierta del timbre 11244 veces
recvret @0x22006824  hits=11243
dpump   @0x22006118  hits=1         <- el data-pump (render) corre UNA vez (bring-up)
dpeng   @0x22006300  hits=1  dpbail=0  dpalt=0
engine  @0x0808170c  hits=1     mixer @0x0809a74c  hits=1     copy32 @0x08016eb4  hits=0
```
El bucle de t25 es `0x22002f5c`: `bl 0x22005e48` (@`0x22002f80`, ret `0x22002f84`)
**sondea la FUENTE** — `0x22005e48` es un dispatch virtual `ldr r0,[r0]; ldr
r1,[r0]; ldr r1,[r1,#28]; bx r1` (llama al método `+28` del objeto fuente). Si la
fuente no tiene datos, devuelve 0 y t25 **vuelve al recv** (`0x22002f88 beq`). El
timbre despierta al consumidor, pero **no puede fabricar trabajo que no existe**:
la fuente (PCM decodificado) está vacía, así que t25 hace wake→sondea→0→re-park,
11 244 veces, **sin renderizar ni una trama más que el baseline**. `copy32=0`
invariante; `memw=0x08888000-0x0888c000` = **8192 stores, TODOS memset**
(`0x22000198`/`0x220001a0`), CERO del decoder. Idéntico a §11.16.

**D) El engobj está SEMI-configurado — VERIFICADO (snapshot).** `[engobj+88]=0xff`
(el TIPO de render sigue en el valor de init del ctor `0x220068e4`; el `switch` del
data-pump `0x220062a0` y del motor operan sobre él — nunca se configuró a un tipo
real). `[engobj+70]=0` (la puerta de arranque en frío del emisor propio, nunca
satisfecha). `[engobj+68]=0`. El motor nunca completó su configuración de render;
el timbre por sí solo no la completa.

**E) EL MODELO CAUSAL CORREGIDO (refuta la hipótesis de la tarea y de 8 sesiones).**
La cadena que la orden de trabajo asumió —"el motor renderiza → emite el tipo-12
al decoder → el decoder produce PCM"— es **FALSA y ahora está REFUTADA por medida**:
el render es el lado **CONSUMIDOR** (`motor 0x0808170c → mixer 0x0809a74c →
copy 0x08016eb4 → ping-pong`), NO emite el tipo-12 al decoder (ver la
investigación "cascada"); el tipo-12 al decoder va por vía separada. Cebar el
timbre del motor **arranca al consumidor, pero el consumidor no tiene nada que
consumir** porque el PRODUCTOR (decoder t2a) está bloqueado en su ENTRADA (buzón
9, RECVmbox dentro de `dstep 0x08123284`) y nunca produce PCM (§11.16-C). Es el
**deadlock de bootstrap circular** de §11.16, ahora confirmado DINÁMICAMENTE desde
el lado del motor: despertar al consumidor 11 244 veces no genera ni un render
extra.

**VEREDICTO (medido, honesto): el retailOS NO suena.** El timbre de render (buzón
0x52) es **necesario pero INSUFICIENTE**. Hasta dónde llegó el kick, con evidencia:
1. **¿El motor renderizó?** NO más que el baseline: `engine=1` (sólo el bring-up
   del watchdog), pese a 11 244 toques del timbre. El motor despierta pero su
   bucle sondea la fuente, la halla vacía y re-parkea.
2. **¿El decoder decodeó?** NO: `h12=1` sólo tras el watchdog (como baseline),
   `copy32=0`, `memw`=sólo memset. t2a sigue bloqueado en la entrada (buzón 9).
3. **¿`copy32` subió de 0?** NO: `copy32=0` invariante (A y B). El WAV capturado
   es SÓLO el clic de UI (`nonzero=0.26 %`, `L==R` 100 %, pico 11444) — no música,
   nada con que correlacionar el AAC.

**Lo que FALTA (el muro real, un nivel más abajo del timbre):** que el PRODUCTOR
(decoder t2a) consuma su entrada (buzón 9) y decodifique la trama 0, poniendo PCM
en la fuente del copy (`0x08888xxx`). Sólo entonces el bucle de t25 (ya despertado
por el timbre) hallará trabajo y renderizará. El timbre del motor es el segundo
eslabón; el primero (entrada del decoder) sigue ausente. Coherente con el
"deadlock circular intrínseco, sin arreglo de modelo" de §11.16 (las tres esperas
son rendezvous inter-tarea, ninguna es poll de HW).

**Decisión sobre el shim: REVERTIDO (mudo), finding conservado.** El shim es
limpio, gateado y default-inerte (build por defecto byte-idéntico; navtest 6G y
Rockbox sin cambios con la env sin poner), y su **mecanismo de entrega está
validado** — pero **no produce audio** (el timbre despierta al consumidor sin
trabajo). Igual que el shim tipo-12 de §11.15 (revertido en `6e9f4a9c7`), se
revierte la fuente y se conserva SÓLO esta documentación medida. Reproducible: es
`dd068aa1e` reapuntado al timbre (`pc=0x22006870`, previo `[0x220106dc+71]=1`),
gateado por `HLE_RENDER_KICK`.

**Direcciones nuevas VERIFICADAS esta sesión (disasm + medida):**
- t25 bucle de render `0x22002f5c`; sondeo de trabajo `bl 0x22005e48` @`0x22002f80`
  (ret `0x22002f84`); `0x22005e48` = dispatch virtual `bx [[[fuente]]+28]` (sondea
  la fuente, devuelve 0 si vacía).
- recv del timbre `0x22006814` = `recv(0x220106d0, 3000ms)` vía `0x22003868`.
- data-pump `0x22006118`; llamada al motor `bl 0x22003840`(→`0x0808170c`)
  @`0x22006300`; limpia la puerta `strb #0,[engobj+71]` @`0x22006308`.
- puerta de entrada del motor: `0x8081718 bl 0x8015860` (getter engobj) →
  `0x808171c ldrb r0,[engobj+71]` → `0x8081730 beq 0x8082020` (sale si 0).
- stub de timbre `0x22006870` (`r0=0x220106d0; r1=0; b 0x22003670`→V()
  `0x080a1858`, veneer que no toca LR); emisores propios `0x220060e0` /
  `0x2200683c` (0 llamadores en frío).
- ctor del engobj `0x220068e4`: `[engobj+0]=vtable 0x22008980`, `[engobj+88]=0xff`.

**Higiene:** `HLE_RENDER_KICK` off por defecto → build byte-idéntico; shim
REVERTIDO tras medir (sólo esta doc se commitea). Rockbox/navtest 6G/5G intactos
(el shim era inerte sin la env; el árbol final es baseline). Recetas de sesión en
`~/dbg/` (`run_rk.sh`, `run_rk2.sh`, `run_rksnap.sh`, `rk_probe.py`,
`rk_probe2.py`), fuera del repo.

### 11.18 El DOBLE KICK (`HLE_DUAL_KICK`): el decode del HUÉSPED SÍ corre por primera vez, pero productor y consumidor están DESLIGADOS en frío — deadlock simétrico confirmado a nivel de instrucción, sin arreglo de kick

Sesión de **implementación** del doble kick que las dos investigaciones de cierre
(cebado del decoder + orden/timing) especificaron: cebar el DECODER (que produzca
la trama 0) **y** el MOTOR (que la consuma), a la vez / decoder-primero, para
romper el deadlock simétrico. Se construyó, se **corrigió el punto de entrega del
decoder** a nivel de instrucción, y se **midió** con navegación FIABLE. VA = off +
`0x07FF51B8`; IRAM en `0x22xxxxxx`. Todo **VERIFICADO** por desensamblado directo
(`~/6g/osos.dis`/`iram.dis`), snapshot gdb no invasivo de un play navegado fiable, y
contadores `pctrace` (`pc=`/`memw=`/`memwpc=`). Disco `Emulti.img`, `AUDIOPACE=1`,
SELECT fiable. Recetas nuevas en `~/dbg/` (`run_dk.sh`, `run_dksnap.sh`,
`run_dk_snap2.sh`, `dk_probe.py`, `dk_probe2.py`); shim en `~/dbg/hle_dual_kick_shim/`.

**Corrección VERIFICADA del punto de cebado del decoder (a nivel de instrucción).**
El dispatcher `0x08123dcc` conmuta por `[msg+4]`: **12**→watchdog `0x8123970`
(§11.16, confirmado); **19**→`blx [[msg]+20](msg)` y luego `switch 0x8123cfc(obj,
[retval+8])`; default→`0x80eaebc`. El `switch 0x8123cfc` conmuta por
`[[retval+8]+4]-0x52800000` (10 casos): **caso 1** (`0x52800001`, `0x8123d64`) →
`bl 0x81232d8` = **DECODE-de-trama**; **caso 7** (`0x52800007`, vía `0x81235d0`) →
`bl dstep 0x08123284` → `bl 0x8025ac4` (pide entrada por ex6 + **RECV mbox 9**, DONDE
BLOQUEA). El decoder vivo está procesando un caso-7 (cadena de retorno medida
`0x08025b04→0x081232a4(dstep)→0x081236c8(caso7)→0x08123da8(switch)→0x08123e20(disp)`).
El DECODE real cuelga del caso-1 `0x81232d8`: en `0x8123390` hace
`blx [[ctx]+104]`=`0x081c0a1c` (decode-method) → `0x081c1508` (frame-pull) →
`reader->vtable[16]`=`0x081e93d8` (pull) → bucle sobre `0x081e9e70` (la math del
códec por-bloque). Alcanzable sólo si `ctx=[obj+0x34]!=0`, `[obj+0x84]==0`,
`[obj+0x38]==0` — **los tres se cumplen en vivo** (snapshot: `ctx=0x0ba90c50`,
`gate[ctx+0x60]=0x4`).

**El shim (GATEADO, build por defecto byte-idéntico).** `HLE_DUAL_KICK=1`: un timer
de reloj virtual, desde el idle RTXC (`pc∈[0x22003b0c,0x22003b34)`, sin pisar tareas
vivas), **alterna decoder-primero**: (1) DECODER = llamar la caso-1 propia del huésped
`0x081232d8(r0=decoder_obj, r1=0)` — decodifica la trama **y** la publica
(`ctx->vtable[100]=0x081c12ac` + ops de mensaje); `r1=0` hace que el post-decode
`0x81235d0` vea `r7==0` y SALTE el dstep de entrada (que haría RECV mbox 9), así **no
bloquea** — VERIFICADO que decode-method/frame-pull/pull/publish no tienen KS-wait; el
lock `0x805c430` de `ctx+80` está descontendido (t2a aparcado en el verbo de entrada,
no lo tiene). (2) MOTOR = `[engobj+71]=1` + timbre `0x22006870` (§11.17). Se gatea el
motor a que el decoder ya esté construido (nunca timbrar en frío durante el boot).
Cero PCM inyectado. Knobs: `_DEC_ONLY`, `_MOTOR_ONLY`, `_CFG` (fuerza `+88=1/+70=1`),
`_DEC_MAX`, `_TRACE`.

**A) HALLAZGO POSITIVO: el DECODE del huésped SÍ CORRE — primera vez en 11 sesiones.**
Con el doble kick (decoder-primero, caso-1 decode+publish), MEDIDO:
`decmeth 0x081c0a1c = 4192`, `fpull 0x081c1508 = 4192`, `rpull 0x081e93d8 = 4193`,
**`innerdec 0x081e9e70 = 1`**. Es decir: el kick alcanza y ejecuta la **math AAC del
huésped** (la que §11.9-§11.16 midieron como "nunca corre": `range` sólo veía
`ParseASC`=config). El mecanismo de entrega decoder-primero funciona y llega al decode.

**B) PERO el decode corre UNA sola vez — la puerta de habilitación de producción está a 0.**
`innerdec=1` pese a 4192 llamadas a caso-1. El `rpull 0x081e93d8` sale temprano
(`0x81e93f8 beq`) porque su puerta **`[[reader+0x30]+0x2b8]` = 0** (snapshot baseline:
`reader=0x0ba61f50`, `[reader+4]ring=ctx`, `decode-gate[[+48]+0x2b8]=0x00000000`). Esa
puerta es el crédito "puedes producir una trama" que gobierna **StreamCache**; a 0, el
decoder no puede producir la trama 1. Es una hambruna de **slot de salida**, no de datos.

**C) copy32 = 0: la trama publicada NUNCA llega al motor — productor y consumidor DESLIGADOS.**
Aun forzando la config del motor (`_CFG`: `[engobj+88]=1`, `[engobj+70]=1`), MEDIDO:
`copy32 0x08016eb4 = 0`; el motor t25 despierta del timbre, **sondea su fuente
`srcpoll 0x22005e48 = 19×` y la halla SIEMPRE VACÍA**; `dpump 0x22006118 = 1` (sólo el
bring-up), `dpgateclr 0x22006308 = 0` (nunca cierra un ciclo de render). El descriptor
de copia del motor **`0x0bb12d54 = 5da35da3 5da35da3…` (relleno de heap SIN
inicializar; `[descr+5]=0x5d≠1`)** — la vinculación fuente↔copia que el propio render
inicializaría NO existe en frío. Y `memw 0x08888000-0x0888c000` = **sólo memset**
(`0x22000198`/`0x220001a0`), CERO escrituras del decoder a la fuente del mixer. La trama
que el decode produjo/publicó quedó en el buffer interno del reader; **no hay camino
cableado a la fuente (basura) del motor**. `CODEC_SETUP 0x22006574` corrió (`hits=1`)
pero su config de render está ausente en el atasco (`[engobj+88]=0xff`, `+71=0`, `+70=0`
= valores del ctor `0x220068e4`; espejismo post-teardown de §11.12-A/§11.13-A).

**D) El deadlock simétrico, ahora a nivel de instrucción (VERIFICADO).** Cada mitad
está gateada por la salida en régimen del OTRO:
- Productor: `decode` ⟸ puerta `[[reader+0x30]+0x2b8]!=0` ⟸ StreamCache da crédito ⟸
  buffer de salida liberado ⟸ el motor renderiza (`copy32`) ⟸ PCM en la fuente ⟸ decode.
- Consumidor: `copy32` ⟸ `[descr+5]==1` + descriptor `0x0bb12d54` inicializado ⟸ el
  render propio del motor ⟸ fuente con PCM ⟸ el decode publicó ⟸ productor habilitado.
Las tres esperas del lazo son rendezvous inter-tarea (t2a RECV mbox 9 dentro del verbo
de entrada; t25 RECV en el timbre; t20 RECV ex6), **ninguna es poll de HW** (coherente
con §11.12-D/§11.16). Cebar UN extremo desde el idle da **un paso transitorio** (1
decode; 1 wake del motor), nunca un ciclo autosostenido, porque los recursos
compartidos (crédito de producción, descriptor de salida inicializado) sólo los produce
el pipeline en marcha, no un timbre ni una llamada-a-decode.

**VEREDICTO (medido, honesto): el retailOS NO suena con el doble kick.** Hasta dónde
llegó cada lado, con evidencia:
1. **¿El decoder decodeó?** SÍ — la math AAC del huésped (`innerdec 0x081e9e70`)
   **ejecutó por primera vez** (todas las sesiones previas: 0). Pero **UNA sola trama**:
   la puerta de producción `[[reader+0x30]+0x2b8]` está a 0 (StreamCache no da crédito).
2. **¿Escribió PCM en `0x08888xxx`?** NO — la fuente del mixer recibió sólo memset; el
   decode escribió en el buffer interno del reader y la publicación quedó desligada de la
   fuente (basura) del motor.
3. **¿`copy32` subió de 0?** NO — invariante. El motor sondea su fuente vacía (19×) y no
   renderiza; su descriptor de copia `0x0bb12d54` está sin inicializar. Forzar `+88/+70`
   no ayuda (el problema es la VINCULACIÓN de la fuente, no el tipo).
4. **¿Autosostenido?** NO. WAV = clic de UI (`nonzero=0.22 %`, `L==R`, pico 11444;
   §11.16-F), nada que correlacionar con el AAC.

**Es el VEREDICTO ESTRUCTURAL firme:** el doble kick avanza el estado **más que las 10
sesiones previas** (el decode del huésped corre), pero el deadlock de bootstrap
**simétrico** no se abre: las puertas de cebado de cada mitad
(`[[reader+0x30]+0x2b8]` en el productor; `[descr+5]` + descriptor `0x0bb12d54` sin
init en el consumidor) las alimenta el estado-en-régimen de la OTRA mitad, y ningún kick
(timbre/decode-call/config) desde el idle las fabrica. Lo único que quedaría es un **HLE
COMPLETO del camino de datos** decoder↔motor: sintetizar el flujo de tramas y los
créditos/descriptores de buffer — lo que es **inyectar PCM/falsear estado**, prohibido
por las reglas (el PCM debe producirlo el decoder del huésped y fluir por su propio
protocolo). No hay palanca de kick, ni valor de HW, ni orden/timing que cierre el lazo.

**Direcciones nuevas VERIFICADAS esta sesión (disasm + snapshot vivo):**
- dispatcher `0x08123dcc` (tipo-12→watchdog `0x8123970`; tipo-19→`blx [[msg]+20]` +
  `switch 0x8123cfc`); `switch 0x8123cfc` conmuta `[[retval+8]+4]-0x52800000`: **caso 1
  → `0x81232d8` (decode)**, caso 7 → `0x81235d0` → dstep.
- decode-de-trama caso-1 `0x081232d8` (gate: `ctx=[obj+0x34]!=0`, `[obj+0x84]==0`,
  `[obj+0x38]==0`); decode-method `[[ctx]+104]=0x081c0a1c`; frame-pull `0x081c1508`
  (`reader=[ctx+0x48]`, pull=`reader->vtable[16]`); reader-pull `0x081e93d8` (puerta
  `[[reader+0x30]+0x2b8]`, ring=`[reader+4]`); **inner-decode/math AAC `0x081e9e70`**;
  publish `ctx->vtable[100]=0x081c12ac` (no bloqueante).
- verbo de entrada `0x8025ac4`: SELECT-mbox9 `0x8015730→0x22003f7c`, SEND-ex6
  `0x8015740→0x22004138`, **RECV-mbox9 `0x8015738→0x22004260`** (bloqueo). El verbo NO
  lee la respuesta: el RECV es puro handshake.
- snapshot vivo (atasco, baseline): `decoder_obj=0x0ba9a8a4` vt=`0x088d1948`
  ctx=`0x0ba90c50` vt=`0x088daa4c`; reader vt=`0x088de828`; **`decode-gate[[reader+0x30]
  +0x2b8]=0`**; **descriptor motor `0x0bb12d54=0x5da35da3` (sin init)**; `engobj+88=0xff`.
- motor: `srcpoll 0x22005e48` (`bx [[[src]]+28]`), `dpump 0x22006118`, `dpgateclr
  0x22006308`, `CODEC_SETUP 0x22006574` (corre `hits=1`, config torn-down en el atasco).

**Correcciones al registro (VERIFICADAS):**
1. **REFINA "cebar-decoder" (report decoder-in):** el decode NO es inalcanzable — la
   caso-1 `0x81232d8` desde el idle **sí ejecuta la math AAC del huésped** (`innerdec=1`).
   Lo que falta no es el disparo sino el **crédito de producción** (`[[reader+0x30]
   +0x2b8]=0`) que gobierna StreamCache.
2. **REFINA "orden/timing" (report orden):** el motor NO sólo necesita el timbre; su
   **fuente/descriptor `0x0bb12d54` está SIN INICIALIZAR** (no sólo `+88=0xff`), y forzar
   `+88=1/+70=1` no rinde (`copy32=0`). La vinculación productor→consumidor no existe en
   frío; no es un campo que forzar sino un binding de render que sólo el render crea.
3. **CODEC_SETUP `0x22006574` SÍ corre** (`hits=1`), refinando la duda de §11.17-D: su
   config de render se ve `0xff`/`0` en el atasco por **teardown**, no por no-ejecución.

**Decisión sobre el shim: REVERTIDO (mudo), finding conservado (regla §11.17).** El shim
es limpio, gateado y default-inerte (build por defecto byte-idéntico; navtest 6G/5G y
Rockbox sin cambios con la env sin poner), y su **mecanismo de entrega del doble kick
está validado** (decoder-primero desde el idle, decode+publish no bloqueante) — pero **no
produce audio** (el lazo simétrico no cierra). Se revierte la fuente y se conserva SÓLO
esta documentación medida. Reproducible: shim en `~/dbg/hle_dual_kick_shim/`, gateado por
`HLE_DUAL_KICK`.

**Higiene:** `HLE_DUAL_KICK`/`HLE_RENDER_KICK` off por defecto → build byte-idéntico;
shim REVERTIDO tras medir (sólo esta doc se commitea). Rockbox/navtest 6G/5G intactos
(el árbol final es baseline; navtest 6G y 5G corridos de verdad al cierre). Recetas de
sesión en `~/dbg/` (`run_dk.sh`, `run_dksnap.sh`, `dk_probe.py`, `dk_probe2.py`) y shim
en `~/dbg/hle_dual_kick_shim/`, fuera del repo.

### 11.19 El HLE del CAMINO DE DATOS (`HLE_AUDIO_DATAPATH`): el retailOS saca SU canción real por su propio I2S — decode host-side, como el VideoCore del 5G

Sesión de IMPLEMENTACIÓN de la vía que §11.18 dejó especificada. Como el pipeline
de decode MeCCA está en el deadlock de arranque simétrico cerrado por medida en
§11.12–11.18 (no se auto-ceba bajo emulación fiel), se decodifica **en el host** la
MISMA canción que el huésped lee de SU disco y se entrega el PCM al buffer que el
DMA/I2S PROPIO del huésped transporta al códec — exactamente el patrón HLE del
VideoCore del 5G (`hw/arm/ipod_video/ipod_video_vc.c`). Gateado por
`HLE_AUDIO_DATAPATH`; build por defecto byte-idéntico (Rockbox y navtest intactos).

**El shim (`hw/arm/ipod_classic/ipod_classic_hle_audio.c`, gateado).**
1. **Parsea la FAT del disco del huésped** (MBR → FAT32) y enumera los ficheros de
   audio. IDENTIFICA la pista por las PROPIAS lecturas ATA del huésped: el read
   DMA que cae en el rango de datos de un fichero (hook en `ipod_classic_ata.c`
   vía `ide_get_sector`). Así se decodifica la MISMA canción que el huésped abrió.
2. **Decodifica esa pista en el host con ffmpeg** → PCM 16-bit estéreo 44100
   (único paso host-side; lee la MISMA imagen cruda del `-drive`; corre en un hilo
   aparte para no congelar la vCPU).
3. **Alimenta el PCM al canal 1 de DMAC0** (el que drena la ping-pong a la FIFO
   del I2S): en `i2s_drain`, antes de cada ráfaga paced (`pl080_pace_request`),
   escribe el siguiente trozo en `ch->src`; el cursor de lectura avanza SÓLO por
   lo que la ráfaga consumió (`moved`=Δtx_samples), no por los ticks de sondeo —
   así la canción sigue el reloj real (fill/commit).

**Correcciones VERIFICADAS a la investigación previa (importantes).**
- **La canción del huésped NO es AAC.** Los 6 M4A/AAC que la investigación de
  ENTRADA carveó (offsets crudos 8.5–55.7 MB) están **FUERA de la partición FAT** y
  NO son lo que el huésped reproduce. La biblioteca real vive en la FAT32 (empieza
  en el byte **92962816**), en `/iPod_Control/Music/Fxx/`: **4 MP3** (48 kHz,
  `libgpodNNN.mp3`) **byte-idénticos** entre sí = **una sola canción**
  ("Numb / Encore"). El huésped saca a 44100 (CLKDIV 272) → se decodifica a 44100.
- **Disco de sectores lógicos de 4096 B (4Kn).** Las LBA del MBR van en unidades de
  4096 (part1 LBA 22696 × 4096 = 92962816 = boot sector FAT); el scanner prueba
  ×512 y ×4096 y valida la firma de boot.
- **El huésped SÍ lee los datos del MP3 al reproducir de verdad** (regime A), pero
  su reproducción es NO DETERMINISTA (§11.11 A/B): a veces lee el fichero (→ ID por
  lectura), a veces se atasca antes (regime B). Para robustez, un **fallback**
  decodifica la ÚNICA canción del disco (cuando la biblioteca es inequívoca: los 4
  ficheros idénticos) al detectar el arranque de reproducción — por el edge de
  CLKCON (reconfig de reloj del play, que el boot no hace) o por una lectura del
  área de datos FAT una vez pasado el boot (I2S ha sonado >~34 s, así que es el
  usuario abriendo un fichero, no el escaneo de arranque).

**Medida (navegación FIABLE, `AUDIOPACE=1`, `Emulti.img`).**
- ID + decode: p.ej. `identified guest track N (file stream, disk offset ...)`;
  `decoded track N -> 36400496 PCM bytes (206.4 s @ 44100 stereo) -- feeding`.
- **El feed es CONTIGUO y al ritmo real** (traza de drenado VERIFICADA): src alterna
  la ping-pong `0x22008c5c`/`0x22009c5c`, `count=moved=2048` por ráfaga, cursor
  `0→4096→8192→…` monótono.
- **Correlación WAV (capturado de la FIFO del I2S del huésped) vs el MP3 del
  disco: Pearson=1.000** en régimen, con el offset avanzando 1:1 con el tiempo
  (`wav+2s→song 1.62s`, `wav+4s→song 3.62s`); pico WAV 30420 ≈ pico del MP3 30653;
  estéreo real (RMS(L−R)≫0). Es la MISMA canción, contigua, en tono. Ruido de
  fondo del correlador ~0.02–0.05 (silencio del baseline). ⇒ **SUENA la canción
  real del retailOS, por su propio DMA→I2S→CS42L55.**

**El obstáculo exacto que QUEDA (honesto).** El motor de render del huésped sigue
en el deadlock: `copy32 (0x08016eb4)=0`; el motor `0x0808170c` corre UNA vez a los
~15 s (watchdog §11.16) y no renderiza. Consecuencias:
1. **La pantalla Now Playing NO avanza / no aparece:** la UI del huésped no
   transiciona porque su pipeline de reproducción está atascado; el shim entrega el
   audio AGUAS ABAJO de ese motor. El shim rompe el silencio, no el deadlock.
2. **Watchdog de reinicio:** sin más, el retailOS reinicia (`SWRCON=0xaa5`, escrito
   en `0x08322b24`, luego spin de nops en `0x08322b28`) ~15 s tras `StartPlayback`
   (el baseline sin HLE también: es el deadlock del huésped). Corta la canción.
   - **`HLE_AUDIO_SUSTAIN` (opt-in):** el modelo de reloj suprime SÓLO ese
     reinicio-watchdog (tag en blanco "----") mientras el HLE alimenta audio → la
     canción suena ENTERA por el DMA/I2S del huésped. Coste honesto: la CPU del
     huésped queda congelada en el spin `0x08322b28` (su intento de reproducir
     falló en el deadlock); el audio continúa porque el DMA es HW y el fill corre
     en el timer del host. Sin el flag, el huésped sigue su ciclo normal (suena
     ~15 s y su watchdog reinicia).

Cerrar el punto 1 (UI + que el motor del huésped renderice él mismo) es el muro de
§11.12–11.18 (cebar el pipeline MeCCA para `copy32>0`), no abierto aquí.

**Honestidad.** El PCM es la canción REAL del disco del huésped (correlaciona con
el mismo MP3 decodificado aparte); sale por el I2S PROPIO del huésped
(DMAC0 ch1 → FIFO 0x3ca00010 → CS42L55), no por un bypass del sumidero. Lo ÚNICO
host-side es DÓNDE se decodifica el audio comprimido — un HLE del camino de datos.
El huésped conduce la apertura del fichero y el arranque de reproducción; el shim
suple el PCM que su decoder atascado no produce, y (opcional) evita que su watchdog
de deadlock se auto-reinicie. La UI de Now Playing NO avanza (obstáculo anotado).

**Higiene / gating.** Todo bajo `HLE_AUDIO_DATAPATH` (`_TRACE`, `_TRACK`,
`_SUSTAIN`, `_DISK`); sin la env el shim no instala nada y la máquina es
byte-idéntica. Hooks: `ipod_classic.c` (init), `ipod_classic_i2s.c` (fill+commit,
edge de reloj), `ipod_classic_ata.c` (offset de lectura para ID),
`ipod_classic_clk.c` (supresión gateada del reinicio-watchdog). Regresiones:
navtest 6G y Rockbox (rbmusic) con la env sin poner — sin cambios.

### 11.20 Asalto nº12 (HLE_NP): plataforma propia; decode kickeado desde idle NO entrega trama; el muro es el CONTEXTO+ENTRADA del decoder, no dos campos

Sesión con módulo propio integrado `hw/arm/ipod_classic/ipod_classic_hle_np.c`
(gateado por `HLE_NP`, build por defecto byte-idéntico), rama `dsp-deadlock-break`.
Objetivo autorizado por el usuario: cebar los bindings de control (no PCM) para
que el motor del huésped renderice (`copy32>0`) y Now Playing avance. Todo
**VERIFICADO** por `pctrace`/`memw`/gdb no invasivo + disasm (`~/6g/osos.dis`,
VA = off + 0x07FF51B8). Disco `Emulti.img`, `AUDIOPACE=1`, SELECT fiable.

**A) La función copy 0x08016e44 decodificada (fuente y gate exactos).**
`src = [r5+12] + (([r4+4]-r6-[r5])<<2)`; `gate = [r7+5]==1`. Captura VIVA de sus
args en el render de bring-up: `r5=0x088890c0` → `[r5+12]=0x08889098` (buffer
fuente); `r7=0x0bb12eb4` → `[r7+5]=0` (gate cerrado); `r4=0x0bb12d54`. El copy
salta por gate=0 / fuente vacía. Fuente `0x08889098` = buffer ESTÁTICO en la
sección de datos del osos (< 0x08976f7c), no heap.

**B) El publish 0x081c12ac es POR MENSAJE, no por campo (clave, corrige el marco
de kicks previo).** Llama a `reader.vtable[3]` (get-frame) y, si devuelve !=0,
postea la trama downstream (`bl 0x832d9dc`); si 0, no postea. **NO escribe
`[r5+12]`/`[r7+5]` del motor.** Por eso tocar el timbre (§11.17) o el tipo-12
(§11.15) nunca cablea el binding: éste llega por el MENSAJE que el motor debe
recibir y procesar, no por el timbre.

**C) El crédito de producción = `[[reader+0x30]+0x2b8]` (fn 0x81c0fd4, dos instr:
`ldr r1,[r0+0x30]; ldr r0,[r1+0x2b8]`).** Snapshot vivo: reader=0x095562c8,
credit=0x02000000 (no-cero). El reader-pull 0x081e93d8 sólo hace `cmp r0,#0; beq`
sobre él.

**D) MEDIDO: el kick-desde-idle NO entrega trama.** Con `HLE_NP` disparando la
caso-1 `0x081232d8(decoder_obj,0)` desde el idle RTXC en régimen A (decoder
primado, `gate[ctx+0x60]=0x4`): breakpoint en el retorno de la get-frame del
publish (`0x081c12e8`) → **`r0=0` (sin trama)**. `memw` de la fuente estática
`0x08888000-0x0888c000` durante el play con el kick activo: **cero escritores no-
memset** (sólo `0x22000198/a0`). `copy32=0` invariante; WAV = sólo el clic de UI
(nonzero 0.3%, L==R 100%, pico 11444). El "innerdec 0x081e9e70" que §11.18 tomó
por "math AAC" es en realidad un **gestor de cola de trabajo** (0x8c bytes) —
correr él no implica producir PCM.

**E) VEREDICTO (medido, honesto): el kick-desde-idle es insuficiente por diseño.**
El decode real requiere el **contexto de la tarea t2a** (pila/estado) y su
**flujo de entrada** (RECV mbox 9 ← StreamCache), ambos en el deadlock; ejecutar
el código de decode desde el idle corre las instrucciones pero **no produce una
trama entregable** por el protocolo de mensajes. Cebar el gate del copy sin PCM
en la fuente no rinde. La única vía nativa restante es un **HLE COMPLETO del
decode** (sintetizar entrada+contexto+encolado por el protocolo MeCCA), de
tamaño equivalente a reimplementar la integración del códec — coherente con la
decisión de §11.19 de decodificar host-side aguas abajo.

**Aportes reutilizables (rama `dsp-deadlock-break`, commit del módulo):** copy
decodificado (A), publish=mensaje (B), credit fn (C), y una plataforma gateada
`HLE_NP` que dispara el decode del huésped a voluntad para futuros experimentos.
Recetas en `~/dbg/`: `run_np.sh`, `run_npsnap.sh`, `run_npmemw.sh`, `run_pub.sh`,
`np_snap.py`, `catch_copy.gdb`, `catch_publish.gdb`.

### 11.22 Asalto #12 (cont.): por qué la inyección nativa NO converge -- el guest no idlea durante el deadlock y t25 se desmonta antes de la ventana

Continuación exhaustiva del asalto #12 con la plataforma `HLE_NP`. Medido a nivel
de instrucción (pctrace `regpc`/`memw`, gdb no invasivo, disasm iram/osos). Cada
capa de inyección probada topa con la siguiente; el resumen del porqué:

**A) El copy 0x08016e44 (medido en el bring-up natural):** lee `count=[r4+16]`
bytes desde `[r5+12]+([r4+4]<<2)`. Valores: `[r5+12]=0x08889098`, `[r4+4]=0`,
`[r4+16]=0x28` (40 B), gate `[r7+5]`. El buffer 0x08889098 es un área de staging
de 40 B; el descriptor r5=0x088890c0 va justo detrás (base+0x28), así que una
ventana grande lo corrompe (corregido: llenar exactamente `count`).

**B) Descriptores dinámicos:** en el path NATURAL (t25) el copy usa el descriptor
de HEAP estable `r4=0x0bb12d54 r7=0x0bb12eb4` (pokeable); llamando al engine
0x0808170c DIRECTAMENTE usa descriptores de PILA transitorios (`0x0896xxxx`, bajan
0x3e0/render) que no se pueden pokear desde idle. El path natural corre UNA vez
(gateado por el contador de jobs `[r5+12]=[0x22008c1c]`, render-por-job 0x22005dec).

**C) El gate `[r7+5]` lo deriva el engine del estado-de-relleno del stream**
(0x088890c0: `[+0]=0 [+4]=0x08889088 [+8]=0xa [+c]buf=0x08889098 [+10]=0x004c1bf8
[+14]=0x580`), no del contenido del buffer. Ponerlo exige la semántica de esos
campos (otra capa).

**D) NO HAY VENTANA DE INYECCIÓN SEGURA (el hallazgo que cierra la vía de kick):**
- Durante el deadlock el guest **NO idlea**: los pc diferidos son bucles MeCCA
  vivos (`0x0813dd3c`, `0x081611f8`, `0x081e47fc`, `0x080c8bb0`, `0x0815bb54`).
  Inyectar desde idle **siempre difiere** (medido: deferred monótono, bind=0).
- Cuando el CPU llega a un punto seguro (el spin del reset suprimido por
  `HLE_AUDIO_SUSTAIN`, 0x08322b28), **t25 ya se desmontó** (recv-timeout de 3 s →
  `st=00`, `waiter=0`): el timbre no tiene destino.
- Sin SUSTAIN, el guest se resetea a los ~15 s. Con SUSTAIN, spinea muerto. En
  ninguno de los dos hay un instante con (t25 vivo + CPU en punto seguro +
  PCM decodificado listo) a la vez.

**VEREDICTO (medido): la inyección de estado/kicks desde el emulador NO puede
abrir el pipeline nativo.** No por falta de un valor concreto, sino porque (a) no
hay punto de inyección seguro sincronizado con t25 vivo, y (b) el render es una
cascada de capas dinámicas (copy←descriptor←engine-build←stream-state←job-count←
t25-loop←mensaje) que habría que conducir TODAS a la vez, en el contexto de tarea
correcto, sin un instante en que hacerlo. Es la confirmación más fina del
"deadlock de bootstrap estructural" de §11.12-11.18: abrirlo equivale a
reimplementar el scheduler de render de MeCCA, no a cebar campos. La plataforma
`HLE_NP` (gateada, default-inerte) queda para futuros intentos.

### 11.23 DÓNDE ESTAMOS: reproductor Now Playing HLE (overlay) usable, con play/pausa/seek/skip

Tras cerrar por medida que el pipeline de render NATIVO de MeCCA es un deadlock
estructural sin apertura por inyección (§11.12-11.22), y validar con una
investigación multi-agente qué SÍ es alcanzable, se implementó un **reproductor
Now Playing funcional por HLE** (no la UI nativa de Apple). Committeado en la rama
`dsp-deadlock-break`.

**Qué es real y qué es HLE (honesto).**
- **Audio: REAL.** La canción del disco del huésped, decodificada host-side, por
  el DMA/I2S propio (CS42L55) → altavoces (Pearson 1.000, §11.19).
- **Pantalla del reproductor: HLE (dibujada por QEMU).** Un overlay sobre el
  framebuffer RGB565 (`ipod_classic_np_overlay.c`), NO la pantalla Now Playing
  del retailOS (que nunca se dibuja: render nativo en deadlock, `copy32`=0).
- **Controles: reales sobre el datapath.** Los botones de la rueda manejan la
  reproducción HLE, no el media player nativo.

**Los dos arreglos clave que lo hicieron posible.**
1. **El guest ya no se congela pasados los 15 s.** `ipod_classic_clk.c`: al
   suprimir el reinicio-watchdog de tag en blanco "--------", ahora **redirige la
   CPU a retornar de la rutina de reset** (PC=LR, la rutina 0x08322b10 es una
   hoja llamada con bl) en vez de dejarla en el spin muerto 0x08322b28. Además,
   con `HLE_AUDIO_SUSTAIN` se suprime el reset en blanco **también durante la
   navegación de menús** (gate `ipod_classic_hle_audio_sustain_on()`), no solo al
   reproducir, para que el guest no reinicie a ~46 s antes de llegar a Música.
   (Descubrimiento del workflow: la pantalla NO depende de `copy32` ni del
   escalador; el único bloqueo de una pantalla estable era este auto-reinicio, y
   el SUSTAIN previo lo suprimía DEMASIADO TARDE dejando la CPU congelada.)
2. **Capa de control sobre el cursor único `hle.pcm_cursor`** (`hle_audio.c`):
   pausa (congela feed+drain en `i2s.c` → silencio real, sin zumbido),
   seek ±s (salta el cursor), siguiente/anterior (worker reestructurado a bucle
   de peticiones de decode con contador de generación), y getters de posición/
   duración/pista. `sm1.c` +0xa48 lee el cursor del datapath. Metadatos reales
   (título/artista/álbum) vía `ffprobe`.

**Controles (teclado):** `s`=play/pausa, `d`/`a`=siguiente/anterior, `]`/`[`=
seek ±5 s, `w`=ocultar (volver al menú). El overlay auto-aparece al decodificar
una pista y **NO secuestra SELECT** (bug corregido: hacía "atrancar" el menú).

**Verificado (capturas + WAV):** overlay muestra "Numb / Encore" — "Linkin Park",
"Track 1 of 4", barra + reloj avanzando (0:06→0:47→1:02); pausa congela el reloj
("PAUSED") y el WAV deja de crecer (silencio); seek salta; siguiente re-decodifica
(track 1→2→3 de 4). Todo gateado (`HLE_NP_OVERLAY` + `HLE_AUDIO_DATAPATH`/
`SUSTAIN`); build por defecto byte-idéntico (sin regresión Rockbox/navtest).

**Cómo lanzarlo:** `HLE_AUDIO_DATAPATH=1 HLE_AUDIO_DISK=<img> HLE_AUDIO_SUSTAIN=1
AUDIOPACE=1 AUDIOLIVE=1 HLE_NP_OVERLAY=1 qemu-system-arm -M iPod-Classic -kernel
osos_decrypted.bin -drive if=ide,format=raw,file=<img> -snapshot -audio pa`.

**Lo que queda (nativo, sin garantía):** hacer aparecer la pantalla Now Playing
REAL de Apple exige reconstruir el protocolo de streams/jobs/mensajes de MeCCA
capa por capa (§11.22); el redirect ya mantiene vivo al guest, pero su contador
de tiempo nativo sigue muerto. Fuera de alcance de esta entrega.

### 11.24 Asalto #13: la hipótesis de TIMING refutada por medida (sweep icount), y el encolado de render localizado como camino MUERTO en frío

Sesión de retome en máquina nueva (clon limpio + WSL Ubuntu-24.04, QEMU 8.2 +
`libpctrace.so` recompilados, assets desde la Release `assets-v1`). El baseline
**reproduce el deadlock idéntico** al documentado (`copy32=0`, `h12=1`, `ex6=57`
matches, `mbox52` cero envíos, `engine=1`) — la emulación es fiel entre máquinas.
Dos experimentos NUEVOS, ambos **VERIFICADOS** por `pctrace` no invasivo. Runners:
`tools/session-dbg/run_icount.sh`, `sweep_icount.sh`, `run_bind.sh`.

**A) El sweep de `-icount` — la hipótesis de timing, propuesta 3× (§11.12/§11.14/
§11.16) y nunca corrida, queda REFUTADA por medida.** Se corrió el mismo probe de
play navegado bajo `-icount shift={off,1,2,3,4}` (≈1000, 500, 250, 125, 62.5 MIPS;
el core real ARM926 ronda ~200 MIPS ⇒ shift 2-3). El core es único (`max_cpus=1`,
`arm926`) y ningún dispositivo de la máquina usa reloj de host (i2s/timers/pl080 =
`QEMU_CLOCK_VIRTUAL`/`ptimer`), así que bajo icount la máquina es **determinista**.
Resultado:

| shift | MIPS | copy32 | h12 | ex6 | mbox52 envíos | engine |
|---|---|---|---|---|---|---|
| off | ~1000 | **0** | 1 | 57 | 0 | 1 |
| 1 | 500 | **0** | 1 | 57 | 0 | 1 |
| 2 | 250 | **0** | 1 | 57 | 0 | 1 |
| 3 | 125 | **0** | 1 | 56 | 0 | 1 |
| 4 | 62.5 | **0** | 1 | 56 | 0 | 1 |

`copy32=0` es **invariante** en todo el rango. El muro **NO es una carrera de
scheduling** sensible a la velocidad del guest: es estructural (confirma §11.13-E,
y da la razón al auditor de §11.13/§11.14 — "el determinismo por sí solo no basta").
El régimen A/B de §11.11 era artefacto del PESO de la instrumentación, no del ritmo
de CPU. `icount` queda **cerrado como arreglo**; sólo sirve ya como replay
determinista para sondas pesadas.

**B) El encolado de render, localizado a nivel de instrucción como camino MUERTO en
frío (refina §11.18).** Decodificado en `iram.dis`: el contador de jobs de render
`[0x22008c1c]` y el objeto del motor `[0x220106d0]` los comparte una **familia de
timbres** del motor, todos con la forma `[0x22008c1c]++; signal(motor)` vía el
veneer KS `0x22003670→0x080a1858`:
- `0x220060e0` (`jobenq`, gateado por `[engobj+0x44]`): **hits=0** — nunca corre.
- `0x2200683c…0x22006878` (el "timbre de render" `0x22006870` de §11.17): gateado
  por **`[engobj+0x46]!=0`** (`cmp [r0+0x46],#0; bxeq lr`); §11.18-C midió
  `+0x46=0` en frío ⇒ **retorna sin encolar**.
- `0x22005e58` (`bindchk`: `if [descr+5]==1 return; else job_setup`): **hits=0** —
  la comprobación que vincularía fuente↔copia **nunca se invoca** (se despacha por
  vtable `bx r1`, no por `bl`, y su slot no se alcanza en frío).

Sonda `run_bind.sh` (pc-watch + `memw=0x22008c1c`): el contador de jobs recibe **3
escrituras totales** — 2 de un `memset` (`0x080001a0`) y **1 sola** del bring-up
único del motor (`0x220067b8`) — y **CERO del incrementador de `jobenq`
(`0x220060f8`)**. Es decir: **jamás se encola un job de render**; el motor corre su
setup una vez (§11.13-B) y se queda esperando el timbre que su propio gate
`[engobj+0x46]=0` impide emitir. Es la afirmación más limpia del muro: no es que
falte un valor, es que **el slot de vtable del encolado de render nunca se despacha
en el arranque en frío**, porque su habilitación (`+0x46`, y simétricamente el
binding del descriptor `0x0bb12d54`) sólo la pone el propio ciclo de render que
nunca arranca. Coherente con la simetría de §11.18-D; forzar un solo gate no basta
(§11.18 ya forzó `+0x46=1`/`+88=1` → `copy32=0`).

**VEREDICTO (medido, esta sesión):** el asalto #13 **cierra la vía de timing** (icount
refutado) y **afina el muro** al slot de vtable de encolado de render que no se
despacha en frío. NO abre el pipeline nativo. La única vía restante sigue siendo la
de §11.14/§11.18: un **HLE del cebado de MeCCA** que despache el primer job de render
en el contexto de tarea correcto (poner `+0x46`, encolar vía `0x220060e0`, vincular
el descriptor `0x0bb12d54` con `[descr+5]=1`) — trabajo abierto, sin garantía de
convergencia. El iPod físico **no aporta** aquí (diskless, sólo emCORE; no ejecuta el
retailOS reproduciendo — wiki/10 §0/§5). Árbol del modelo byte-idéntico (sólo esta
entrada de wiki + runners en `tools/session-dbg/`); baseline/Rockbox sin tocar.

### 11.25 Asalto #13 (cont.): el lazo del motor MeCCA decodificado entero; SM1 sin IRQ (hueco de fidelidad REAL); y por qué forzar cada capa no basta

Sesión larga de retome en máquina nueva. Contiene **una corrección importante de una
afirmación propia** (§A), el **mecanismo completo del lazo de render** que trece
asaltos no habían decodificado (§C), y un **hueco de fidelidad genuino del modelo**
(§D). El muro nativo **NO cae**, pero el mapa cambia de forma sustancial.

**A) CORRECCIÓN: el `copy32=2` de §11.24 NO fue un render.** Con un hook de PC
externo (breakpoint gdb en `0x08016e88`, CPU detenido) se forzó `[r7+5]=1` y el copy
**ejecutó** — `copy32=2`, `copygo=1`, primera vez en 13 asaltos que el contador se
mueve. Pero midiendo el puntero destino en el store (`regpc` en `0x08016eb4`):
**`r2 = 0x2238b800`**, y la IRAM del S5L8702 va de `0x22000000` a `0x22040000`. El
copy escribía **3,4 MB FUERA de la RAM**, porque el campo `[r4+12]` del descriptor
vale `0x0036b800` (basura). **El gate estaba cerrado por una razón legítima:** el
contexto de render no existe en frío. Forzarlo produce una escritura fuera de rango,
no audio. Lección: un contador que se mueve no es una trama.

**A2) La tormenta de excepciones NO la causan los hooks.** `CPU_exception_handler`
(semihosting del retailOS): **1** en baseline limpio, **111 722** en toda corrida con
`HLE_AUDIO_SUSTAIN`, incluidas las que no llevan hook ni `HLE_NP`. Es un coste
preexistente de SUSTAIN (suprime un reset que el firmware quiere) y conviene tenerlo
presente al interpretar cualquier medida hecha con esa configuración.

**B) La ventana de inyección, RESUELTA (mecanismo).** `HLE_NP_PASSIVE` (nuevo,
gateado): ceba la fuente del copy y los gates por **escrituras de memoria puras**,
sin secuestrar el PC ni exigir CPU en idle. **`deferred=0`**, frente a 1000+ intentos
diferidos y **cero** kicks aterrizados del método de §11.22-D. El muro "no hay ventana
de inyección" queda superado como mecanismo — lo que no basta es lo que se inyecta.
También se midió que el host-decode (`have_pcm`) sólo está listo ~48 s **después** del
único render natural, así que el prefill se arma desde el play-start.

**C) EL LAZO DEL MOTOR, DECODIFICADO ENTERO** (desensamblado, en parte por análisis
multi-agente; corrige el modelo mental de §11.13/§11.14):

- **"mbox 0x52" NO es un buzón.** Es una **variable de condición** (`0x220106d0`) con
  timeout de 3000 ms (`0x22006814` → `0x080b3e78` → `0x0805c518`). Su "post"
  (`0x080a1858` con `r1=0` → `0x08051bac`) es un **notify-one sin datos**, y es
  **no-op si nadie está encolado**. Por eso forzar el `recv` a devolver éxito
  (medido: 6 veces) no produjo absolutamente nada.
- **El motor es el hilo `"MeCCADecode"`** (IRAM `0x22002f5c`, creado en `0x22005f48`,
  16 KB de pila, prio 10). Hermanos del mismo constructor: `"FrameDone"`
  (`0x22004630`) y `"EndOfStream"` (`0x22004754`).
- **Al despertar NO actúa sobre la señal:** re-testea el **contador de jobs
  `0x22008c1c`** (`0x22002fe0`: `ldr/cmp/ble`, con signo) y si es `<= 0` vuelve
  directo a su espera de 3 s. Las notificaciones son irrelevantes sin jobs.
- **Con `jobcount > 0`** llama al pump (`0x22005dec` → slot vtable +0x20 →
  `0x22006118`) y **decrementa** el contador.
- **El lazo es autosostenido por diseño:** si el pump devuelve 0, el motor
  **re-encola** un job (`0x22005e38` → `0x220067e4`, `jobcount++`) — el contador se
  mantiene y el render corre libre. **Un solo pump exitoso bastaría.**
- **Lo que lo rompe:** el pump **aborta con −1 nada más entrar** si
  **`[engobj+0x44] != 0`** (`0x22006170`, escribe estado 5 en `0x22008c54`); segunda
  salida temprana si `[engobj+0x4c] == 2`. Un solo −1 y el motor **no re-encola**: el
  contador cae a 0 y el lazo **aparca para siempre**. Ése es el deadlock.
- **Medido y aún sin explicar:** el render llamado en `0x22006300` **nunca retorna**
  (`0x22006304` hits=0, **también en baseline limpio**) — coherente con el
  `dpgateclr = 0` que §11.18-C midió sin interpretar.

**D) HUECO DE FIDELIDAD REAL: SM1 no tenía interrupción, y la IRQ 18 no estaba
cableada.**

- El retailOS **instala un handler** (`0x08017378`) en el **índice 18** de la tabla
  del VIC (cadena `0x0813e688` → `0x080159c0` → IRAM `0x22004cc4`, que guarda el
  handler en `tabla+8+id*4` y toca `VIC0+0x14`).
- Ese handler lee un registro **SM1** (getter `0x080a7638`, el mismo del bring-up de
  audio), testea el **bit 2**, y **señala el evento `0x22008c58`** (`0x080173cc` →
  `0x080a1a24`) — exactamente el evento que el **predicado del pump** (`0x0808af58`)
  espera **500 ms** antes de rendirse. (Nota: el retorno del predicado sale de esa
  espera, **no** del test del bit 12 de SM1+0xa98, que sólo gatea unas escrituras de
  enable — eso explica por qué el experimento de §11.14-D no vio nada.)
- **El guest tiene la línea 18 HABILITADA:** `VIC0 IntEnable = 0x21a7010f` (§11.12-C)
  tiene el **bit 18 activo**, junto a 16/17 (DMAC), 8 (timer), 23 (wheel), 29 (ATA).
- **Nuestro modelo no podía levantarla jamás:** `ipod_classic_sm1.c` no tenía
  `qemu_irq` ni `sysbus_init_irq`, y la máquina nunca hacía `sysbus_connect_irq`. El
  mapa de IRQ salió de Rockbox (`s5l87xx.h`), cuya tabla tiene un **HUECO justo en la
  18** porque Rockbox decodifica en el ARM y **no usa SM1**.
- **Implementado:** `qemu_irq` en SM1 + `sysbus_connect_irq` a `S5L8702_SM1_IRQ = 18`.
  Disparo gateado: `SM1_IRQ=1` (pulsa cuando el motor está `powered`), `SM1_IRQ=2`
  (incondicional, diagnóstico).
- **Medido:** con `=1` **no dispara nunca** — el flag `powered` de SM1 **no se activa
  en toda la ruta de play** (11 diagnósticos de idle en ~110 s), lo cual es en sí un
  hallazgo pendiente de explicar. Con `=2` el pulso temprano **mata al guest** a los
  ~125 ms (salta a un vector antes de que el handler esté instalado). **La línea queda
  cableada; el disparo fiel queda por caracterizar.**

**E) El cebado del contador de jobs: hace girar el lazo, pero no renderiza.**
`HLE_NP_JOB=1` mantiene `[0x22008c1c] >= 1` y `[engobj+0x44] = 0`. Medido
(`run_pump.sh job2/job3`, idéntico con y sin SUSTAIN):
`motorpred=35 motorpump=22 dpump=22 jobrepost=11 pumpabort=11` **pero `engine=0`,
`copy32=0`, `resets=11`**. Es decir: el motor **sí** cicla (baseline: `dpump=1`,
aparcado) y **sí re-encola por su cuenta** (`jobrepost=11`, el autosostenimiento de
§C existe y se observa), pero la mitad de las pasadas abortan y la otra mitad
**devuelven 0 sin llegar al render**; el guest acaba reiniciándose 11 veces. Frente al
baseline (`engine=1`) esto es una **regresión**: inyectar jobs desde fuera, de forma
asíncrona al mutex del motor, hace girar el lazo en vacío. **No abre el pipeline.**

**VEREDICTO honesto del asalto #13.** El pipeline nativo **sigue cerrado** y la
pantalla Now Playing de Apple **no se dibuja**. Lo que sí queda establecido:

1. La hipótesis de **timing está refutada** (§11.24-A, sweep icount).
2. El **mecanismo del lazo de render está decodificado** (§C) — condvar sin datos,
   contador de jobs, re-encolado autosostenido, flag de aborto `[engobj+0x44]`.
3. Existe un **hueco de fidelidad real, cableado a medias** (§D): SM1 sin línea de
   interrupción y la IRQ 18 sin cablear, con el guest esperándola.
4. Cada capa forzada revela que **la de abajo tampoco está construida** (descriptor
   con destino basura, contexto de render inexistente, `powered` que nunca sube). El
   patrón sugiere que **falta un paso de construcción del contexto de render**, aguas
   arriba de todo lo forzado, y que ése —no un gate— es el objetivo del asalto #14.

**Próximo paso concreto propuesto:** averiguar **por qué `powered` de SM1 nunca se
activa en la ruta de play** (¿el bring-up de audio no llega a correr entero?) y **por
qué el render `0x0808170c` no retorna**. Ambas son preguntas cerradas, baratas de
medir, y cualquiera de las dos puede ser la construcción de contexto que falta.

**Higiene:** todo gateado (`SM1_IRQ`, `HLE_NP_PASSIVE`, `HLE_NP_JOB`) o externo
(hooks gdb en `tools/session-dbg/`); **build por defecto byte-idéntico**; Rockbox y
navtest sin tocar. `pctrace` ampliado para volcar `r4..r7` en `regpc`/`sendex`.
Runners nuevos: `run_icount.sh`, `sweep_icount.sh`, `run_bind.sh`, `run_np_prime.sh`,
`run_descr.sh`, `run_cascade.sh`, `run_pump.sh`, `copygate_hook.gdb`,
`cascade_hook.gdb`.

### 11.26 CAE EL MURO: la posición de SM1 era un OFFSET DE ANILLO — el render nativo NO estaba en deadlock, estaba FALLANDO

**El hallazgo que cierra catorce asaltos.** Durante catorce sesiones el render nativo
se caracterizó como un "deadlock de rendezvous estructural" del framework MeCCA.
**Era incorrecto.** El render no esperaba a nadie: **tomaba un data abort** por una
dirección que nuestro propio modelo le hacía calcular mal.

**A) La cadena completa, localizada bloque a bloque** (`pctrace range=` + `trig=`,
que fue la herramienta decisiva: traza el PC de cada bloque traducido dentro de una
ventana):

```
pump 0x22006118 → render 0x0808170c → mixer 0x0809a74c → 0x08016f14
   → 0x08016f68 → copy 0x08016e44 → gate [r7+5] cerrado → 0x08016ecc
   → bl 0x08085ea0   ← AQUÍ SE DETIENE TODO
```

Cada paso se midió por el bloque siguiente que **no** llega a ejecutarse: la traza
del render acaba en `0x08081a8c` (el bloque que contiene el `bl` al mixer), la del
mixer acaba en `0x0809a880` (el bloque que contiene `bl 0x08016f14`), y la del
render por-stream acaba en `0x08016ecc` (el bloque que contiene `bl 0x08085ea0`).

**B) `0x08085ea0` no es una espera: es un memcpy.**

```
8085ea0:  ldr r3,[r1,#16]   ; bytes
8085ea4:  ldr r1,[r1,#12]   ; base del destino
8085ea8:  lsr r3,r3,#2      ; palabras
8085eac:  add r1,r1,r2      ; destino = base + offset
8085eb0:  cmp r3,#0
8085eb4:  ldrne r2,[r0],#4
8085ebc:  strne r2,[r1],#4
8085ec0:  bne 0x8085eb0
8085ec4:  bx lr             ← NUNCA se alcanza (medido, hits=0)
```

Volcado del descriptor en vivo (gdb, `memcpy_probe.gdb`): `[descr+12]=0`,
`[descr+16]=0x28` → **40 bytes, 10 palabras**. Es la copia de silencio, diminuta.
No es un bucle infinito.

**C) El destino, y la prueba causal.** El destino medido fue `0x222dc000` en una
corrida y `0x222fbc00` en otra — **crece con el tiempo de reproducción**. Con la
posición de SM1 forzada a 0 (`SM1_POS=0`) el destino es **exactamente `0x22020000`**.
Es decir:

```
destino = 0x22020000 (base de IRAM1) + posición_SM1 × 4
```

El búfer de salida de audio **vive en IRAM1** (128 KiB), y el firmware calcula su
puntero de escritura como base + posición en tramas estéreo.

**D) EL FALLO ERA NUESTRO.** `sm1_pos_reg()` devolvía la posición como
`i2s->tx_samples / 2`, un **contador acumulativo que nunca envuelve**. A los ~1,9 s
de reproducción (32768 tramas) el puntero se salía de IRAM1; las 10 escrituras
caían en **memoria no mapeada**, tomaban un **data abort**, y el copy no retornaba —
ni el mixer, ni el render, ni el pump. Desde fuera, indistinguible de un deadlock:
todas las tareas paradas, ninguna IRQ pendiente, ningún poll de hardware.

Esto explica por qué **nada** de lo intentado en catorce asaltos funcionaba: el
sweep de `icount` (§11.24-A), el cebado de gates, forzar el `recv` del motor, el
cebado del contador de jobs — todos atacaban un síntoma. Y explica también por qué
forzar el gate del copy (§11.25-A) sólo producía una escritura fuera de rango: el
puntero ya estaba corrupto antes de llegar ahí.

**E) LA CORRECCIÓN (fidelidad de hardware, sin tocar el firmware).** La posición es
un **offset dentro del anillo**, así que envuelve:

```c
#define S5L8702_SM1_POS_RING_FRAMES  (0x20000 / 4)   /* IRAM1 = 128 KiB */
pos %= S5L8702_SM1_POS_RING_FRAMES;
```

Toda escritura que emite el firmware cae ahora dentro de IRAM1. `SM1_POS=<frames>`
permite anular el wrap para experimentos.

**F) MEDIDO (baseline de 14 asaltos → con el fix, build por defecto):**

| contador | antes | después |
|---|---|---|
| `rendret` (0x22006304, el render retorna) | **0** | **2** |
| `dpump` (0x22006118) | 1 | **5** |
| `pumploop` (0x22006324) | 0 | **4** |
| `pumppred` (0x0808af58) | 0 | **4** |
| `mixer` (0x0809a74c) | 1 | **5** |
| `copyent` (0x08016e44) | 1 | **144** |
| `jobrepost` (0x220067e4) | 0 | **2** |
| excepciones | 1 | 1 (limpio) |

**El render nativo de Apple retorna por primera vez y el lazo cicla**, con el motor
re-encolando jobs por su cuenta — el autosostenimiento descrito en §11.25-C existe y
ahora se observa.

**G) Lo que queda.** `copy32` sigue en **0**: el gate `[r7+5]` del copy nunca pasa
porque el decodificador aún no publica trama (`decframe 0x081232d8` hits=0; de 87
entradas al copy sólo 12 llegan al chequeo del gate, el resto toman la rama
`[r4]!=0`). El siguiente eslabón es **quién manda al decodificador el mensaje de
decodificar** (código `0x52800001`, caso 1 del switch `0x08123cfc`) y por qué no
llega. Dado el precedente de esta sesión, merece buscarse **otro registro cuya
semántica modelemos mal**, no lógica del guest.

**H) Método que lo resolvió, para futuras sesiones.** Lo decisivo no fue una
hipótesis sino una técnica: **traza de bloques acotada** (`range=LO-HI` con
`trig=ADDR` para abrir la ventana) que muestra el último bloque ejecutado antes de
que el control desaparezca, aplicada recursivamente hacia dentro de cada llamada.
Cuatro corridas bastaron para pasar de "el render no retorna" a la instrucción
exacta. Los contadores agregados (`pc=`) dicen *si* algo corre; la traza de bloques
dice *dónde deja de correr*.

**Higiene:** el fix es por defecto (Rockbox no lee SM1, así que no le afecta; boot y
navegación tampoco entran en el bring-up). `SM1_POS` queda como knob de experimento.
Herramientas nuevas: `run_sm1en.sh`, `run_rtrace.sh`, `memcpy_probe.gdb`.

### 11.27 CORRECCIÓN GRAVE: el "decodificador de audio" del clúster `0x08123xxx` es el hilo de CARÁTULAS (`iMAImageCacheThread`)

Hallazgo de un análisis multi-agente del desensamblado, **verificado a mano**. Obliga
a reinterpretar buena parte de §11.9–§11.22.

**A) La prueba.** El singleton en `0x0891233c` sobre el que se construye todo ese
clúster se crea en `0x08123400`, que pasa a la creación de hilo (`0x0832dd88`) la
cadena en `0x081234f4`. Sus words, en little-endian:

```
81234f4: 49414d69 → "iMAI"
81234f8: 6567616d → "mage"
81234fc: 68636143 → "Cach"
8123500: 72685465 → "eThr"
8123504: 00646165 → "ead\0"          ⇒ "iMAImageCacheThread"
```

Corroboran las cadenas vecinas del mismo islote de código: `"iTunes Image DB.itdb"`
(`0x08122e9c`, `0x081d7cb0`), `"iTunes Image DB"` (`0x081f07e0`) e
`"ImagePresentationEngine"` (`0x081d8f0c`).

**B) Qué queda reatribuido.** Pertenecen al subsistema de **carátulas / iTunes Image
DB**, no al audio:
- el despachador `0x08123dcc` (y por tanto los contadores `disp`),
- el switch `0x08123cfc` y su familia de códigos `0x52800000..0x52800009`,
- el caso 1 (`0x08123d64` → `0x081232d8`), que es **"ejecuta el siguiente job de
  imagen"**, no "decodifica una trama de audio",
- el handler tipo-12 `0x08123970` (`h12`) y `dwork` `0x08123bfc`.

Los cuatro emisores que mandan el código `0x52800001` son rutas de **píxeles**:
`0x08122b28` y `0x08122be8` (expansión RGB565: constantes de formato `0x1888`/`0x565`,
máscaras `0x7e0`/`0xf800`) y `0x0813ada8` / `0x0813b090` (YUV 4:2:0: `w*h*3/2`). En
esa clase, `reason==1` significa **"mi superficie de destino todavía no está lista,
re-encólame"** — un reintento, no un arranque.

**C) Segunda reatribución.** `0x08025ac4` **no** "pide entrada por exchange 6 +
RECV mbox 9": es un **emisor de traza**. Adquiere el canal de traza 9
(`mov r0,#9; bl 0x8015730`), emite un registro (`bl 0x8015740`) y lo libera. Por
tanto los "57 posts al exchange 6" de §11.14-A estaban contando **registros de
traza**, no flujo de datos, y la conclusión "el lado de entrada está sano" no se
sostiene sobre esa medida.

**D) Lo que NO cambia.** El arreglo de §11.26 (la posición de SM1 como offset de
anillo) es independiente de todo esto: se verificó **causalmente** por la dirección
de destino del copy (`SM1_POS=0` → destino exactamente `0x22020000`) y por el efecto
medido en el camino de render REAL (el memcpy pasa de no retornar nunca a retornar
101/102 veces; `rendret` 0→2; `copy16`=2405 almacenamientos de PCM). Ninguna de esas
medidas depende del clúster de imagen.

**E) Consecuencia práctica.** `decframe = 0` **nunca fue** un indicador de audio, y
tampoco `h12` ni `dwork`. La pregunta correcta, ahora despejada, es: **quién llena el
anillo fuente del mixer en `0x08889098`**, del que el copy nativo lee. Ése es el
decodificador de audio real, y está sin localizar.

**F) Lección de método.** Dos premisas centrales del proyecto (el clúster del
decodificador y el canal de entrada del exchange 6) resultaron ser subsistemas
distintos, y sobrevivieron catorce asaltos porque nadie verificó la **identidad** de
los objetos: bastaba decodificar una cadena. Antes de construir sobre un clúster,
conviene confirmar a qué hilo/clase pertenece (nombre de hilo en la creación,
cadenas vecinas, vtable).

### 11.28 Segundo fallo de fidelidad: el estado de sub-bloque de SM1 era un latch pegajoso — y el render ya construye su contexto

Continuación directa de §11.26. Encontrado por **auditoría del modelo contra el
firmware que lo consume** (la misma técnica que dio §11.26), y confirmado por medida
antes y después del cambio.

**A) El fallo.** `sub+0x14` (leído por `0x080b8470`, enmascarado `&7`) lo modelábamos
como *"7 para siempre en cuanto se escriba cualquier comando"*. Es una **máquina de
estados**, y dos sitios del firmware lo leen esperando valores **opuestos**:

- **El power-up `0x080ba768` quiere 7.** Escribe cmd 4, sondea hasta diez veces
  esperando 7, escribe cmd 16 (run) y **finalmente escribe cmd 96 (stop)**.
- **El render `0x0808170c` quiere 0:**

```
80820e8:  ldrb r0,[r7,#1]
80820f0:  bl   0x80b8470      ; estado del sub-bloque 4
80820f4:  cmp  r0, #0
80820f8:  bne  0x80821a8      ; distinto de 0 → SALTA el build
```

El bloque que se salta, `0x08082100`–`0x080821a0`, es la **construcción del contexto
del motor**: copia de buffer, alta de descriptor (`0x080b67a8`), encadenado
(`0x080bfa58`) y traducción de direcciones (`0x08075d80`).

Como el último comando que escribe el power-up es 96 (stop), en silicio el estado
vuelve a 0 y el render construye su contexto. Nuestro latch respondía 7 para siempre,
así que **el contexto no se construía nunca**.

**B) Medido, antes del cambio:** `statetest=1, ctxbuild=0, ctxskip=1` — el render lee
el estado y se salta su propio build. Es exactamente el *"paso de construcción de
contexto que falta"* que §11.25 predijo, y explica por qué cada capa forzada aguas
abajo revelaba que la de abajo tampoco estaba construida.

**C) El arreglo.** 7 mientras el último comando sea 4 (power up) o 16 (run); 0 tras
el 96 (stop) o si nunca se comandó. Constantes `S5L8702_SM1_SUB_CMD_UP/RUN/STOP`.

**D) Medido, después (corrida con reproducción confirmada, `StartPlayback=1`):**

| contador | baseline 14 asaltos | §11.26 | + §11.28 |
|---|---|---|---|
| `ctxbuild` (`0x08082100`) | 0 | 0 | **2** |
| `rendret` (`0x22006304`) | 0 | 2 | **2** |
| `engine` (`0x0808170c`) | 1 | 3 | **3** |
| `copyent` (`0x08016e44`) | 1 | 144 | **144** |
| `copy16` (`0x08016f08`, PCM real) | 0 | 2405 | **2405** |

**E) El destino del render, ya correcto.** Con ambos arreglos, el destino medido del
copy es **`0x22026400`** — dentro de IRAM1 (`0x22020000`–`0x22040000`). Antes de
§11.26 era `0x222fbc00`, 2,6 MB fuera de la RAM. El pipeline nativo escribe **donde
debe**, con offsets que envuelven en el anillo.

**F) Lo que queda, aislado con precisión.** El anillo fuente del mixer,
`0x08889098`, recibe **sólo `memset`**: watch de memoria sobre
`0x08889000-0x0888a000` da **2048 escrituras desde exactamente 2 PCs**, ambos rutinas
de memset en IRAM (`0x22000198`, `0x220001a0`). **Nadie escribe PCM decodificado
ahí.** Por eso el render nativo, que ya funciona, copia silencio: el WAV capturado
(91 s) sólo contiene los clics de UI (ráfagas idénticas de 1248 muestras, pico
constante 11444).

**G) Pistas para el asalto siguiente.** Los nombres reales de los componentes de
audio, extraídos de la imagen:

| VA | nombre |
|---|---|
| `0x07ffb148` | `MeCCADecode` (hilo del motor, en el blob de IRAM) |
| `0x0816c130` | `MeCCA_PCM_Output` |
| `0x081840ec` | `MeCCA_MediaPlayer` |
| `0x0818d5d4` | `MeCCA_VideoBufferMgr` |
| `0x0818d7dc` | `MeCCAVideoDecode` |
| `0x081c4460` | `MeCCAIOTask` |
| `0x08388c5c` | `MeCCARecordingTask` |
| `0x08859824` | `MeCCA_StreamCache` |

Y un regalo: la función **`0x0816bf48`** contiene un **volcado de diagnóstico que el
propio firmware trae** — escribe su PCM a `"MeCCA_PCM_Output.wav"` (`0x0816c080`
carga el nombre; el flag y el presupuesto de bytes viven en la struct `0x08948f24`,
byte 0 = habilitado, +4 = bytes restantes; escribe `r6` con `r4*4` bytes vía
`0x08121a00`). Esa función **recibe el PCM producido**, así que es el punto exacto
donde nace el audio del retailOS — y activando su flag el firmware se auto-volcaría
el audio, sin instrumentación nuestra.

**Higiene:** ambos arreglos son por defecto y sólo afectan al camino de audio del
retailOS (Rockbox no lee SM1). Sondas nuevas en `run_pump.sh`
(`statetest`/`ctxbuild`/`ctxskip`/`pcmout`/`copy16`) y guarda de puerto para que las
corridas repetidas dejen de colisionar en silencio.

### 11.29 El SHA-1 no estaba modelado: caen el panic y los reinicios — y el aparato queda navegable por primera vez

Sesión centrada en **usar** el reproductor, no sólo en medirlo. Tres resultados: un
tercer fallo de fidelidad hallado y corregido, la mecánica de navegación
caracterizada (llevaba invalidando pruebas en silencio), y el estado del audio
acotado con datos limpios.

**A) El reinicio cada ~46 s no era la batería: era un PANIC del firmware.**
De las siete rutas al reset `0x08322b10` sólo dispara `0x08021c00`, y lo que hay
inmediatamente antes es:

```
8021bec:  ldr r1, =0x08375894
8021bf8:  svc 0x00123456      ; imprime por semihosting
8021c00:  b   0x8322b10       ; y reinicia
```

La cadena en `0x08375894` es literalmente **`"CPU_exception_handler"`**. Pero con
`-d int` se midieron **100 119 excepciones, TODAS IRQ**, ninguna de aborto: la CPU
nunca falla. Es la ruta de panic propia del firmware.

**Causa:** el **acelerador SHA-1** (`0x38000000`) era una ventana no implementada y
el retailOS lo usa de verdad. Con `-d unimp`: escribe la config en `+0x00`, arma el
modo memoria en `+0x80`, fija el tamaño en `+0x8c` y lee el digest de
`+0x20..+0x34` — recibiendo **ceros**. Un hash que nunca cuadra acaba en el panic.

**Arreglo:** el bloque del S5L8720 ya modelado en `hw/arm/ipod_touch_sha1.c` tiene
**exactamente** ese mapa de registros, así que sirve tal cual al S5L8702.
Reutilizado, no reescrito. **Medido: `resets=0 panics=0`** (antes 1-2 por corrida).

**B) El intercambio que impone, medido (A/B sobre la MISMA imagen).**

| | Biblioteca | Reinicios |
|---|---|---|
| `SHA1_OFF=1` | **4 canciones** | 1 |
| por defecto | "No hay música" | **0** |

Con el motor funcionando el firmware **valida de verdad** el hash del iTunesDB de la
imagen y lo rechaza; antes pasaba cualquier cosa porque el digest era ceros. El
arreglo es correcto como hardware; lo que destapa es que la biblioteca de la imagen
lleva una firma que este dispositivo no reproduce (`SysInfo` tiene
`FirewireGuid: 0000000000000000`). Se deja `SHA1_OFF` porque poder hacer el A/B
sobre la misma imagen es lo que distingue "el modelo está mal" de "la imagen está
mal".

**C) La navegación, caracterizada — invalidaba pruebas en silencio.**
1. Un arranque limpio (con `-snapshot` no persiste nada) entra en el **selector de
   idioma**, no en el menú principal. Si no se descarta primero, **cada pulsación
   posterior está eligiendo idioma en vez de canción** — así acabó el menú en griego
   varias veces, y así se explican navegaciones "que no hacían nada".
2. El clickwheel del retailOS **descarta ~5 de cada 7 pulsaciones de SELECT**
   (documentado en `iremote.py`). Una sola pulsación es **indistinguible de un botón
   muerto**; hay que usar la forma con reintentos. Con reintentos, SELECT entra.
3. `tools/session-dbg/run_play.sh` incorpora ambas cosas y conduce el aparato hasta
   reproducir, midiendo el audio por **RMS del WAV del I2S**, no de vista.

**D) El estado del audio, acotado.** Con reproducción lanzada (`StartPlayback=1`):

```
dpump=3  engine=2  rendret=1  ctxbuild=1  mixer=3
copyent=87  copy16=1203        <- el render NATIVO escribe PCM
framepull=1  aacmath=1  fpull2=0  credread=8
escritores del crédito +0x2b8:  0x081cc1f4=1  0x081cc7a4=16  0x081d7548=0  0x081e360c=1
WAV 91 s: 0,19 % de muestras no nulas, todas ráfagas idénticas de 1248 con pico
constante 11444  ->  son los CLICS DE UI, no música.
```

**E) Dos cosas descartadas con datos limpios.**
- **El DSP no interviene.** Durante la reproducción **no hay ni un acceso** a
  `0x39000000`/`0x3F000000`. Los únicos bloques sin implementar que se tocan son
  gpio/eic/miu/lcd/wdt. La vieja teoría del CalmADM2E queda cerrada por medida.
- **`0x0816bf48` no es el empuje de PCM por trama.** Se llama **una vez** y con
  `r1 = 0x080338f4`, que es una dirección de **código**: es el registro del sumidero
  de audio. (`r0=0x0bb169dc r2=0x08a873c0 r3=0`.)

**F) Un error propio, corregido y verificado.** El anillo fuente que se vigilaba
(`0x08889098`) es el del render de **arranque**; en reproducción real el copy lee de
`0x0888a2xx-0x0888a6xx` (medido en vivo con `regpc` en `0x08016edc`). Se repitió la
vigilancia sobre la ventana correcta (`0x0888a000-0x0888c000`) **y la conclusión no
cambia**: 4096 escrituras, **sólo** de las dos rutinas de memset de IRAM
(`0x22000198`, `0x220001a0`), todas en el arranque. **Nadie deposita PCM
decodificado**, y por eso el render nativo —que ya funciona— copia silencio.

**VEREDICTO.** El aparato es hoy **navegable y estable** (sin reinicios), su
**pipeline de render nativo funciona** de punta a punta, y la biblioteca se lee con
`SHA1_OFF=1`. Lo que **no** hay es música: el decodificador de audio no llega a
producir tramas. Con el DSP descartado, el clúster de imagen descartado y el
sumidero identificado, lo que queda es RE del lazo de decodificación —
concretamente por qué `0x081e93d8` extrae una sola trama pese a que el crédito
`+0x2b8` se escribe 18 veces.

### 11.30 El lazo del decodificador, acotado: al códec se le pide CERO muestras (y eso no es la causa única)

Continuación de §11.29, ya con el aparato estable y navegable. Se persiguió el
último eslabón —por qué no sale música— hasta un número concreto, y se acotó qué
NO es la causa.

**A) El extractor de tramas sí tiene bucle y sí pasa su puerta de crédito.**
`0x081e93d8`, tras el gate, ejecuta:

```
81e9428:  ldr r6, [obj+0x3c]        ; índice productor
81e9434:  ldr r1, [obj+0x44]        ; muestras pedidas
81e9438:  ldr r0, [reader+8]        ; espacio de salida
81e9440:  cmp r1, r0 ; movle r0, r1 ; r0 = min(...)
81e9448:  mov r3, r0                ; <- la CUENTA que recibe el códec
81e9458:  bl  0x081e9e70            ; la matemática del códec
81e945c:  sub r6, r6, #1
81e9468:  cmp [obj+0x40], r6 ; bne  ; itera hasta alcanzar al consumidor
```

Medido (`pullpass=1`): **pasa el crédito**. Queda refutado que el crédito `+0x2b8`
fuera el muro, que era la hipótesis de §11.18/§11.25.

**B) La cuenta es CERO — medido en vivo** (gdb en `0x081e9428`, con reproducción
lanzada):

```
reader=0x093cbbd4  obj=0x0ba90c50
prod[+0x3c]=3  cons[+0x40]=2  depth=1     <- SÍ hay trabajo encolado
[obj+0x44]=0   [reader+8]=0               <- pero min(...) = 0
```

Es decir: **el códec se invoca pidiéndole cero muestras**. Por eso "decodifica una
trama" (`aacmath=1`) que no produce nada, y el anillo del mixer nunca se llena.

**C) Pero forzar la cuenta NO basta.** Con un hook que escribe 1024 en ambos campos
justo antes del bucle (`count_probe.gdb`): `aacmath` sigue en 1 y el audio sigue
siendo sólo clics de UI (170 s capturados, 0,200 % de muestras no nulas, todas
ráfagas de 1248/2496 con pico constante 11444). **Hay otra capa debajo**, así que la
cuenta es un síntoma más, no la causa raíz.

**D) Qué queda descartado, con datos.**
- **No es el DSP.** Cero accesos a `0x39000000`/`0x3F000000` durante la reproducción.
- **No es el almacenamiento.** El disco se lee activamente: **30 666 escrituras a
  registros ATA desde 29 PCs distintos** en una corrida, repartidas por todo el
  tiempo, no sólo al arrancar.
- **No es el crédito** `+0x2b8` (§A).
- **No es el anillo que se vigilaba**: en reproducción real el copy lee de
  `0x0888a2xx-0x0888a6xx`, no del `0x08889098` del render de arranque. Re-medido
  sobre la ventana correcta: sólo escriben los dos memset de IRAM.

**E) El interruptor del motor sí cierra su ciclo.** Con `SM1_IRQ=1` (§11.29) el
firmware arma la interrupción por su cuenta (`sm1arm=2`) y el render responde:
`rendret 1→4`, `dpump 5→8`, `copy16 2405→4808`. El camino de salida funciona y se
realimenta; lo que no llega es contenido.

**ESTADO.** El aparato es **navegable y estable**, su **pipeline de render nativo
funciona y se realimenta**, la biblioteca se lee (`SHA1_OFF=1`), y el
almacenamiento sirve datos. **No hay música**: entre el fichero comprimido y el
anillo del mixer hay un eslabón que no produce muestras, y ya no es ninguno de los
sospechosos habituales. El siguiente paso natural es seguir la entrada del
decodificador —qué objeto alimenta `[obj+0x3c]` y de dónde saca los bytes
comprimidos— en vez de seguir empujando desde la salida.


## 11.31 La firma del iTunesDB: hipótesis levantada y REFUTADA por control

Vale la pena dejarla escrita porque era plausible, encajaba con varias medidas, y el
experimento de control la tumbó limpiamente.

**La hipótesis.** `run_play.sh` arrastraba desde §11.28 la nota de que hacía falta
`SHA1_OFF=1` "porque el motor, una vez modelado, rechaza el hash del iTunesDB de la
imagen". Sumado a que la base del disco venía **sin firmar**:

```
hashing_scheme @0x32 = 0
hash58        @0x58 = 0000...0000        <- 20 bytes a cero
```

y a que `iPod_Control/Device/SysInfo` declaraba `FirewireGuid: 0000000000000000`,
la lectura natural era: el Classic 6G valida la firma hash58 derivada del FireWire
GUID, sin GUID no hay firma, sin firma retailOS descarta la biblioteca — y eso era
el "no hay canciones" y, aguas abajo, el `count = 0` del códec.

**El GUID "real" que se dedujo.** Está en el bloque SysCfg al inicio de
`assets/classic6g/bootflash_nor_1MB.bin`, en entradas de 20 bytes (tag de 4 + 16 de
datos) con los tags almacenados al revés:

```
0x18 'SrNm' -> "8L745SDZYMV"
0x2c 'FwId' -> 01000000 | 17 8b 11 13 00 27 0a 00 | 00000000
0x40 'HwId' -> 85160282     0x58 'HwVr' -> 0x13 (19), el mismo que exige 0x0827a188
0xa0 'Mod#' -> MB147
```

Los 8 bytes de `FwId` en little-endian dan **000A270013118B17**, y `00:0A:27` es el
OUI de Apple para FireWire. Se escribió eso en `SysInfo` y libgpod firmó
(`hash58 = 116dcb48...`).

**El resultado: peor.** Con la base firmada, el reproductor pasó a decir **"0
skladeb"** (0 canciones) — y también con `SHA1_OFF=1`, o sea que el SHA-1 no tenía
nada que ver.

**El control que lo zanja.** Se guardó copia intacta antes de tocar nada, y se
arrancó con ella:

| imagen | iTunesDB | GUID en SysInfo | canciones en pantalla |
|---|---|---|---|
| original | **sin firmar** | ceros | **4 skladeb** ✓ |
| reconstruida | firmada, GUID de la NOR | 000A270013118B17 | 0 ✗ |

La imagen original **siempre leyó su biblioteca bien**. La firma no era el problema:
el problema lo introduje yo al reescribir la base. Y de paso queda refutado que la
NOR ausente importe aquí — el firmware no lee SysCfg (no hay ni una cadena `SCfg` ni
`FwId` en el osos, ni bloque SysCfg en todo el disco).

**Refutado también, de camino:** que el firmware no pudiera leer la FAT por el
tamaño de sector. La partición está en unidades de 4096 B (22696 x 4096 cae exacto
en el superbloque FAT32) mientras el núcleo IDE de QEMU es de 512 B, lo que hacía
sospechar que el firmware buscaría el sistema de ficheros en el sitio equivocado.
Pero lee 4 canciones de esa FAT, así que lo encuentra.

**Lo que sí queda establecido de §11.30, con la lectura corregida:**

- **El camino de salida funciona de punta a punta.** DMAC0 ch1 mueve `0x22008c5c` ->
  `0x3ca00010` (FIFO del I2S) con un LLI de un solo nodo que se apunta a sí mismo en
  `0x22010020`, 4 KB por vuelta, 16 bits, fuente incremental, sin parar.
- **Y ya sale audio real por él**: todas las capturas traen ~15 000 muestras no nulas
  de ~9 M, pico 11444, siempre en múltiplos de 1248 — son los **clics de la rueda**.
  Mixer -> render -> IRAM0 -> DMA -> FIFO -> WAV está probado.
- **Las "muestras no nulas" del anillo del mixer** (`007fffff 006ba27e 005a827a
  004c1bf8`) son la envolvente decreciente de ese clic, no una canción.

**EL FALLO REAL, medido en la imagen que sí tiene biblioteca** (corrida `ctl1`):

```
StartPlayback=1  framepull=1  pullpass=1  pullbail=1  aacmath=1
copyent=201  copy16=3607  mixer=7  dpump=7  rendret=3
```

La reproducción **arranca**, el tirón de trama **pasa** el gate de crédito una vez, el
códec corre **una vez**... y ahí se acaba. **Es un decodificador que saca una sola
trama y se detiene**, no una biblioteca ilegible ni una salida rota.

## 11.32 El nudo real: la biblioteca y el SHA-1 son excluyentes

Cuatro corridas con la misma imagen y una sola variable cambiada dejan el estado sin
ambigüedad:

| corrida | SHA-1 | iTunesDB | biblioteca en pantalla | reproducción | reinicios |
|---|---|---|---|---|---|
| `ctl1` | OFF | original | **4 skladeb** | arranca | - |
| `mj4`  | OFF | parcheada en sitio | **4 skladeb** | `StartPlayback=2` | 1 |
| `mj3`  | **ON** | parcheada en sitio | 0 canciones | no arranca | 0 |
| `sig1` | **ON** | reescrita y firmada | 0 skladeb | no arranca | 0 |

**Con el SHA-1 conectado el firmware rechaza la base; con el SHA-1 apagado la acepta
pero vuelve el panic de ~46 s.** La corrida `rtc1` lo enseña de golpe: empieza a
entrar en Música y a los pocos segundos está de vuelta en el selector de idioma.

Eso reinterpreta §11.31 en un punto importante: **la nota original de `run_play.sh`
tenía razón**. El acelerador SHA-1, una vez modelado, hace que el firmware pueda
verificar la firma del iTunesDB -- y la de esta imagen no pasa. No es un fallo del
acelerador: es una comprobación que antes no podía ni ejecutarse.

Y reinterpreta también el "decodificador que saca una trama y se para": las corridas
donde se veía eso llevaban `SHA1_OFF=1`, es decir, **el aparato se estaba
reiniciando**. `framepull=1` puede ser sencillamente lo que le dio tiempo a hacer
antes del reset, no un atasco del códec.

**Lo que NO funcionó para firmar** (medido, no supuesto):

- Reescribir la base con libgpod la rompe aunque el SHA-1 esté apagado: la original
  tiene OCHO secciones `mhsd` (tipos 1, 3, 2, 4, 8, 6, 10 y 5) y libgpod 0.8.3 no
  las conserva todas. `mj1` -> "Pas de musique", y poner el hash a ceros tampoco lo
  arregla (`nh1` -> "Keine Musik"). Por eso `tools/ipod-library/patch_tracks.py`
  edita en sitio y no reescribe nada.
- Firmar con el GUID de la NOR (`000A270013118B17`) tampoco basta, y de hecho el
  firmware **no lee SysCfg**: no hay ni una cadena `SCfg` ni `FwId` en el osos, ni
  bloque SysCfg en todo el disco, ni región NOR mapeada en la máquina.

**El camino que queda es medir la firma, no adivinarla.** El SHA-1 está modelado, así
que el propio motor ve qué bytes le da el firmware a digerir y qué digest saca; con
eso y el punto donde compara se puede construir una firma que pase, sin tocar el
firmware.

## 11.33 Dos fallos de fidelidad encontrados de camino

**El RTC no corría.** Con `PMUTRACE=1` se ve al firmware releer en bucle el bloque
`0x59..0x5f` del PMU -- segundos, minutos, hora, día de la semana, día, mes, año --
y el modelo respondía **cero a todo**. Un segundero real no se queda quieto, y en
pantalla se notaba: Reloj y Calendario mostraban una fecha fija absurda. Ahora esos
registros se sirven en BCD desde `qemu_get_timedate()`, la misma fuente que usa
`hw/rtc/pl031.c`, así que `-rtc` funciona aquí como en el resto de QEMU.

**La pantalla de batería baja.** retailOS levanta "Batería baja -- Conecte la
alimentación" al entrar a reproducir, y eso por sí solo bloquea el audio. Queda
medido que **no viene del canal de ADC que este modelo contesta**: con `PMUBATT=1023`
(fondo de escala) la pantalla sale igual. `PMUBATT` queda como variable de entorno
justamente para poder seguir descartando; el origen del veredicto sigue SIN
DETERMINAR, aunque tras arreglar el RTC no volvió a aparecer en `rtc1`.

## 11.34 El arnés de pruebas mentía, y por qué

Dos fallos del propio instrumental invalidaron medidas y conviene no repetirlos.

**Instancias zombis.** El emulador se lanza como `build/qmus_$TAG`, no como
`qemu-system-arm`, así que el `pkill -x qemu-system-arm` de rigor **nunca mataba
nada**. Las instancias viejas seguían escuchando en `:4455`, y entonces
`iremote.py` conducía y fotografiaba **la máquina anterior** mientras la nueva no
producía ni log ni WAV. Corregido en `run_play.sh` y `run_nav.sh` con
`pkill -9 -f 'build/qmus[_]'` -- con el corchete para no matarse a sí mismo y con el
prefijo de ruta para no matar al script que lo invoca.

**Capturas prematuras.** La captura del menú se tomaba mientras la base todavía
estaba cargando, así que la misma imagen intacta daba "4 skladeb" en una corrida y
"Zádná" en otra. Cualquier conteo en pantalla sólo vale tras dejar cargar.

Y de la navegación, dos cosas medidas: el par de despertar `q,e` **no suma cero al
principio de una lista** (subir desde la primera entrada no hace nada, así que cada
reintento arrastra el cursor hacia abajo -- así se acabó en Calendarios en vez de en
Canciones); y el submenú de Música empieza por **Cover Flow**, con Canciones en
**sexta** posición, no en cuarta.

## 11.35 El motor SHA-1 hasheaba la CADENA VACIA (y le faltaba media DRAM)

§11.32 dejaba el nudo planteado como "la base no pasa la firma". **Eso era falso**,
y la traza nueva `SHA1TRACE=1` lo enseña de un vistazo:

```
SHA1TRACE #1: buffered=64 bytes, trailer says 0, memory_mode=1 start=0x8ba718c0
  head: 00 00 00 00 00 00 00 00
  digest:da39a3ee5e6b4b0d3255bfef95601890afd80709
```

Ese digest es el SHA-1 **de la cadena vacía**. Dos fallos encadenados, los dos
nuestros:

**1) La longitud del mensaje.** El modelo viene portado del iPod Touch y deducía
cuántos bytes hashear leyendo los últimos ocho del búfer, asumiendo que el firmware
escribe el relleno SHA-1 con su trailer de longitud en bits. El retailOS del Classic
**no rellena**: entrega 64 y luego 4096 bytes en modo memoria y espera que lo haga el
hardware. Sin trailer, esos ocho bytes eran cero, la longitud salía cero, y el motor
contestaba siempre lo mismo. Ahora la longitud sale de **cómo llegó** el mensaje: en
modo memoria el invitado ya la dijo por `SHA_INSIZE`. (Un primer intento confió en el
trailer "cuando pareciera una longitud" y leyó 34 de 64 bytes de datos como si lo
fueran — estos mensajes parecen aleatorios, cualquier octeto pasa por uno.)

**2) La DRAM sin caché no estaba mapeada.** Las direcciones son `0x8ba718c0` y
`0x8ba7192c`, o sea `0x88000000 + 0x03a718c0`: la vista sin caché de `0x0ba718c0`.
Con nada mapeado ahí, `cpu_physical_memory_read` devolvía **ceros**. El SoC expone la
misma DRAM dos veces y el propio código ya lo daba por hecho — la nota de DMAC0 dice
que el invitado programa el canal con el alias cacheado mientras la CPU corre sobre
el otro. Ahora el alias existe, y el motor lee datos de verdad
(`head: 67 23 fe 30 ...`).

**3) La ventana de 1 MB se respalda en los dos casos**, con el motor superpuesto.
Antes era uno u otro: con `SHA1_OFF` todo el megabyte era stub y con el motor
cableado sólo sus `0x100` bytes de registros. El interruptor cambiaba mucho más que
si los digests eran reales, y eso contamina cualquier comparación A/B.

**Lo que esto REFUTA.** La firma del iTunesDB no pinta nada aquí: el motor hace
**dos** hashes en toda la corrida, de 64 y 4096 bytes, y el contenido **no es la
base** (cambia entre arranques y no lleva magia `mhbd`). Toda §11.31-11.32 apuntaba a
una comprobación que no existe.

**El estado real, medido:**

| SHA-1 | arranque | biblioteca | reproducción |
|---|---|---|---|
| **activo** | **limpio**: ni `CPU_exception_handler` ni reinicio, `resets=0` | vacía | no arranca |
| `SHA1_OFF=1` | panic + reinicio a los ~46 s | 4 canciones | arranca, 1 trama |

Con el motor arreglado, **la configuración fiel (SHA-1 activo) es la que arranca
limpia**. Pero la biblioteca sigue vacía ahí, y ya no por una firma. Comprobado que
tampoco es lentitud: dejando el aparato quieto, las capturas a 30, 120 y **240
segundos** son idénticas ("Ingen musik"). Lo que el firmware hace con esos dos
digests, y por qué el resultado le deja la biblioteca vacía, es lo que queda por
determinar.

## 11.36 El corte real: el copy del render se sale de IRAM1

Con `SHA1_ZERO=1` (el motor presente, pero el digest respondiendo cero como hacía el
stub) **la reproducción arranca**: `framepull=1`, `aacmath=1`, `pcmout=4`. O sea que
lo que decide si la biblioteca se acepta es el **valor del digest**, no que el
dispositivo esté o no: con un digest real el firmware ejecuta una comprobación que
esta imagen no pasa; con ceros la salta. Eso deja un único bloqueo para que suene: el
reinicio.

**Y el reinicio tiene una sola causa, medida.** El log `-d int` de QEMU dice que en
toda la corrida se toma **exactamente un** Data Abort:

```
Taking exception 4 [Data Abort] on CPU 0
  ...with DFSR 0x8 DFAR 0x22040216
```

`0x22040216` está 0x216 bytes por encima del final de IRAM1 (que acaba en
`0x22040000`). Capturado en el vector de abort, el culpable es inequívoco:

```
faulting insn at 0x08016f08
  0x8016f08:  strhne r3, [r2], #2
  r2=22040a16  r1=00000021  r4=0bb12d54  r5=0888ba74
```

Es **`copy16`**, el bucle de copia de media palabra del render, escribiendo PCM en el
anillo de IRAM1 y saliéndose por arriba.

**El desbordamiento está ACOTADO.** Mapeando RAM por encima de IRAM1 (`IRAMSPILL`,
ayuda de medición, no arreglo) la corrida continúa y se cuentan **1178 escrituras**
por encima del límite, todas desde ese mismo PC: unos 2,3 KB. No es una fuga
descontrolada; el puntero de destino acaba una cantidad fija demasiado alta. Eso es
la forma que tiene una copia de anillo que debería partirse en dos tramos (cola hasta
el final del búfer y cabeza desde el principio) y se hace de una sola vez.

**Por qué importa el orden.** Una nota de método: la primera vigilancia de escrituras
sobre `0x22040000-0x22040800` dio **cero** y llevó a concluir que el abort era de
lectura. Era la ventana, que se quedaba corta: el puntero ya iba por `0x2204221x`.
Ampliada a `0x22060000` aparecieron las 1178.

**Lo que queda.** De dónde sale ese destino: si el firmware lo calcula a partir de un
registro que este modelo contesta mal (el candidato natural es la posición de SM1, que
ya dio un fallo de esta misma familia en §11.24), entonces es un fallo nuestro y se
arregla; si lo calcula solo, hay que ver qué le hace creer que el anillo es 2,3 KB más
grande de lo que es.

## 11.37 Con el desbordamiento sobrevivible: se acaban los reinicios, el render dobla

Respaldando la región por encima de IRAM1 (`IRAMSPILL=0x100000`) para que el
desbordamiento de §11.36 no aborte, y con `SHA1_ZERO=1` para que la biblioteca cargue:

| | antes | ahora |
|---|---|---|
| `resets` | 1 | **0** |
| excepciones | 1 Data Abort | **ninguna salvo IRQ** |
| `copy16` | 4 809 | **9 616** |
| `copyent` | 258 | **456** |
| `pcmout` | 4 | **8** |

Es decir: **ese único abort era lo que cortaba la corrida**, y sin él el pipeline de
render corre justo el doble. Eso confirma §11.36 de la otra dirección.

**Pero sigue sin haber música.** El WAV de esa corrida: 17 472 muestras no nulas de
10,8 M (0,161 %), pico 11444 -- la firma de los clics de rueda otra vez. Y
`framepull` sigue en **1**.

**Lo que eso descarta.** El "decodificador que saca una trama y se para" **no era el
reinicio**. Sin reinicio, sin abort y con el render corriendo el doble, el códec
sigue ejecutándose una sola vez. Son dos fallos independientes, y §11.32 los había
juntado en uno.

Queda por tanto el lado del decodificador: `credread=13` y `cred1=cred4=2` dicen que
la ruta de crédito sí se recorre, `pullpass=1` que el tirón pasó su puerta una vez, y
`fpull2` (`0x081c1508`) sigue en **0** -- ese segundo punto de tirón no se ejecuta
nunca.

**Y el crédito NO es cero.** Capturado el retorno de la lectura en `0x081e9bd0`, once
veces seguidas, siempre lo mismo:

```
r0=0x00000004  r1=0x0ba95574  r2=0x2200ac64  r5=0x0ba90c98
```

Constante 4. Así que la teoría de "se queda sin crédito y espera" tampoco se sostiene:
el bucle recibe un valor estable y aun así **no vuelve a llamar a `framepull`**. El
`r2=0x2200ac64` es IRAM0, con pinta de objeto del kernel (semáforo o evento), y el
valor 4 tiene más aspecto de código de retorno que de cuenta.

**ESTADO al cierre de esta sesión.** Con `SHA1_ZERO=1` + `IRAMSPILL`: biblioteca
cargada (4 canciones), reproducción iniciada, **cero reinicios**, render corriendo al
doble... y sólo clics en el WAV. El códec se ejecuta **una vez** y el flujo se va a un
bucle en `0x081e9bc0` del que no sale. Eso, y no el reinicio, es lo que queda entre
este emulador y la música.

## 11.38 El getter de 0x081e9bc0 NO es el atasco (refutado con la sonda)

Leyendo el desensamblado, `0x081e9bc0` resultó no ser un bucle sino un **getter** con
tres salidas distinguibles:

```
81e9bc0: bl 0x81c0fd4        credito -- mide 4, asi que NO es esta la salida
81e9bd8: ldr r1,[[r4]+0x48]  ranura virtual
81e9be4: blx r1              -> r5 = siguiente bufer
81e9bfc: ldrb r0,[r4,#0x30]
81e9c00: cmp r0,#3
81e9c04: beq 0x81e9c34       -> DEVUELVE NULL
81e9c08: cmp r5,#0
81e9c0c: beq 0x81e9bf0       -> bufer NULL: bombea 0x81e96d0 y reintenta
```

Y `0x81e96d0` es un despachador sobre ese mismo byte `[obj+0x30]`: estados 0, 1 y 2
hacen trabajo (`0x81e92cc`, `0x81e9580`, `0x81ea394`); con **3 no hace nada y vuelve**.
De ahí la hipótesis natural: el objeto llega a estado 3 ("terminado") tras una trama y
por eso no hay segunda.

**Medido en `0x081e9bfc`, es falso.** El estado **avanza**:

```
slot#1  r5=0x0ba7564c  state=0        slot#5  r5=0x0ba7564c  state=1
slot#2  r5=0x0ba7564c  state=0        slot#6  r5=0x0ba7564c  state=1
slot#3  r5=0x0ba7564c  state=0        slot#7  r5=0x00000000  state=2  (reintenta)
slot#4  r5=0x00000000  state=1  (reintenta)   slot#8  r5=0x0ba7564c  state=2
```

0 -> 1 -> 2, con búferes reales, y los NULL son justo las transiciones donde el
bombeo hace su trabajo. Es la **secuencia de arranque funcionando**, no un atasco.
Nunca llega a 3 en la ventana medida.

Así que el "el códec corre una vez" no está aquí. Sigue sin localizarse.

## 11.39 Prueba de navegación completa, con la biblioteca cargada

`run_nav.sh` con `SM1_IRQ=1 SHA1_ZERO=1 IRAMSPILL=0x100000`, pulsaciones verificadas
en cada paso (§11.34):

| paso | resultado |
|---|---|
| selector de idioma | superado |
| menú principal | correcto |
| entrar en Música | correcto |
| bajar a Canciones (6ª entrada) | correcto |
| **lista de canciones** | **Greatest Hits / Michael Jackson: Billie Jean 4:55, Beat It 4:58, Thriller 13:41, Smooth Criminal 9:25** |
| seleccionar canción | entra en la animación de Reproduciendo |
| +8 s / +48 s | **se queda en la portada**, no llega a la vista con barra de progreso |

Contadores: `StartPlayback=1`, `framepull=3`, `pullpass=3`, **`pullbail=3`**,
`copy16=4808`, `resets=0`.

**Dos cosas que esto establece.** La primera: la biblioteca, los títulos, el artista,
el álbum y **las duraciones** son correctos en pantalla, así que
`tools/ipod-library/patch_tracks.py` hace su trabajo y el iTunesDB se lee bien. La
segunda: entrando por Canciones el tirón de trama corre **tres** veces en vez de una
(por Aleatorio corría una), pero **las tres salen por `pullbail`** (`0x081e9470`), la
rama de abandono. Antes `pullbail` no se distinguía de `pullpass`; aquí van
emparejados 3 y 3.

Esa asimetría -- una ruta de navegación tira más veces que otra, y todas abandonan --
es la pista concreta que queda: `0x081e9470` es la salida que hay que seguir.

## 11.40 La cola SÍ se alimenta -- y la ventana que lo negaba estaba mal puesta

§11.39 dejó `framepull` pasando sus dos puertas y el bucle de `0x081e9460`
ejecutando el códec una vez por unidad de profundidad de cola. Y una nota de método
que costó una medida entera.

**El error.** `credread` se llamaba con `0x0ba90c98`, y lo leí como `obj+0x48` de un
objeto en `0x0ba90c50`. Falso: `0x0ba90c98` **es** el objeto (`[r4+4]`). Vigilando
`0x0ba90c88-0x0ba90c98` sólo aparecieron **ocho** escrituras, todas en el arranque,
desde PCs con `bic #0xC0000003`, división de bloques y contadores de estadísticas --
es decir, **cabeceras del asignador de memoria**, no una cola de audio. La conclusión
"el productor no encola nunca" era un artefacto de mirar 0x40 bytes más abajo de lo
que tocaba.

**Con la ventana correcta** (`0x0ba90cd0-0x0ba90ce0`, o sea `obj+0x38..obj+0x48`) la
cola está viva durante toda la reproducción:

```
0x081c0940 / 0x081c0944  ->  +0x38 / +0x3c   (lado productor)  -- repetidamente
0x081e9f48 / 0x081e9f50  ->  +0x40 / +0x44   (tras el codec)
0x081e9f68 / 0x081e9f88  ->  +0x40 / +0x44   (otra ruta, mas tarde)
```

Con marcas a 65,32 s, 65,33 s, 80,76 s, 80,84 s... El productor corre, el consumidor
actualiza, y aun así no sale música.

**Lección de método, la tercera de esta clase.** Una ventana de vigilancia mal puesta
no da error: da CERO, y el cero parece un hallazgo. Ya pasó con
`0x22040000-0x22040800` (§11.36), donde el silencio hizo concluir que el abort era de
lectura cuando el puntero ya iba por `0x2204221x`. Antes de creerse un "nadie escribe
aquí", hay que confirmar la dirección base con una lectura independiente.

## 11.41 La causa de que todo vaya lentísimo: una TORMENTA de interrupciones

Siguiendo la cadena desde el códec hacia arriba, con el desensamblado en la mano:

- `framepull` (`0x081e93d8`) pasa sus dos puertas; su bucle en `0x081e9460` ejecuta
  el códec **una vez por unidad de profundidad de cola** ("while `[obj+0x40]` != r6",
  r6 desde `[obj+0x3c]` bajando). Medido prod=3 cons=2 -> profundidad 1 -> una llamada.
  Eso explica `aacmath=1` **sin necesidad de ningún fallo en el códec**.
- La profundidad la fija `0x81c08bc` a partir de dos campos: `[obj+0x2b4]` (posición)
  y `[obj+0x2b8]` (límite). Medidos: límite **4**, posición **0**.
- La posición no está muerta: avanza 0 -> 1 -> 2 -> 3, de uno en uno
  (`adds r2,r1,r2` en `0x81cc404`, con reposición a cero si sale negativa). Sólo que
  avanza **tres veces en toda la corrida**.

**Todo va unas cuatro órdenes de magnitud demasiado lento. Y aquí está por qué:**

```
credithdlr (manejador de la IRQ linea 18, 0x08017378)  = 1 196 485
creditsig  (donde señala el evento, 0x080173cc)        =       490
dpump      (bomba del render)                          =         8
SM1_IRQ: line 18 raised                                =         4
```

El modelo levanta la línea **cuatro veces** y el firmware entra al manejador **1,2
millones**. El log `-d int` de QEMU lo confirma por otra vía: ~1,05 millones de
excepciones IRQ. La línea se queda asertada los 5 ms que van hasta el siguiente tick
y el invitado reentra sin parar durante toda esa ventana, ahogando a los hilos de
audio. Por eso `dpump` corre 8 veces mientras el manejador corre un millón.

**Arreglo parcial, medido.** El reconocimiento por lectura estaba limitado a `+0xa98`
porque el getter del manejador (`0x080a7638`) lee ese registro. No basta: ampliado a
**cualquier** lectura del bloque SM1, las entradas bajan de 1 196 485 a **789 182**
(-34 %). Sigue siendo una tormenta, así que la línea la mantiene o la re-despacha
algo que no es nuestra subida -- el modelo del VIC, o un segundo usuario de la línea
18. Esa es la pista viva.

**Lo que esto reordena.** El "códec que corre una vez" no es un fallo del códec ni de
la cola: es la consecuencia aritmética de una profundidad de 1, que a su vez viene de
una posición que apenas avanza porque el sistema está ahogado. Hay **un** problema,
no tres.

## 11.42 La tormenta, resuelta: de 1 196 485 entradas a 2 399

§11.41 dejó la tormenta medida pero viva. La forma de la línea era el problema, y
dos experimentos la acotan:

**Un pulso no vale.** Con `SM1_IRQ_PULSE=1` (subir y bajar en el mismo instante) el
manejador entra **cero** veces y `creditsig` también cae a cero: el VIC quiere
**nivel**, un flanco se pierde. Dato limpio, y descarta la solución obvia.

**Lo que sobraba era el tiempo.** La línea se sostenía hasta el siguiente tick de
5 ms, y ~197 000 reentradas por subida sólo son posibles porque el tiempo virtual
apenas avanza mientras la CPU gira en el bucle de interrupción. Un temporizador
grueso es exactamente lo que no debe sostener una línea de interrupción. Acotando la
ventana con un one-shot propio:

| ventana | entradas al manejador | `creditsig` | `dpump` | `copy16` |
|---|---|---|---|---|
| hasta el tick de 5 ms | 1 196 485 | 490 | 8 | 4 808 |
| ídem + ack en cualquier lectura | 789 182 | 493 | 8 | 4 808 |
| **250 us** | 43 750 | 482 | 8 | 4 808 |
| **20 us** | **2 399** | **2 399** | **16** | **9 616** |

**500 veces menos**, y el detalle que importa: a 20 us `creditsig == credithdlr`, o
sea **cada interrupción hace trabajo útil**, frente a 490 productivas de 1,2 millones.
El pipeline de render vuelve a doblar: `dpump` 8 -> 16, `rendret` 4 -> 8, `pcmout`
4 -> 8, `copy16` 4 808 -> 9 616.

`S5L8702_SM1_IRQ_ASSERT_US` es una **elección de modelo**, marcada como tal: lo que
hace la línea del motor real está SIN DETERMINAR, y qué toca el firmware para
reconocerla no se ha encontrado -- el getter del manejador (`0x080a7638`) no parece
llegar a este bloque, o ampliar el reconocimiento a cualquier lectura habría acabado
con la tormenta en vez de recortarla un tercio.

**Y sigue sin haber música.** `aacmath` y `framepull` siguen en **1**, y el WAV
sigue trayendo sólo clics (18 720 muestras no nulas, pico 11444). Quitado el ahogo,
la cola de decodificación sigue con profundidad 1. La tormenta era una causa real de
lentitud -- 500 veces -- pero no era la única que separa a este emulador de la música.

## 11.43 Lo que queda, acotado: la cola sigue con profundidad 1

Con la tormenta resuelta (§11.42) el sistema ya no está ahogado, y la pregunta se
estrecha. Medido después del arreglo:

- `credithdlr` = `creditsig` = **2 399**: cada interrupción del motor señala su evento.
- `dpump` = **16**, `copy16` = **9 616**: el render responde y ha doblado.
- `aacmath` = `framepull` = **1**, WAV con 18 720 muestras no nulas de 12,9 M -- clics.

Y la cantidad que gobierna todo sigue igual: muestreando `0x081c08b4` a lo largo de
la corrida (impactos 1, 8, 32 -- el getter sólo se llama unas decenas de veces),
`pos[+0x2b4]` = **0** y `limit[+0x2b8]` = **4**, invariables. La posición sí avanza
--medido en `0x81cc404`, de uno en uno-- pero **tres veces en toda la corrida**.

**Corrección de una etiqueta.** `framepull` (`0x081e93d8`) **no** es la bomba de
decodificación. Se le llama desde `blx r2` en `0x081ea384` --una llamada virtual-- y
sólo cuando `byte[r4+0x1c]` **cambia de estado**. Que valga 1 es lo esperado: es una
notificación de flanco. Perseguirlo como si fuera un bucle de decodificación fue otra
suposición mía puesta en un nombre.

**El siguiente hilo, concreto:** 2 399 señales de evento producen 16 vueltas de la
bomba de render. Entre `creditsig` (`0x080173cc`, que señala el evento `0x22008c58`) y
`dpump` (`0x22006118`) hay un factor 150 sin explicar. Ahí, y en qué incrementa
realmente `[obj+0x2b4]`, está lo que falta.

## 11.44 Corrección: "cuatro subidas de línea" era un recuento mal hecho

Las secciones 11.41-11.43 citan "4 subidas de línea" frente a 1,2 millones de
entradas al manejador, y de ahí sale la cifra de ~197 000 reentradas por subida. **El
recuento estaba mal.** El `printf` del modelo está filtrado:

```c
if (s->irqs <= 4 || s->irqs % 2000 == 0)
```

así que contar líneas del log no cuenta subidas. Los números reales:

| corrida | última línea impresa | subidas reales | entradas al manejador |
|---|---|---|---|
| `pi1` (antes) | `raised #4` | **< 2000** | 1 196 485 |
| `bd2` (20 us) | `raised #2000` | **2000-4000** | 2 399 |

**La conclusión no sólo se mantiene: mejora.** Tras el arreglo hay ~una entrada al
manejador **por subida**, que es exactamente lo que debe ser. Y explica lo que antes
no cuadraba: si el modelo tiquea cada 5 ms y sube cada quinto tick, en 100 s debería
subir ~4000 veces -- pero antes del arreglo sólo llegaba a 4. **La tormenta congelaba
el tiempo virtual**, así que el propio temporizador del motor apenas corría. Causa y
efecto se realimentaban.

La posición del I2S lo confirma por otra vía: entre la primera subida y la número
2000, `pos` va de 3 251 200 a 5 453 824 stereo frames, o sea **50 segundos de audio
reloj­eados** en esa parte de la corrida. El camino de salida marcha a ritmo real.

**Lo que sigue sin cuadrar, y es el hilo vivo:** ~2400 interrupciones señalan su
evento y la bomba de render (`dpump`) corre **16** veces. El render mueve 9 616
medias palabras -- unos 19 KB -- donde 100 s de audio necesitan 17,6 MB. Está al
0,1 % del ritmo necesario mientras el DMA drena silencio a tiempo real.

## 11.45 La cadena entera, con un número en cada eslabón

Con la tormenta resuelta y los recuentos corregidos, la ruta completa queda medida de
punta a punta en una sola corrida (`pc1`):

| eslabón | medida | ¿sano? |
|---|---|---|
| I2S / DMA sacando muestras | 50 s de audio relojeados en la ventana medida | **sí, tiempo real** |
| interrupción del motor SM1 | 2000-4000 subidas, ~1 entrada al manejador cada una | **sí** (§11.42) |
| `creditsig` señalando el evento `0x22008c58` | **1 822** | **sí** |
| **`dpump`, la bomba de render (`0x22006118`)** | **16** | **NO -- aquí está el corte** |
| `posfn` (`0x081cc360`, avanza la posición) | 12, todas desde `0x081f6e94` | consecuencia |
| `posadd` (`0x081cc404`, el incremento) | 6 | consecuencia |
| profundidad de cola | 1 | consecuencia |
| códec (`aacmath`) | 1 | consecuencia |
| WAV | 18 720 muestras no nulas de 12,9 M -- clics | consecuencia |

`posfn` tiene exactamente **dos** llamadores en toda la imagen, `0x081d7874` y
`0x081f6e94`; el primero **no se ejecuta nunca** y el segundo corre 12 veces.

**El corte es un factor ~114 entre `creditsig` y `dpump`.** El motor señala su evento
1 822 veces y la bomba que espera en él arranca 16. Todo lo que viene después --
posición, profundidad de cola, llamadas al códec, muestras -- sale de ahí por
aritmética. Ya no hay que buscar en el códec, ni en la cola, ni en la salida: los tres
están medidos y son consecuencia.

**Dónde mirar:** qué hace `0x080a1a24` con el objeto `0x22008c58` (la señal), frente a
qué espera exactamente la bomba en `0x22006118`. Si son objetos distintos, o si la
semántica del evento RTXC es de "un solo disparo" que se pierde mientras el que espera
no está bloqueado, ahí está la explicación.

## 11.46 La línea 18 queda CONFIRMADA por el propio firmware

`S5L8702_SM1_IRQ = 18` se eligió porque la tabla de Rockbox tiene un **hueco** en 18,
y eso nunca fue prueba de nada. Ahora sí lo es. En el arranque del render, en IRAM:

```
220069c4:  mov r0,#8       ; bl 0x22003870    crea el objeto
220069cc:  mov r1,#18      ; bl 0x22003888    numero de IRQ: 18
220069d4:  ldr r1,=0x08017378 ; bl 0x22003890 registra ESE manejador
220069e4:  ldr r1,=0x08017378 ; bl 0x22003898 lo habilita
220069ec:  ldr r0,=0x22008c58 ; bl 0x220037f8 crea el evento
```

El firmware registra `0x08017378` -- el manejador que hemos estado contando -- en la
**IRQ 18**, y a continuación crea el evento `0x22008c58` que ese manejador señala.
La eleccion de linea era correcta, y ya no por descarte.

Queda por tanto una sola pregunta abierta en toda la ruta: **quién espera en ese
evento y por qué arranca 16 veces cuando se le señala 1 822** (§11.45). El objeto se
pasa por puntero, así que el que espera no se localiza con una búsqueda del literal
`0x22008c58`; hay que seguir el manejador devuelto por `0x220037f8`.

## 11.47 Quién espera en el evento -- y el bit 12, refutado

Siguiendo el literal `0x22008c58` fuera de IRAM aparece el que espera, en
`0x0808af58`:

```
808af6c:  bl 0x80a7638          lee el MISMO estado que el manejador de la IRQ
808af74:  tst r0, #0x1000       comprueba el BIT 12
808af78:  beq 0x808afa0         si no esta: se salta el bloque de proceso
808af7c:  ... bl 0x80a35a0 ; bl 0x809a72c(1)      <- trabajo por trama
808afb0:  ldr r0,=0x22008c58 ; mov r1,#500
808afb8:  bl 0x80bf2b8          espera en el evento, timeout 500
```

Dos consumidores del mismo registro de estado con **bits distintos**: el manejador de
la línea 18 mira el **bit 2**, este otro el **bit 12**. El modelo sólo contestaba el
bit 2, así que ese bloque de trabajo por trama no se ejecutaba nunca.

**Parecía la pieza que faltaba. Medido, no lo es.** Encendiendo el bit 12 mientras hay
una trama pendiente: `dpump` 16 -> **14**, `aacmath` sigue en **1**. Ninguna mejora.
Así que bit 12 significa otra cosa, y queda **apagado por defecto** -- inventar un
estado que la pieza quizá nunca reporta es exactamente lo que no se debe hacer. Se
conserva como interruptor (`SM1_FRAME_BIT=1`) porque el hallazgo -- que este esperador
condiciona en el bit 12 mientras el manejador condiciona en el bit 2 -- es real y
conviene poder volver a probarlo.

También queda visto que la espera lleva **timeout 500**, así que no es un bloqueo
eterno: el que espera se despierta solo aunque nadie señale. Eso descarta que el corte
sea un deadlock en ese punto.

## 11.48 Corrección: el corte no está en el evento, está por encima

§11.45 situó el corte en el factor ~114 entre `creditsig` (1 822) y `dpump` (16), y
§11.47 fue a buscar la semántica del evento. **Las dos apuntaban al sitio
equivocado.**

El que espera (`0x0808af58`) se invoca a través de un trampolín en IRAM
(`0x22003850`: `ldr pc,[pc,#-4]` seguido de la dirección), así que se puede contar:

```
waitcall  (trampolin 0x22003850)  =    8
waitfn    (0x0808af58)            =    8
dowait    (0x0808afb0, la espera) =    8
bit12set  (rama del bit 12)       =    0
creditsig (senales del evento)    = 1841
dpump                             =   16
```

**Al que espera sólo se le LLAMA ocho veces.** No es que la espera falle ni que se
pierdan señales: las 1 841 señales nunca iban a consumirse porque **nadie estaba
esperándolas**. Buscar la semántica del evento RTXC habría sido tiempo perdido.

El corte está más arriba: **el lazo de la tarea que debería bombear audio gira ocho
veces en toda la corrida**. Todo lo de aguas abajo -- la espera, el evento, la
posición, la profundidad de cola, el códec, las muestras -- es consecuencia de eso.

Y esto explica por qué ninguna de las mejoras de aguas abajo movió `aacmath`: la
tormenta (§11.42) hizo que el sistema respirara y el render doblara, pero no cambió
cuántas veces gira ese lazo.

**Dónde mirar ahora:** quién llama al trampolín `0x22003850` y por qué su lazo termina
tras ocho vueltas. Es una tarea del RTXC; el patrón a buscar es una condición de
salida del bucle, no un bloqueo -- la espera de dentro lleva timeout 500 y por tanto
nunca cuelga indefinidamente (§11.47).

## 11.49 La espera NO falla: tiene éxito siempre. Sube otra vez el objetivo

§11.48 dejó al que espera invocado sólo ocho veces, y el lazo que lo llama parecía
explicarlo:

```
22006324:  bl 0x22003850      llama al que espera
22006328:  cmp r0, #0
2200632c:  beq 0x22006310     devolvio 0 = fallo: dormir y reintentar
22006310:  mov r0,#10 ; bl ... dormir
2200631c:  cmp r7, #1
22006320:  bgt 0x220061fc     tras DOS intentos, abandonar
```

De ahí salió la hipótesis natural: la espera (timeout 500) vence, y el lazo se rinde
a los dos reintentos. **Medido, es falsa.**

```
loopcall (0x22006324, la llamada)        = 8
waitok   (0x22006330, la rama de EXITO)  = 8
```

`waitok == loopcall`: **la espera tiene éxito las ocho veces.** La rama de reintento
no se toma nunca. Y subiendo la cadencia del motor a `SM1_IRQ_EVERY=1` --
`creditsig` de 1 841 a **8 180** -- ni `dpump` ni `aacmath` se mueven. La cadencia no
es el motor de nada aquí.

**Así que el objetivo sube otra vez.** El lazo no se rinde: sencillamente **la función
que lo contiene sólo se ejecuta ocho veces**. `dpump` (`0x22006118`) corre 16 veces y
dentro de ella esta espera ocurre 8, siempre con éxito. La pregunta viva es quién
llama a `0x22006118` y por qué sólo dieciséis veces.

**Advertencia de método, otra vez la misma.** Leí el `beq` de reintento como el camino
habitual sin comprobarlo. Contar la rama de éxito costaba una corrida y habría evitado
toda la teoría del timeout. Antes de razonar sobre qué rama toma un salto, hay que
contarla.

## 11.50 La tarea de audio se planifica cada ~20 SEGUNDOS

`0x22006118` no aparece como literal ni como destino de `bl` en ningún sitio: se le
llama por un puntero construido en ejecución, así que el desensamblado no lo
encuentra. Capturado en vivo con `regpc`, el llamador es **único**:

```
lr=0x22002ff8   r0=0x220106dc  r1=0x22006118  r5=0x22008c10
```

`r1` es la propia dirección de la bomba pasada como argumento: `0x22002ff8` es un
**despachador de tareas** del RTXC. Y los tiempos son lo que importa:

```
+80.597 s  +80.760 s  +80.804 s  +80.814 s  +80.864 s  +80.874 s  +80.925 s  +80.935 s
+98.428 s  ...                                          <- hueco de 17,5 s
+119.650 s ...                                          <- hueco de 21 s
```

Ráfagas de pares separados ~10 ms, y entre ráfaga y ráfaga **17 a 21 segundos**. La
tarea que debe bombear audio se planifica unas tres veces por corrida en vez de unas
cuarenta veces por segundo. Ese factor de ~1000 es del mismo orden que el déficit de
audio (19 KB movidos donde harían falta 17,6 MB), así que es la explicación, no un
síntoma más.

**Descartado de camino: el ritmado de audio.** `AUDIOPACE` compuerta la línea de
petición de DMAC0 con un temporizador al ritmo de muestreo, así que era sospechoso de
estrangular la cadena. Medido al revés: **sin** él todo empeora (`dpump` 16 -> 4,
`copy16` 9 616 -> 2 404). Ayuda, y debe quedarse puesto.

**El hilo, ahora sí acotado a una pregunta de planificación:** por qué el despachador
`0x22002ff8` deja pasar 20 segundos entre activaciones de esta tarea. Los candidatos
naturales son el tick del kernel -- el canal de 1 kHz del temporizador (ECLK/2,
DATA0 = 6000) -- y cualquier `sleep` que la tarea pida en ticks: si un tick nuestro
dura mil veces lo que debe, una siesta de 10 ms se convierte en 20 segundos.

## 11.51 El temporizador está bien: la hipótesis del tick, refutada

§11.50 dejó como candidato principal un tick de kernel demasiado lento -- si un tick
durase mil veces lo debido, la siesta de 10 ms de la tarea de audio se convertiría en
los 20 segundos medidos. `TIMERTRACE=1` lo zanja:

```
chan A   freq=6000000 Hz  data0=6000  pre=0   -> periodo 1.000 ms
chan B   freq=6000000 Hz  data0=6000  pre=0   -> periodo 1.000 ms
chan E   freq=1000000 Hz  (libre, 32 bits)    -> el contador de microsegundos
avisos "selects PCLK": 0
```

El tick del kernel corre a **exactamente 1 kHz**, que es lo que debe, y el contador de
microsegundos a 1 MHz. Y **ningún** canal selecciona PCLK, así que la sustitución
ECLK-por-PCLK que documenta `chan_freq` -- la parte no modelada -- nunca llega a
aplicarse en esta ruta. El modelo de temporizadores no es el problema.

**Balance de lo descartado, todo con medida:** el códec, la cola de decodificación, el
camino de salida, la firma del iTunesDB, el bit 12 del estado, el crédito, la
semántica del evento, el timeout de la espera, la cadencia del motor SM1, el ritmado
de audio y ahora el reloj del sistema.

**Lo que queda en pie:** la tarea de audio se activa en ráfagas separadas 17-21
segundos (§11.50), y los instantes medidos -- 80,9 s, 98,4 s, 119,6 s -- caen dentro
del guion de navegación del arnés, que duerme 6, 15 y 15 segundos entre pulsaciones.
Eso sugiere que la tarea se está despertando **cuando algo del interfaz la toca**, no
sobre un reloj de audio propio. Comprobarlo es la siguiente medida: una corrida que
llegue a reproducir y luego no toque ninguna tecla, y ver si las ráfagas siguen
apareciendo o se detienen.

## 11.52 CONFIRMADO: la tarea de audio no tiene reloj propio

La sospecha de §11.50 -- que las ráfagas coincidían con las pulsaciones del arnés --
se comprueba directamente con `run_quiet.sh`: llegar a reproducir y después **no
emitir ni un solo mensaje por el monitor** durante dos minutos (ni siquiera una
captura, que es un viaje de ida y vuelta al monitor).

```
                        con actividad de interfaz   tras 120 s SIN TOCAR NADA
dpump                            16                            8
copy16                        9 616                        4 808
aacmath                           1                            1
```

**Dos minutos de silencio no producen ni una activación más.** La bomba de render no
se auto-reloja: cada vez que corre es porque algo del interfaz la ha despertado. En un
reproductor de verdad esa tarea tiene que girar unas cuarenta veces por segundo con el
aparato quieto en el bolsillo.

**Lo que esto significa.** El audio no está esperando datos, ni crédito, ni una
interrupción del motor -- todo eso está medido y sano (§11.45, §11.42). Está esperando
**un despertar que sólo llega como efecto colateral de la actividad de interfaz**. El
reloj propio que debería tener la reproducción no existe en esta emulación, o no llega
a esa tarea.

**Por dónde seguir, con lo que ya se sabe:** el despachador es `0x22002ff8` y le pasa
`0x22006118` en r1 con el objeto `0x220106dc` en r0. Hay que ver qué cola o
temporizador del RTXC alimenta a ese despachador, y por qué sólo se mueve cuando el
invitado atiende la rueda o el LCD. El temporizador del sistema NO es (§11.51: tick de
1 kHz exacto), así que el candidato es el propio mecanismo de encolado de tareas.

## 11.53 Tampoco es el DMA: el ISR corre 7 185 veces

Confirmado que la tarea de audio no se auto-reloja (§11.52), el candidato natural era
la interrupción de fin de transferencia de DMAC0: el canal de audio usa un LLI que se
apunta a sí mismo, y si el modelo PL080 no dispara la interrupción en cada recarga, la
tarea nunca recibiría su "búfer consumido".

**Medido, llega de sobra.** El ISR registrado en IRQ 16 (`0x22002810`):

```
dmairq    (0x22002810, ISR de DMAC0)   = 7 185
creditsig (evento del motor SM1)       =   394
dpump     (bomba de render)            =     8
```

Siete mil interrupciones de DMA y casi cuatrocientas señales del motor, y la bomba
corre ocho veces. **Ninguna de las dos fuentes periódicas despierta a esa tarea.**

**El inventario de fuentes de reloj queda agotado:** el temporizador del sistema es
correcto (§11.51), la interrupción del motor SM1 llega y se atiende (§11.42), la
interrupción del DMA llega 7 185 veces, y el ritmado del I2S ayuda en vez de
estorbar (§11.50). La tarea sencillamente no está colgada de ninguna de ellas en esta
emulación.

**Lo que queda por hacer, y no cabía en esta sesión:** seguir el despachador
`0x22002ff8` hacia atrás -- qué cola del RTXC lee, quién la llena -- en vez de seguir
probando fuentes de interrupción una por una. Las cuatro que existen ya están
descartadas por medida, así que el fallo está en el encolado, no en la señal.

## 11.54 La cadena entera, del altavoz a la cola de trabajos

Estado al cierre de la sesión, con cada eslabón medido:

```
[ARRIBA]  quien encola trabajos     0x220067b8 / 0x220067fc   4 veces cada uno  <-- AQUI
              |  (count++ en 0x22008c1c, luego senal a 0x220106d0)
          cola de trabajos          0x22008c1c                8 items
              |
          despachador               0x22002fe0..0x2200302c    lazo "while count>0"
              |  (bl 0x22005dec, que salta a la bomba)
          bomba de render           0x22006118                8 vueltas
              |
          espera del evento         0x0808af58                8 llamadas, 8 EXITOS
              |
          posicion / profundidad    [obj+0x2b4]=0 [obj+0x2b8]=4   profundidad 1
              |
          codec                     0x081e9e70                1 vez
              |
[ABAJO]   WAV                       solo clics
```

El despachador es un lazo `while (count > 0) { procesar; count-- }` sobre
`[r5+0x0c]` con `r5 = 0x22008c10`, o sea la cuenta en **`0x22008c1c`** -- el mismo
contador que una sesión anterior ya había señalado. Vigilando esa palabra salen
exactamente cuatro ráfagas, dos encolados y dos desencolados cada una: **ocho items,
ocho vueltas de la bomba**. Cuadra perfecto y no queda nada por explicar aguas abajo.

**Todo lo de abajo está medido y sano o descartado:** el códec, la cola de
decodificación, el evento, la espera, el camino de salida, el DMA (su ISR corre 7 185
veces), el motor SM1, el temporizador del sistema y el ritmado del I2S. **El fallo
está arriba del todo: nadie encola trabajo de audio salvo cuatro veces.**

**Por dónde entrar la próxima vez:** `0x220067b8` está en una rutina que antes limpia
una fila de campos (`[r4+0x2c]`..`[r4+0x80]`, `[r4+0x8c]`) -- tiene forma de
arranque/reinicio, no de bombeo periódico. `0x220067fc` incrementa además un contador
propio en `[r0+0x68]`. Ambos terminan señalando `0x220106d0`. Lo que hay que averiguar
es qué debería llamarlos cuarenta veces por segundo durante la reproducción, y no lo
hace.

## 11.55 Un eslabón más: quien encola es otra tarea, y tampoco gira

La función que encola es `0x22006574` (prólogo `push {r4,r5,r6,r7,r8,lr}`, epílogo en
`0x220067cc`). No aparece como destino de `bl` en el desensamblado -- otra llamada por
puntero -- así que capturada en vivo:

```
lr=0x2200591c  r0=0x220106dc  r1=0x093b34c0  r3=0,1,2,3,5,6,7  sp=0x093b33d0
   8 llamadas, todas desde el mismo sitio
```

- `r0 = 0x220106dc` es **el mismo objeto de audio** que recibe la bomba de render.
- `r1` es un mensaje, y `r3` su número de secuencia: 0, 1, 2, 3, **5**, 6, 7 -- falta
  el 4, así que un mensaje se posteó por otra vía o se perdió.
- `sp = 0x093b33d0` está en SDRAM: **es otra tarea**, no el despachador (que corre en
  pila de IRAM).

O sea que quien produce trabajo de audio es a su vez una tarea que sólo se ejecuta
ocho veces. El patrón se repite un nivel más arriba y la sesión se acaba aquí.

**Resumen del estado para retomarlo.** La ruta completa, con un número medido en cada
eslabón, está en §11.54. El punto de entrada es `0x22005918` -- el sitio que llama al
encolador -- y la pregunta sigue siendo la misma en cada nivel: **qué debería estar
girando cuarenta veces por segundo durante la reproducción y no gira**. Todas las
fuentes de reloj del hardware están descartadas por medida (§11.51, §11.53), así que
lo que falta es del lado del software: una tarea, o una cadena de tareas, que no se
está replanificando.

## 11.56 La cadena, completa desde el WAV hasta el firmware

Trazada de abajo arriba, con un número medido en cada eslabón y el llamador capturado
en vivo donde el desensamblado no bastaba (varias llamadas son por puntero):

```
0x081fef28   sitio de envio en el firmware          3 veces (+1 arranque 0x081febe4)
     |       r0=0x0bae8ad8  r2=0x7fffffff
0x081d0944   arma el mensaje (marco de 372 bytes)   4 veces
     |       bl 0x8015b18 -> veneer a IRAM
0x22005674   postea a la cola                       4 veces
     |       bl 0x22005d04 desde 0x22005918
0x22006574   encolador                              8 veces
     |       count++ en 0x22008c1c, senal a 0x220106d0
0x22008c1c   cola de trabajos                       8 items
     |
0x22002fe0   despachador  while(count>0){...count--}
     |
0x22006118   bomba de render                        8 vueltas
     |
0x0808af58   espera del evento 0x22008c58           8 llamadas, 8 EXITOS
     |
[obj+0x2b4]=0 [obj+0x2b8]=4                         profundidad de cola 1
     |
0x081e9e70   codec                                  1 vez
     |
WAV                                                 solo clics
```

**Todo eslabón por debajo del envío es correcto y consistente**: la aritmética cuadra
en cada paso (4 envios -> 8 items -> 8 vueltas -> profundidad 1 -> 1 llamada al
codec), y las fuentes de reloj del hardware están descartadas una por una por medida
(§11.51 temporizador, §11.53 DMA, §11.42 motor SM1, §11.50 ritmado I2S).

**El fallo está en la cima: `0x081fef28` envía tres trabajos en toda la corrida** donde
un reproductor necesita unos cuarenta por segundo. `r2 = 0x7fffffff` en cada llamada
tiene pinta de "sin límite/sin plazo". El siguiente paso es entender qué gobierna ese
sitio -- qué condición o qué evento debería hacerlo repetir -- y ahí ya se está en
código de reproductor, no de plumbing del kernel.

**Nota sobre `IRAMSPILL`.** Todas estas medidas se tomaron con esa ayuda puesta, que
respalda memoria por encima de IRAM1 para que el desbordamiento de `copy16` (§11.36) no
aborte. **No es un arreglo** y no debe quedarse: sin ella la corrida se corta a los
~46 s y no se puede medir nada de esto. El desbordamiento sigue siendo un fallo abierto
por derecho propio.

## 11.57 Conclusión de la sesión: no hay un eslabón roto, no arranca el lazo

`0x081fef28` resulta ser `blx r3` con `r3 = [[r4]+0xc8]`: una llamada **virtual**, la
tercera de una secuencia (+0xd4, +0xcc, +0xc8) con forma de "procesar un búfer". Y la
función que la contiene corre, como todo lo demás, tres o cuatro veces.

Ese es el patrón que cierra el diagnóstico. Recorriendo la cadena de §11.56 de arriba
abajo, **cada nivel se ejecuta un puñado de veces y la aritmética entre niveles cuadra
siempre**: 3-4 envíos, 4 mensajes, 8 items, 8 vueltas de bomba, profundidad 1, una
llamada al códec. No hay ningún sitio donde N entre y salga mucho menos que N.

**Es decir: no hay un eslabón roto. Lo que no ocurre es que el lazo de reproducción
vuelva a empezar.** Arranca, procesa unos pocos búferes y se queda quieto. Buscar el
fallo bajando por la cadena --que es lo que ha ocupado esta sesión-- estaba condenado:
cada nivel reflejaba fielmente al de arriba.

**Lo que eso sugiere para la próxima vez.** En vez de seguir subiendo por llamadores,
la pregunta útil es qué termina o suspende la reproducción justo después de empezar. Un
candidato con evidencia previa: el reproductor mostró la pantalla de **batería baja**
(§11.33) y la de "Reproduciendo" se queda en la animación de portada sin llegar a la
vista con barra de progreso (§11.39). Un estado de "parado" explicaría a la vez el
puñado de búferes y la UI congelada, y se comprueba mirando el estado del reproductor
en pantalla o en su objeto, no persiguiendo mas contadores.

**Estado del arbol al cierre.** Seis arreglos verificados y en `master` de la rama:
tormenta de IRQ (§11.42), motor SHA-1 (§11.35), alias de DRAM sin caché (§11.35),
ventana de 1 MB del bloque SHA-1, RTC del PMU (§11.33) y el data abort de `copy16`
localizado (§11.36, sin arreglar: `IRAMSPILL` sólo lo hace sobrevivible para medir).
Instrumentos nuevos: `SHA1TRACE`, `SHA1_ZERO`, `PMUTRACE`, `PMUBATT`, `TIMERTRACE`,
`QEMU_DEBUG`, `IRAMSPILL`, `SM1_IRQ_EVERY`, `SM1_IRQ_PULSE`, `SM1_FRAME_BIT`, y las
sondas `abort_probe`, `limit_probe`, `posinc_probe`, `slot_probe`, `dma_probe`,
`dma2_probe`, mas `run_nav.sh`, `run_quiet.sh` y `run_settle.sh`.

## 11.58 CORRECCIÓN de §11.57: el lazo de salida SÍ gira, 7 500 veces

§11.57 concluyó "no arranca el lazo". Medido directamente con una traza nueva en
el PL080 (`DMATRACE=1`, cuenta qué se programa en cada canal y cuántos
terminal-count levanta cada uno), eso es **falso para el lado de la salida**:

```
dmac0 ch1 ENABLE conf=0x00008a81 flow=1 srcid=0 dstid=10
          src=0x22008c5c  dst=0x3ca00010  lli=0x22010020  xsize=2048
dmac0 ch1 TC #7500 srcid=0 dstid=10
```

`dstid=10` es `IIS0_TX`; `0x3ca00010` es el FIFO del I2S. El canal drena un
**doble búfer de 4 KB en IRAM0** (0x22008c5c / 0x22009c5c) hacia el FIFO y
levanta más de **7 500** terminal-counts por corrida. Eso son ~15 millones de
muestras entregadas, que es exactamente el tamaño del WAV (32 MB). El consumidor
está sano y va al tempo correcto.

**Por qué el descarte anterior del DMA no valía.** §11.53 dio el DMA por bueno
porque "su ISR corre 7 185 veces". Una cuenta global del manejador no dice de qué
canal vienen las interrupciones: podían ser todas del display o del ATA. Ahora se
sabe que son del canal de audio, pero eso hubo que medirlo, no suponerlo. La
lección se repite en esta investigación: **un contador agregado no responde una
pregunta sobre un componente concreto.**

**Y un detalle que reordena el problema.** En la corrida `dma1` la reproducción no
llegó a arrancar siquiera (`StartPlayback hits=0`, `dpump=0`, `aacmath=0`) y aun
así el canal entregó 7 500 búferes. Es decir, **ese canal es una bomba de silencio
que corre desde el arranque**, independientemente de que haya o no reproducción.

**Dónde queda el fallo, entonces.** No en que el lazo no gire, sino en que **nadie
rellena de audio los búferes de IRAM0 que el DMA drena**. Y el sitio exacto ya
estaba anotado en la cabecera de `run_play.sh`: en el lazo del códec

```
81e9434: ldr r1, [r0, #0x44]     ; r0 = obj2
81e9438: ldr r0, [r4, #8]
81e9440: cmp r1, r0
81e9444: movle r0, r1            ; r0 = min(...)
81e9448: mov r3, r0              ; 3er argumento = cuenta de muestras
81e9458: bl 0x81e9e70            ; el codec
```

el códec **sí se llama, pero con `r3 = min([obj2+0x44], [r4+8])`, y ambos campos
están medidos a CERO**. Se le pide producir cero muestras. Así que la pregunta no
es quién consume ni cuándo, sino **quién debe rellenar esos dos contadores**.

## 11.59 REFUTADO: el formato de los MP3 no es la causa

Los cuatro ficheros a los que apunta la base los escribió `patch_tracks.py` con un
`shutil.copyfile` a secas: no comprueba que el formato del origen coincida con la
extensión del destino ni toca el campo de tipo del registro. Y los originales
descargados llevan delante un tag **ID3v2.4 de 682 KB** con carátula incrustada.
Dos razones plausibles para que el decodificador no sacara nada.

Prueba: reescritos los cuatro con `ffmpeg -map_metadata -1 -id3v2_version 0`,
MP3 CBR 128 kbps 44,1 kHz estéreo, **sin ningún tag** — empiezan directamente en
`fffb9064`, una trama MPEG-1 Layer III pelada. Instalados en las mismas rutas,
con los tamaños y duraciones parcheados en la base.

Resultado: la reproducción arranca (`StartPlayback`, `aacmath`, `framepull`,
`copy16` todos activos) y el WAV sigue conteniendo **sólo clics**: 33 696 muestras
no nulas de 26 122 218, pico 11444, y todos los recuentos por tramo son múltiplos
exactos de 1248. Ese 1248 y ese pico son la firma del clic de la rueda. **El
formato del fichero no es la causa.**

## 11.60 Dos trampas del banco de pruebas que costaron tres corridas

**`IRAMSPILL` es un número de BYTES, no un booleano.** `IRAMSPILL=1` mapea un solo
byte y la corrida se reinicia igual, con el mismo data abort de `copy16`. El valor
util es `0x10000`. El mensaje de arranque lo dice —"IRAMSPILL: 1 bytes mapped"—
pero pasa desapercibido entre el ruido.

**La región de verificación importa al pulsar SELECT.** `iremote.py press -u`
repite la pulsación hasta que la pantalla cambia más que un umbral. Entrar en un
submenú sólo redibuja la LISTA DE LA IZQUIERDA: el panel derecho sigue mostrando
el mismo icono y "4 Songs". Verificando con `--region right` el diff sale 0, se
reintenta quince veces, y las pulsaciones de más se meten en Cover Flow. Para
transiciones de lista hay que usar `--region left`.

**Y el recordatorio que más caro sale.** Con el acelerador SHA-1 activo la
biblioteca sale VACÍA ("No Music" / "No Songs") — es el nudo de §11.35, no una
regresión. Toda corrida que deba reproducir necesita `SHA1_ZERO=1`. Tres corridas
seguidas se interpretaron como "la reproducción no arranca" cuando lo que pasaba
es que no había biblioteca que reproducir.

El menú, verificado en pantalla y en inglés: **Music, Videos, Photos, Podcasts,
Extras, Settings, Shuffle Songs** — Music ya viene resaltado al aceptar el idioma,
no hay que desplazarse. Dentro: **Cover Flow, Playlists, Artists, Albums,
Compilations, Songs**, luego Genres, Composers, Audiobooks — Songs a cinco abajo.

## 11.61 CORRECCIÓN de etiqueta: `0x081e9e70` no es un códec

Todo el trabajo anterior lo llama `aacmath` y lo trata como el decodificador, y de
ahí sale la frase "el códec se llama pero con cuenta cero". Leído el desensamblado,
eso no se sostiene:

```
81e9e70: ...
81e9e94: bl 0x81c0c50          ; obtiene un objeto
81e9ed8: bl 0x80f8ba8          ; <-- "siguiente" de una lista
81e9ec4: cmp r0, r6            ; el [+4] de cada elemento contra los dos valores
81e9ecc: cmp r0, r7
81e9ef0: bl 0x80f8ab4          ; actúa sobre el que encaja
```

`0x80f8ba8` es un iterador de lista enlazada sin ninguna ambigüedad: cursor en
`[obj+0x30]`, cabeza en `[obj+0x28]`, avanza por el puntero `next` y devuelve
`cursor+8`. Y `0x80f8ab4` recorre un array de punteros de función
(`[r5+4] + i*4`, llamando a `0x83613a8`). Eso es **despacho de notificaciones a
suscriptores**, no aritmética sobre PCM.

Así que `aacmath=1` nunca significó "el decodificador produjo una trama", y
`min([obj2+0x44],[r4+8]) = 0` no significa "se le pide producir cero muestras":
son los extremos de un rango que se usa para elegir a quién notificar. La
conclusión que colgaba de esa lectura queda retirada.

**La lección, que ya va dos veces en esta investigación:** una etiqueta puesta por
conjetura (`aacmath`, `pullbail`) se propaga a través de docenas de medidas y las
contamina todas. Antes de construir sobre un nombre, leer las diez instrucciones.

## 11.62 CORRECCIÓN de §11.58: los búferes SÍ se rellenan, ~4 000 veces

§11.58 concluyó "nadie rellena de audio los búferes de IRAM0 que el DMA drena".
Medido con un `memw` sobre la cabeza del búfer (`0x22008c5c-0x22008cdc`), también es
falso:

```
--- mem-write watch 0x22008c5c-0x22008cdc: 127264 stores from 8 distinct PC(s) ---
    writer pc=0x22000294  stores=63328
    writer pc=0x22000298  stores=63328
    writer pc=0x080fb4ec  stores=256
    writer pc=0x080fb50c  stores=256
    (y cuatro más, de 16 a 32 escrituras)
```

La ventana vigilada son **128 bytes**, o sea 32 palabras, así que 127 264 escrituras
son unos **4 000 rellenos completos** del búfer de 4 KB — el mismo orden que los
terminal-counts del DMA. Un bucle de copia residente en IRAM (dos instrucciones de
store, `0x22000294` y `0x22000298`, con recuentos idénticos) mantiene el búfer
alimentado al ritmo exacto al que se vacía.

**La desproporción real, en la misma corrida:**

| sitio | veces |
|---|---|
| relleno en IRAM (`0x22000294/8`) | ~4 000 rellenos |
| `copy16` | 4 808 |
| `dpump` (bomba de render) | **8** |
| `mixer` | **8** |
| `rendret` | 4 |
| `pcmout` | 4 |

El consumidor y su rellenador van a tempo. **El render corre ocho veces en toda una
corrida de 150 segundos.** Así que el rellenador copia una y otra vez un origen que
nadie refresca, y por eso entrega silencio: no falta el escritor, falta que el
render produzca.

Con esto, las tres formulaciones anteriores quedan retiradas — "no arranca el lazo"
(§11.57), "nadie rellena los búferes" (§11.58) y "al códec se le piden cero
muestras" (§11.61). La pregunta que queda es estrecha y comprobable: **el SM1 levanta
~2 400 interrupciones y el manejador señala su evento otras tantas; ¿por qué la bomba
de render sólo despierta ocho veces?**

## 11.63 Lo que escribe el búfer de audio es un `memset` a CERO

Las 127 264 escrituras de §11.62 vienen de `0x22000294` y `0x22000298`. En
`assets/classic6g/iram.dis`:

```
2200027c: mov r2, #0                      <-- el valor de relleno es CERO
22000280: subs r1, r1, #32
22000288: mov lr, r2
2200028c: mov r3, r2
22000290: mov ip, r2
22000294: stmiacs r0!, {r2, r3, ip, lr}   <-- 16 bytes de ceros
22000298: stmiacs r0!, {r2, r3, ip, lr}
2200029c: subscs r1, r1, #32
220002a0: bcs 0x22000294
```

Es `memset(dst, 0, n)` residente en IRAM. **Nadie está copiando audio al búfer: el
firmware lo está BORRANDO**, unas 4 000 veces por corrida, una por cada vuelta del
DMA.

Eso no es un fallo, es la ruta de **underrun** del driver de salida haciendo
exactamente lo correcto: no hay datos que enviar, saca silencio. Y confirma desde
otro ángulo que todo el lado de salida está sano. Verificado además en la máquina
viva:

```
22008c5c: 0x00000000 0x00000000 0x00000000 0x00000000    (el bufer, reproduciendo)
22010020: 0x22008c5c 0x3ca00010 0x22010020 0x84249800    (el descriptor del DMA)
```

El descriptor se apunta a sí mismo, así que el DMA relee el mismo búfer para
siempre; `ctrl=0x84249800` decodifica como 2048 transferencias de 16 bits, origen
incrementando, destino fijo en el FIFO, interrupción al terminar. Todo correcto.

**El diagnóstico, ahora sí, con las tres piezas medidas:** el consumidor pide a
tempo, el driver responde con silencio porque no tiene nada, y el productor
(`dpump`, ocho veces) no produce. Y la bomba de render sólo se invoca desde el
despachador de la cola de trabajos, que recibió ocho elementos — que es el número
que viene arrastrándose desde el sitio de envío del firmware. La cadena entera es
coherente; lo que falta es lo que debería repetir el envío.

## 11.64 La puerta que apaga el render: `0x080a3c00` y un bit de estado

Con las tres variables puestas (`SHA1_ZERO=1`, `IRAMSPILL=0x10000`, **`SM1_IRQ=1`**)
la cadena de despertar funciona:

```
credithdlr  2646     el manejador de la linea 18 entra
creditsig   2646     y senala el evento 0x22008c58 otras tantas
evwait         8     pero el render solo lo espera OCHO veces
dpump         16     copy16 9616     WAV: solo clics
```

**`SM1_IRQ=1` gatea la creación misma del temporizador** (`ipod_classic_sm1.c`,
`s->irq_enabled`). Sin ella no hay tick, no hay aserción y `raises=0`. El mensaje
que sí aparece — "guest armed the engine interrupt; VIC line 18 live" — es el
FIRMWARE armándola, no el modelo levantándola: leerlo como confirmación costó cinco
corridas. La señal buena es "engine interrupt armed on VIC0 line 18" al arrancar y
luego "line 18 raised #N".

**Y ahora el lazo, desmontado.** El bucle del render en IRAM:

```
22006324: bl 0x22003850   -> veneer a 0x0808af58 (la espera del evento)
22006328: cmp r0, #0
2200632c: beq 0x22006310  -> duerme 10 y reintenta... solo DOS veces
22006338: str fp, [sp,#24]        <- pone la longitud a CERO
2200633c: bl 0x22003858   -> veneer a 0x080a3c00
22006340: ldr r1, [sp,#24]
22006344: cmp r1, #0
22006348: bne 0x22006370          <- trajo datos: camino vivo
22006358: bne 0x220061fc          <- no trajo: SALIDA
```

La espera tiene éxito. Lo que decide es `0x080a3c00`, que calcula puntero y longitud
del búfer PCM y los escribe en los dos parámetros de salida — **pero sólo si pasa una
guarda**:

```
80a3c18: ldr  r1, [r4,#4]     ; r4 = 0x0896a684 (global), r1 = descriptor
80a3c1c: ldrh r2, [r1]        ; palabra de estado de 16 bits
80a3c20: cmp  r0, #6          ; r0 = (estado==6) ? 2 : 1
80a3c2c: tst  r2, r0
80a3c30: beq  0x80a3cbc       ; <-- bit ausente: sale SIN escribir nada
80a3c34: ldrh r0, [r1,#2]     ; segunda guarda: indice de sub-bloque == 0
80a3c4c: cmpeq r3, #1         ;   y [entrada+10] == 1
80a3c50: bne  0x80a3cbc       ; <-- tambien sale
80a3c6c: str  r1, [r5]        ; puntero del bufer
80a3c70: str  r0, [r6]        ; LONGITUD
```

Si el bit no está, la longitud se queda en el cero que el llamador puso, y el render
abandona. **Esa palabra de estado de 16 bits es un espejo de hardware**, y el bloque
que lo alimenta es justo el que estamos modelando. Si el modelo no pone ese bit, el
fallo es nuestro y tiene arreglo. La medida que lo decide —cuántas veces se toma cada
salida— está corriendo.

## 11.65 LOCALIZADO: la guarda de `0x080a3c00` falla el 100 % de las veces

Medido con la reproducción viva (`SHA1_ZERO=1 IRAMSPILL=0x10000 SM1_IRQ=1`, y
comprobando que los contadores no fueran cero antes de creer nada):

```
fetch      4      se llama cuatro veces
fetchok    0      NUNCA sale por el camino vivo
fetchbail  4      SIEMPRE sale por la guarda
```

Y el descriptor, leído de la máquina en marcha:

```
0896a684: 0x00000000 0x2203ff28 0x2203ff00 0x22038000
0896a694: 0x22038100 0x00000000 0x00000000 0x00000001

2203ff28: 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000 0x0000
```

`[0x0896a684+4] = 0x2203ff28`, y **su palabra de estado de 16 bits está a CERO**. La
guarda es `tst r2, r0` con `r0` valiendo 1 o 2, así que con `r2 = 0` no pasa jamás:
`0x080a3c00` vuelve sin escribir la longitud, el llamador la lee como cero y el
render abandona tras sus dos reintentos. **Todas las veces.**

Esa es la cadena completa del silencio, sin huecos:

```
el DMA drena 4 KB de IRAM0 y pide mas            18 500 veces
el driver responde con un memset a CERO           ~4 000 veces   (underrun correcto)
el SM1 senala "trama lista"                        2 646 veces
el render despierta, pide un bufer PCM...
  ...0x080a3c00 mira [0x2203ff28] = 0, y no da nada    100 % fallo
el render abandona tras 2 reintentos
```

**Queda una pregunta, y es la que decide de quién es el fallo:** ¿esa palabra está a
cero porque nadie la escribe nunca (entonces falta algo que el modelo debería
producir), o porque algo la pisa? El candidato para lo segundo tiene motivo y
oportunidad: `copy16` (0x08016f08) escribe su PCM en `[r4+12] + offset` y se sale por
encima de `0x22040000` — luego **pasa por encima de `0x2203ff28` en el camino**. Y el
tamaño del anillo que gobierna ese offset es una INFERENCIA del modelo
(`S5L8702_SM1_POS_RING_FRAMES = 0x20000/4`, "IRAM1 son 128 KiB"), no una medida —
mientras que lo alto de IRAM1 resulta contener las estructuras de control del audio
(tabla en 0x22038000, búfer en 0x22038100, descriptor en 0x2203ff28). Un anillo de
128 KB desde 0x22020000 las cubre a las tres.

## 11.66 CONFIRMADO: el render pisa su propia estructura de control

Watch de escrituras sobre el descriptor, con reproducción viva:

```
--- mem-write watch 0x2203ff28-0x2203ff40: 24 stores from 1 distinct PC(s) ---
    writer pc=0x08016f08  stores=24
```

**Un solo escritor, y es `copy16`** — el bucle que copia el PCM decodificado al
anillo del render. Escribe muestras de audio encima de `0x2203ff28`, palabra de
estado incluida. Es decir: el render destruye la estructura que la vuelta siguiente
necesita para saber dónde escribir, y por eso `0x080a3c00` falla el 100 % de las
veces (§11.65).

**La aritmética cuadra al byte.** `copy16` escribe en `[r4+12] + offset` con
`[r4+12] = 0x22020000` y `offset = posición * 4`, y el modelo envuelve la posición a
`S5L8702_SM1_POS_RING_FRAMES = 0x20000/4` = 32768 tramas:

```
destino maximo = 0x22020000 + 0x20000 + (trozo de ~2,4 KB) = ~0x22040960
abort observado en s11.36:                                    0x22040a16
```

Ese envoltorio **es una inferencia del modelo**, escrita en el propio comentario del
header: *"IRAM1 is 128 KiB, so the position wraps every 32768 frames"*. Nunca se
midió. Y lo que ahora sabemos es que lo alto de IRAM1 **no es anillo**: contiene las
estructuras de control del audio —tabla en `0x22038000`, búfer en `0x22038100`,
descriptor en `0x2203ff28`—. Un anillo de 128 KB desde `0x22020000` las cubre a las
tres.

Con eso, el fallo de `copy16` (§11.36) deja de ser un misterio aparte: **es el mismo
fallo**. No es que la copia se desmadre, es que le estamos diciendo que el anillo es
el doble de grande de lo que es. Y `IRAMSPILL` nunca podía arreglarlo: evita el
abort del final, no el pisotón del descriptor que ocurre 2,3 KB antes.

Si el anillo va de `0x22020000` a `0x22038000`, son `0x18000` bytes = **24576
tramas**. Hay override por entorno (`SM1_POS=N`), así que se comprueba sin
recompilar.

## 11.67 De dónde sale el destino de `copy16`, sin inferencias

§11.66 atribuyó el pisotón al tamaño del anillo, apoyándose en que el destino sería
`0x22020000 + posición*4`. Eso venía del comentario del header, no de leer el código.
Leído:

```
8016e58: mov r2, sp          ; parametro de salida
8016e60: bl  0x08091710      ; el getter de posicion del SM1
...
8016e94: ldr r2, [sp]        ; lee lo que el getter escribio
8016e9c: ldr r3, [r4,#12]    ; base
8016ea4: add r2, r3, r2      ; destino = base + valor, suma de BYTES, sin escalar
```

Y el getter no devuelve la posición cruda:

```
8091734: add r1, r3, r1, lsl #2   ; base del bloque + canal*0x14
8091738: ldr r3, [r1,#0xa48]      ; NUESTRO registro de posicion
809173c: ldr r0, [r0,#4]          ; una base que mantiene el FIRMWARE
8091740: add r0, r0, r3           ; base + posicion
8091744: str r0, [r2]             ; <- esto es el offset que usa copy16
```

Luego **destino = `[r4+12]` + `[tabla+4]` + posición**, con un acumulador del propio
firmware en medio. Dos consecuencias:

1. La aritmética de §11.66 (destino máximo ~0x22040960 contra el abort en 0x22040a16)
   **no prueba la atribución**: con esa indirección el ajuste puede ser casualidad.
   La parte medida de §11.66 —que `copy16` es el ÚNICO escritor del descriptor— sigue
   en pie; la explicación de por qué llega hasta allí, no.
2. Bajar el envoltorio a 24576 tramas redujo los pisotones de 24 a 12 pero **no los
   eliminó**, y `fetchok` siguió en 0. Coherente con lo anterior: nuestro registro
   contribuye, pero no manda solo.

La medida que hace falta es la que está corriendo: capturar `r2` (destino) y `r3`
(base) en la cabeza del bucle `0x08016efc`, que es destino de salto y por tanto da
lecturas de registro fiables. Con eso se sabe el rango real que barre la copia y si
el envoltorio lo gobierna, en vez de deducirlo.

## 11.68 El pisotón era consecuencia, no causa: NADIE escribe la palabra de estado

Con `SM1_POS=16384` (anillo de 64 KB desde 0x22020000):

```
destinos de copy16 observados   0x2203cef4 .. 0x2203d65a    (medidos, no deducidos)
pisotones al descriptor         0                            <-- eliminados
fetchok                         0                            <-- IGUAL que antes
WAV                             solo clics, pico 11444
```

Quitar el pisotón **no arregla nada**. Y como `copy16` era el ÚNICO escritor de
`0x2203ff28` (§11.66), ahora no queda ninguno: en toda la corrida, arranque incluido,
**nadie escribe esa palabra de estado**. Estaba a cero desde el principio; `copy16`
la pisaba *además*, pero no era la causa del cero.

Queda retirada la explicación de §11.66. Lo que sobrevive de aquella sección es el
hecho medido —`copy16` escribía allí— y una mejora real: con el anillo corregido el
render deja de corromper la zona de control y el data abort de §11.36 desaparece.
Eso arregla un fallo, pero no el silencio.

**Lo que esto deja establecido, que es lo importante:** el bloque de control del
audio en lo alto de IRAM1 **nunca se inicializa**. El puntero que lo registra sí está
puesto (`[0x0896a684+4] = 0x2203ff28`), luego alguien montó la estructura y algo
debería llenarla. Las dos posibilidades:

- **(a)** código del firmware que no llega a ejecutarse, gateado por algo que el
  modelo contesta mal — sería un fallo nuestro, indirecto;
- **(b)** el bloque lo escribe el propio motor SM1 por DMA, y nuestro modelo no
  escribe nada en memoria — sería un hueco nuestro, directo.

(b) encaja con la forma de la estructura: una palabra de bits "hay hueco listo", un
índice de sub-bloque y una tabla de ranuras en `0x22038000` con longitud y paso. Es
exactamente lo que un motor de audio reporta a la CPU. Distinguirlas es la medida que
está corriendo: vigilar quién escribe el global que registra el bloque nombra al
código de montaje.

## 11.69 Quién monta el bloque, y un punto ciego del modelo

**El global SÍ se mantiene.** Watch sobre `0x0896a684-0x0896a6a4`: lo escriben once
sitios del motor de audio (`0x08081754`, `0x08081764`, `0x08081984`, `0x08081aa0`,
`0x08081b6c`, `0x080820e0`, `0x0808214c`, `0x08082174`, `0x08082184`, `0x08082190`,
`0x08085ef8`), en `+0`, `+1`, `+2`, `+0x14` y `+0x18`. Así que la estructura de
control está viva y el firmware la mantiene; lo único que nadie escribe es el bloque
al que apunta.

**Y ese bloque cae donde nosotros escribimos.** `S5L8702_BOOTINFO_BASE = 0x2203ff00`,
y el arranque de la máquina hace `cpu_physical_memory_write(0x2203ff00, zero, 0x100)`.
El descriptor del audio está en `0x2203ff28` — dentro de esos 256 bytes. El comentario
del header dice *"away from anything the firmware uses"*, marcado ASSUMED; no lo está.
Es una escritura única de init, así que no pisa nada más tarde, pero explica el cero
que se lee: lo pusimos a cero y nadie lo tocó después.

**El firmware NO le da direcciones de IRAM1 al SM1.** Buscadas en `SM1TRACE`: cero
coincidencias con `2203ff` y con `22038`. Pero sí le da trabajo, y mucho:

```
+0xc48 (COMMIT)   519 escrituras
+0x400 (RUN)      518
+0x110, +0xb10, +0xa90, +0xa8c, +0x99c...   la puesta en marcha
```

519 «commits» al motor, y el modelo los acepta sin hacer nada.

**El punto ciego.** La ventana del bloque mide 1 MB y sólo se modelan sus primeros
4 KB. Todo lo de arriba se descartaba en silencio **antes** de la traza, así que ni
siquiera `SM1TRACE=1` podía enseñarlo: si el firmware sube descriptores o código de
DSP ahí, lleva toda la investigación cayendo en un agujero sin que ninguna medida lo
notase. Añadido un contador (misma escala logarítmica, apagado sin la variable) que
reporta cuántas escrituras van arriba y en qué rango. Corriendo.

## 11.70 El punto ciego, medido: una segunda página de registros, no una carga

Con el contador nuevo (`SM1HI=1`, variable propia porque `SM1TRACE` genera un log
capaz de tumbar el sistema de ficheros entero — pasó):

```
+0x1000 = 0 / 1 / 3      desde codigo IRAM (0x2200239c)
+0x1008 = 1              mas de 1024 veces, desde 0x080915f0
rango visto: +0x01000 .. +0x0100c     (11 lineas en toda la corrida)
```

No hay carga masiva de código ni de descriptores: lo que se descartaba es **una
segunda página de control pequeña**, `+0x1000..+0x100c`. El despachador que la
escribe (0x080915a4) traduce una máscara de bits a parejas activar/desactivar:

```
bit 8 -> +0x1004 = 3, +0x1008 = 1, +0x100c = 1
bit 0 -> +0x1008 = 1
bit 1 -> +0x1008 = r2
bit 2 -> +0x100c = 1
bit 3 -> +0x100c = r2
bit 9 -> +0x1004 = 0, +0x1008 = 0
```

Todos son **stores**; en toda la zona del driver (`0x8091xxx`) no hay ni una lectura
de esa página, así que devolver cero no engaña a nadie. Descartada también la
lectura vecina de `+0x404` (0x08091590 la enmascara con el bit 8 y nuestro cero es
justo la respuesta de éxito).

Es un hueco real del modelo y conviene taparlo, pero **no es el que calla la música**.

## 11.71 CORRECCIÓN: el cambio de anillo no arregla el data abort (no se reproduce)

§11.68 dijo que con el anillo corregido "el data abort de §11.36 desaparece". Puesto
a prueba de la única forma honesta —**sin `IRAMSPILL`**, que es la muleta que lo
tapaba— y en A/B:

```
A: anillo por defecto (32768 tramas), sin IRAMSPILL -> reinicios=0  excepciones=0
B: anillo corregido   (16384 tramas), sin IRAMSPILL -> reinicios=0  excepciones=0
```

**Ninguna de las dos aborta.** Con `SM1_IRQ=1` el abort no se reproduce siquiera, así
que el cambio de anillo no puede acreditarse con arreglarlo: no había nada que
arreglar en esta configuración. (El reinicio de §11.36 se observó con `SM1_IRQ`
apagado; con la cadena de interrupción viva el render toma otro camino.)

Lo que **sí** queda medido del cambio de anillo, y sigue siendo verdad:

| anillo | escrituras de `copy16` sobre el descriptor |
|---|---|
| 32768 tramas (por defecto) | 24 |
| 24576 tramas | 12 |
| 16384 tramas | **0** |

Deja de corromper la zona de control, que es correcto por sí mismo. Pero no cambia
`fetchok` (sigue 0) ni el WAV (sigue en clics). **No es el arreglo del silencio, y no
debe presentarse como tal.**

Y hay una relectura que conviene considerar antes de seguir tirando de este hilo: que
`0x080a3c00` no dé búfer puede ser **comportamiento correcto** —no hay nada
decodificado que entregar— en vez de una guarda rota. En esa lectura el fallo no está
en quién escribe la palabra de estado sino en que el decodificador nunca produce, y
`fetchbail` es síntoma, no causa. `framepull` y `pullpass` valen 1 en estas corridas,
lo cual encaja.

## 11.72 El fichero SÍ se lee del disco

`ATATRACE` sólo imprimía offsets de registro, así que en toda la investigación nunca
se pudo ver **qué sectores** se leen. Añadido `ATALBA` (=1 escala logarítmica, =2
todas las lecturas con marca de tiempo virtual), que usa el `ide_get_sector()` que ya
estaba a mano en `prepare_buf`.

La primera canción de la lista (Billie Jean) es
`iPod_Control/Music/F47/libgpod282598.mp3`, y `filefrag` la sitúa en un extent único,
bloques 20707..21862 de 4 KB. Con el offset de partición (92962816) eso son los bytes
absolutos **177 778 688 .. 182 513 664**. Y entre las lecturas medidas:

```
ATALBA rd #512  lba=348368  (byte 178 364 416)   <-- DENTRO de ese rango
total leido en la corrida: 36 756 KB
```

**El firmware lee el fichero.** Eso descarta la lectura más simple de §11.71 —"el
decodificador nunca produce porque nunca le dan nada"— al menos en su forma fuerte:
los bytes del MP3 llegan del disco.

Queda por separar *cuándo*: si el bloque se lee de golpe al abrir la pista o en goteo
mientras suena. Es lo que mide `ATALBA=2`, cuyo resultado se cruza con el rango de la
canción y con el reloj virtual.

## 11.73 El fichero se lee ENTERO, y en dos ráfagas

Con `ATALBA=2` (todas las lecturas, con reloj virtual), cruzado con el rango de la
canción:

```
368 lecturas dentro de la cancion tras pulsar play
primera: +136 844 374us  byte 177 778 688   <- el primer byte del fichero
ultima:  +153 057 975us  byte 182 513 664   <- el ultimo
reparto: dos rafagas, ~136,9-137,3 s y ~152,9-153,1 s
total leido en la corrida: 51 804 KB
```

El fichero se lee **de principio a fin**, no en goteo. Eso es lo que hace un iPod de
verdad: llenar un búfer grande para poder parar el disco. Así que el decodificador
tiene la canción **completa en memoria** y aun así no sale una muestra.

Con esto el fallo queda encerrado en el paso de decodificación, y la sospecha tiene
por fin un nombre concreto en el árbol: en la tabla de bloques sin implementar de
`ipod_classic.c` están **`s5l8702.adm` y `s5l8702.adm-sfr`**. ADM es el
**CalmADM2E**, el DSP de audio del SoC — un stub que devuelve ceros y descarta lo que
se le escribe.

Si el firmware le entrega ahí el trabajo de decodificar, todo lo medido encaja sin
forzar nada: entrada correcta, salida correcta, estructuras de control en IRAM1
vacías porque quien las llena no existe, y silencio perfectamente ordenado. La medida
que lo confirma o lo tumba —cuánto tráfico recibe ese bloque, y si se le vuelca
código— está corriendo con `-d unimp`.

## 11.74 REFUTADO el DSP: el firmware no toca el ADM ni una vez

Corrida con `-d unimp`, que registra cada acceso a un bloque sin implementar:

```
reparto por bloque, toda la corrida (274 accesos en total):
    226  s5l8702.gpio
     42  s5l8702.eic
      2  s5l8702.miu
      2  s5l8702.lcd-win
      2  s5l8702.lcd
  lineas del adm: 0
```

**Cero.** Ni `s5l8702.adm` ni `s5l8702.adm-sfr` reciben un solo acceso, ni en el
arranque ni durante la reproducción. La hipótesis de §11.73 queda refutada, y con
ella la idea —que venía arrastrándose desde wiki/14— de que retailOS decodifica en el
CalmADM2E. **En estas corridas el DSP no participa en nada.**

Con eso, la decodificación tiene que ser en CPU, y lo medido dice que no ocurre:
`framepull` una vez, `pullpass` una vez. Y el resto del sistema está sano: entrada
(el fichero entero en memoria, §11.73), salida (18 500 búferes entregados, §11.58) y
la cadena de despertar (2 646 señales, §11.64).

Van catorce hipótesis refutadas con medida en esta investigación. Las dos últimas
—formato del fichero (§11.59) y DSP (aquí)— eran de las más plausibles, y ninguna se
sostuvo. Lo que queda por responder es estrecho: **quién debería llamar al pull de
tramas más de una vez, y qué se lo impide.**

## 11.75 El pull de tramas se llama UNA vez, a los 16 ms del arranque

Captura de registros en la entrada del pull (`0x081e93d8`, entrada de función, así
que la lectura es fiable):

```
pullwho hit#1  +16877us  lr=0x081ea388  r0=0x093effbc r1=0 r5=1 r6=r7=0xffffffff
llamadores distintos: 1
credread 8   pullpass 1   pullret 1   fetchok 0   StartPlayback 2   dpump 8
```

**+16 877 µs son 16 milisegundos: eso es el arranque, no la reproducción**, que
empieza pasados los 130 segundos. Así que la ruta de decodificación **no se entra ni
una vez mientras suena la canción**. Todo lo que se midió aguas abajo de ahí —y las
conclusiones que colgaban de "el códec corre una vez"— describían un evento de
arranque.

`credread=8` no lo contradice: es un ayudante compartido, llamado también desde otros
sitios, y ocho coincide con `dpump`.

**Quién lo llama.** `lr = 0x081ea388` es el retorno de un `blx r2` en `0x081ea384`,
dentro de un notificador que empieza en `0x081ea2c0` y tiene tres salidas:

```
81ea2cc: cmn r6,#1 / cmnne r7,#1     el rango pedido es (-1,-1)?
81ea2e0: ldrne r1,[r4,#0x20]         si no: capacidad
81ea2ec: cmpne r0,r1                 tramo pedido == capacidad?
81ea2fc: popne {..,pc}               <-- SALIDA A: vuelve sin notificar
81ea344: ldrb r0,[r4,#0x1c]          estado guardado
81ea348: cmp r0,r5                   != al pedido?
81ea34c: bne 0x81ea370               --> NOTIFICA
81ea350: cmp r9,#0                   si son iguales, histeresis por signo
81ea384: blx r2                      <-- SALIDA B: notifica (y de aqui el pull)
81ea388: ...                         <-- SALIDA C: no hay flanco, no notifica
```

La única ejecución medida traía `r6=r7=-1` (rango "todo") y estado 1, y disparó. Lo
que hay que saber ahora es cuántas veces se invoca este notificador **durante la
reproducción** y por cuál de las tres salidas se va: si no se invoca, el fallo está
más arriba; si se invoca y se cae por la salida A, el tramo pedido no cuadra con la
capacidad y eso es aritmética de búferes que se puede seguir. Corriendo.

## 11.76 El notificador tampoco se llama mientras suena

Captura en la entrada del notificador (`0x081ea2c0`):

```
6 llamadas en TODA la corrida, todas con lr=0x081c17f0 (un unico llamador)
   hits 1-3:  +15,4 / +16,3 / +17,9 ms     arranque
   hits 4-6:  +16,17 s                     todavia muy antes de reproducir
notifyfire 1   notifyskip 6   pull 0   dpump 8
r0=0x09555db0  r3=1 (estado pedido)  r4=0x0ba90c98  (el objeto de la cola)
```

(Los 6 "skip" con 1 "fire" no se contradicen: `0x081ea388` es también la caída
natural después del `blx`, así que toda llamada pasa por ahí.)

**Durante la reproducción, entre los 130 y los 190 segundos, el notificador no se
invoca ni una sola vez.** Así que el fallo no está en sus tres salidas: está en quien
debería llamarlo.

Ese llamador es único, `0x081c17f0`, dentro de una función que empieza en
`0x081c1794` y que **arranca con una puerta**:

```
81c1794: push {r1, r2, r3, r4, r5, lr}
81c1798: mov  r4, r0
81c179c: ldrb r0, [r0, #0x70]      <-- un byte de estado del objeto
81c17ac: cmp  r0, #0
81c17b4: bne  0x81c1808            <-- si NO es cero, se salta la notificacion entera
81c17c0: ldr  r0, [r4, #0x48]      (camino vivo: despacho virtual por [+0x18])
```

Si ese byte se queda distinto de cero, la petición al decodificador no sale nunca, por
muchas veces que se llame. La medida en curso cuenta las llamadas durante la
reproducción y por cuál de las dos salidas se van; `r0` en la entrada da además la
dirección exacta del byte, con lo que el siguiente paso —quién lo escribe— es un
watch de una línea.

## 11.77 Tres niveles seguidos corren SÓLO en el arranque: se estaba trazando el conducto equivocado

```
0x081c1794 (peticion)    3 llamadas, a los 15,0 / 16,0 / 17,6 ms
0x081ea2c0 (notificador) 6 llamadas, todas por debajo de 17 s
0x081e93d8 (pull)        1 llamada,  a los 16,9 ms
durante la reproduccion (130-190 s): CERO en los tres
```

La puerta de `[objeto+0x70]` de §11.76 tampoco es la culpable: `reqgated` y `reqgo`
salen ambos a 3, que es el paso natural por la etiqueta, no un rechazo.

Tres niveles consecutivos que sólo se ejecutan en el arranque no son tres fallos: son
la señal de que **esta cadena no es la ruta de reproducción**. Las direcciones vienen
de sesiones anteriores que las midieron en condiciones de arranque —y buena parte de
aquellas medidas se tomaron además sin `SM1_IRQ=1`, es decir con la cadena de
despertar muerta (§11.64)—, así que describen el cebado inicial, no lo que ocurre al
pulsar reproducir.

Conviene decirlo sin adornos: una parte del mapa heredado apunta al sitio equivocado,
y seguir subiendo por llamadores dentro de él sólo produce más niveles dormidos.

**Cambio de método.** En vez de suponer qué código corre, medirlo: muestrear el PC
por el monitor mientras suena la canción y hacer un histograma. Eso dice, sin
hipótesis previas, qué está ejecutando el aparato durante los 60 segundos en los que
debería estar decodificando, y en qué región vive ese código (IRAM0, IRAM1 o DRAM).

## 11.78 EL DATO QUE FALTABA: la CPU está DORMIDA el 88 % del tiempo

Muestreo del PC por el monitor, 300 muestras mientras la canción "suena":

```
0x22003b20   264  (88,0%)  IRAM0
0x00004da8    10   (3,3%)  vector/bajo
0x22003e8c     4   (1,3%)
... el resto, colas de 1-5 muestras
reparto: IRAM0 275, direcciones bajas 25, DRAM 0
```

Y `0x22003b20` es, en `iram.dis`:

```
22003b0c: nop / nop / nop / nop
22003b1c: mcr 15, 0, r0, cr7, cr0, {4}    <-- WAIT FOR INTERRUPT
22003b20: nop                             <-- donde cae el 88% de las muestras
22003b24: nop / nop / nop
22003b30: b 0x22003b0c
```

Es el **lazo ocioso del kernel**, y el PC está en la instrucción siguiente al WFI.

**El aparato no está atascado: está dormido.** Eso reencuadra la investigación entera.
No hay un bucle que se coma la CPU, ni un interbloqueo, ni una espera que falle: no
hay trabajo. Cero muestras en DRAM significa que durante esos 60 segundos **no se
ejecuta código de aplicación en absoluto**, y por tanto tampoco decodificador —
cualquier decodificador, en cualquier sitio del mapa.

Encaja con todo lo medido y explica de golpe por qué cada nivel que se trazaba daba
"ocho veces" o "una vez": no es que ocho eventos se pierdan, es que el sistema
despierta ocho veces y se vuelve a dormir.

Y encaja también con §11.77: no había un conducto equivocado que trazar, porque
**durante la reproducción no se ejecuta prácticamente nada**.

**La pregunta correcta, por fin.** No "qué eslabón está roto" ni "quién escribe tal
palabra", sino: *el SM1 levanta 2 646 interrupciones y su manejador señala el evento
otras tantas; ¿por qué eso no despierta a ninguna tarea?* La señal llega y el sistema
sigue durmiendo. Eso es una cuestión de planificación del kernel RTXC, no de audio.

## 11.79 Los punteros de la estructura son datos estáticos del firmware

Watch sobre los campos de puntero del global (`0x0896a688-0x0896a698`):

```
+0us   pc=0x22000198  addr=0x0896a688
+28us  pc=0x22000198  addr=0x0896a68c
+29us  pc=0x220001a0  addr=0x0896a690
+30us  pc=0x220001a0  addr=0x0896a694
(y ninguna mas en toda la corrida)
```

Se escriben en el microsegundo cero, desde código de IRAM0 (`0x22000198`, la rutina
de copia de la que también sale el `memset` de §11.63). Es decir, **son datos
estáticos de la imagen**, copiados durante el arranque: la dirección `0x2203ff28` no
la calcula nadie en ejecución, viene fijada en el firmware. Ahí es donde sus autores
pusieron la estructura de control del audio, en lo alto de IRAM1.

Cronología del fallo en la misma corrida:

```
fetchbail #1 .. #4   +142,2 s .. +142,4 s      (al pulsar reproducir)
fetchbail #5 .. #6   +163,6 s
fetchbail #7 .. #8   +169,7 s
fetchok              0
```

Ocho intentos, ocho fallos, ninguno acierta.

## 11.80 SONDA (no arreglo): poner el bit a mano

La guarda exige un bit —1 o 2— en la palabra de 16 bits de `0x2203ff28`, que está a
cero y que nadie escribe nunca. Escribirlo a mano por gdb con la máquina en marcha
responde una pregunta que ninguna lectura del desensamblado puede responder: **¿es
esa guarda el único bloqueo, o hay más detrás?**

Si tras ponerlo `fetchok` deja de ser cero y el WAV cambia, el mecanismo queda
confirmado y se sabe qué tiene que producir el modelo. Si no cambia nada, la guarda
es síntoma y hay que mirar más arriba —lo cual, con la CPU dormida el 88 % del tiempo
(§11.78), es lo más probable—.

Queda dicho de forma explícita: **esto es instrumentación de diagnóstico**, escribir
memoria del invitado desde fuera. No es un arreglo, no puede quedarse, y no cuenta
como "el reproductor funciona".

## 11.81 RESULTADO DE LA SONDA: la guarda no era el bloqueo

```
antes:     0x2203ff28 = 0x0000    fetchbail=4  fetchok=0
escrito:   0x2203ff28 = 0x0003    (el bit que la guarda exige, puesto por gdb)
45 s despues: fetchbail=4  fetchok=0  dpump=0
WAV: sigue en clics, pico 11444
```

`fetchbail` **no sube** y `dpump` es **cero**: tras poner el bit, la búsqueda de
búfer no se vuelve a llamar ni una sola vez, porque el render no vuelve a
ejecutarse. La guarda de `0x080a3c00` no era el bloqueo — es que **nadie replanifica
al render para reevaluarla**. Y gdb, al enganchar, encontró la CPU en `0x22003b20`:
el WFI de nuevo.

Queda retirada toda la línea de §11.64-§11.65: localizar la guarda estaba bien como
observación, pero la conclusión "ahí se apaga el reproductor" era falsa. Se apaga
antes, y por eso la guarda sólo se evalúa ocho veces.

Con esto y con §11.78, el diagnóstico converge en una sola frase: **el sistema
procesa unos pocos búferes al pulsar reproducir y luego se duerme, y nada lo vuelve
a despertar para trabajar.** Las 2 646 interrupciones del SM1 entran, su manejador
señala un evento, y la CPU vuelve al WFI: no hay tarea esperando ese evento porque la
que debía esperarlo ya terminó.

**Lo que hay que mirar, y no se ha mirado.** Todos los muestreos y casi todas las
medidas se han tomado *bien entrada* la reproducción, cuando ya está todo dormido.
El fallo ocurre **en el instante de pulsar play** —el primer `fetchbail` es lo
primero que pasa—, así que lo que importa es qué código corre en esa ventana y qué
deja sin hacer. Es lo que mide el muestreo en curso.

## 11.82 Ni siquiera al pulsar play se pone a trabajar

Muestreo del PC en la ventana del arranque de la reproducción (400 muestras,
pulsando reproducir a los 2 s del inicio del muestreo):

```
ocioso (0x22003b20): 358 de 400  = 89,5%
PCs no ociosos: colas de 1-13 muestras en vectores y kernel de IRAM0
DRAM: CERO muestras
```

§11.78 midió el 88 % ocioso bien entrada la reproducción, y quedaba la duda de si el
arranque de la canción sí traía una ráfaga de trabajo. No la trae: **el aparato está
igual de dormido en el instante de pulsar play**. Y sigue sin aparecer una sola
muestra en DRAM, que es donde vive el código de aplicación.

(Nota de método: el muestreo por el monitor es intrusivo —cada `info registers`
detiene la máquina un instante— así que estos porcentajes valen para comparar entre
sí, no como medida absoluta de carga. La conclusión que soportan es cualitativa y
robusta: no hay ráfaga de trabajo donde debería haberla.)

Queda una comprobación elemental que no se ha hecho en toda la investigación, y que
puede reordenarlo todo: **qué muestra la pantalla después de pulsar reproducir.** En
las capturas de esta sesión se queda en la lista de canciones y no pasa a
"Reproduciendo". Si el reproductor no llega siquiera a abrirse, entonces los ocho
búferes medidos no son "el principio de una reproducción que se apaga" sino otra cosa
—los sonidos de la interfaz, por ejemplo— y toda la cadena que se ha estado trazando
describe algo que nunca fue la reproducción.

## 11.83 EL SÍNTOMA REAL: el reproductor se abre y se cierra solo

Capturas tras pulsar reproducir, en la misma corrida:

```
+5 s   la animacion de portada, girando -> EL REPRODUCTOR SE ESTA ABRIENDO
+20 s  de vuelta en la lista de canciones
+45 s  de vuelta en la lista de canciones
+70 s  de vuelta en la lista de canciones
```

Y el diff de la pulsación es de 42 416 píxeles: la pantalla cambia mucho y **vuelve**.
No es que "Reproduciendo" se quede colgada en la animación, como se anotó en §11.39:
**el reproductor arranca y aborta**, devolviendo el control a la lista.

Ese es el síntoma que había que tener desde el principio, y reordena la lectura de
todo lo demás: los ocho búferes, los ocho `dpump`, los cuatro `fetchbail` no son "una
reproducción que se apaga poco a poco" sino **el intento de apertura y su
cancelación**. Después de eso no hay nada que replanificar, y por eso la CPU se queda
en el WFI (§11.78).

Descartado además, en la misma línea, que el registro de la pista esté mal: volcado
el `mhit`, el campo de tipo en `+0x18` contiene la fourcc `'MP3 '`, y tamaño,
duración, bitrate (128) y frecuencia (44100 en 16.16 = 0xac440000) cuadran con los
ficheros que hay en disco.

**Para la próxima sesión, la pregunta correcta es otra:** no "por qué no sale audio"
sino **"por qué el reproductor aborta al abrirse"**. Se ataca desde la UI hacia
dentro —qué error devuelve la apertura de la pista— y no desde el DMA hacia fuera,
que es por donde se ha entrado todas las veces y donde todo está sano.

## 11.84 Descartada la geometría rancia del registro, y una comprobación de método

`patch_tracks.py` sólo actualiza `size`, `tracklen`, `bitrate` y `samplerate`, así que
cualquier otro campo del `mhit` sigue describiendo el fichero que libgpod importó la
primera vez. Candidato natural para un reproductor que abre y aborta. Volcados todos
los campos no nulos de la cabecera (584 bytes):

```
+0x070/+0x0a8  0x1ef4fab4 17e97dfa   el dbid de 64 bits, repetido (normal)
+0x12c         0x007df303            <-- el sospechoso
+0x134/+0x138  0x80808080 / 0x8080   marcadores de "desconocido"
```

**Refutado sin gastar una corrida:** las cuatro pistas llevan **el mismo**
`0x007df303`, y cuatro canciones de duraciones distintas no pueden compartir una
cuenta de muestras. Es una constante que escribe libgpod, no geometría del audio. Y
`0x80808080` son marcadores de "sin dato": ponerlos a cero habría sido inventar un
valor, no corregir uno. Se restauró la base desde la copia previa.

(Antes ya se había descartado que el registro estuviera mal de tipo: `+0x18` lleva la
fourcc `'MP3 '` y el resto cuadra, §11.83.)

## 11.85 La comprobación que faltaba: ¿ATERRIZAN los bytes?

Saber que se lee el sector correcto (§11.72) **no dice que el dato llegue a la
memoria del invitado**. Si el destino del DMA del ATA está mal, el parser ve basura y
el reproductor aborta — que es exactamente el síntoma de §11.83, y sería un fallo
nuestro.

Un volcado por el monitor no sirve para comprobarlo: hecho 25 s después, la memoria
en `0x0bee0720` contenía `mhdp`, un trozo de la base de carátulas. Los búferes se
reutilizan en segundos, así que la lectura tardía no prueba nada — ni a favor ni en
contra.

La forma correcta es hacerlo dentro del modelo: guardar el destino de cada
transferencia y, en la siguiente (cuando la anterior ya ha terminado), leer sus
primeras palabras y publicarlas junto al sector del que venían. La canción empieza en
el LBA 347224 y sus ficheros empiezan por `ff fb 90 64`, así que la comprobación es
exacta: o aparece esa cabecera, o el dato no llega donde debe.

## 11.86 VERIFICADO: los bytes del MP3 aterrizan bien

Primer intento, engañoso: todos los aterrizajes salían `00 00 00 00`. La causa era un
fallo de la propia sonda — `s->dma_cur` ya se ha adelantado `len` bytes cuando se lee,
así que estaba mirando el FINAL del bloque. Corregido a `dma_cur - len`:

```
ATALND lba=347224 dst=0x0bee0720 -> ff fb 90 64 00 0f f0 00   <-- cabecera de trama MP3
ATALND lba=347344 dst=0x0be53240 -> ff fb 90 64 97 01 05 6b
ATALND lba=181568 dst=0x0bee0720 -> eb 58 90 6d 6b 66 73 2e   <-- control: arranque FAT32
224 aterrizajes dentro de la cancion
```

El control es tan importante como la medida: `eb 58 90` + "mkfs." es un sector de
arranque FAT32 legítimo en el primer sector de la partición, lo que confirma que la
sonda mira donde debe. **El camino de entrada queda verificado de punta a punta:
sector correcto, destino correcto, bytes correctos.**

Merece anotarse el patrón, porque ha mordido tres veces hoy: una ventana de `memw`
demasiado pequeña (§11.36), un volcado sobre un búfer ya reutilizado (§11.85) y este
desplazamiento. Los tres devuelven CEROS, y un cero se lee igual que un hallazgo.
**Toda sonda nueva necesita un control conocido antes de creerse su resultado.**

## 11.87 Estado al cierre del tramo

Todos los subsistemas verificados, ninguno falla:

| pieza | evidencia |
|---|---|
| lectura de disco | el fichero entero, del primer byte al ultimo (§11.73) |
| **datos en memoria** | **`ff fb 90 64` en el destino correcto (§11.86)** |
| salida DMA -> I2S | 18 500 buferes entregados (§11.58) |
| driver de salida | `memset` a cero por underrun, correcto (§11.63) |
| cadena de interrupcion SM1 | 2 646 senales (§11.64) |
| metadatos de la pista | fourcc `'MP3 '`, tamano y duracion cuadran (§11.83) |
| CPU | **~89 % dormida en el WFI, cero muestras en DRAM (§11.78, §11.82)** |

Y el síntoma: **el reproductor se abre —animación de portada— y vuelve a la lista**
(§11.83).

Con la entrada verificada y la salida verificada, no queda ningun subsistema al que
culpar: **la decision de abortar es del propio reproductor**. La proxima sesion tiene
que entrar por ahi, desde la UI hacia dentro, y no por el DMA hacia fuera como se ha
hecho todas las veces.

## 11.88 CORRECCIÓN a §11.78/§11.82: "cero código en DRAM" no se sigue del muestreo

El muestreo por el monitor toma unas **16 muestras por segundo**. Si el trabajo que
hace el aparato al abrir una pista dura decenas de milisegundos de tiempo de
invitado, la esperanza matemática de capturarlo es **menos de una muestra**. Que no
aparezca DRAM en 400 muestras no prueba que no se ejecute código de aplicación: sólo
prueba que la CPU está ociosa **la mayor parte del tiempo**, que es lo normal en un
aparato guiado por eventos.

Lo que §11.78 y §11.82 sí sostienen: la CPU pasa el ~88-89 % del tiempo en el WFI del
lazo ocioso, y no hay una ráfaga de trabajo sostenida donde debería haberla (un MP3 a
128 kbps ocupa una fracción apreciable y continua de un ARM de esta clase). Lo que no
sostienen, y queda retirado, es la frase "no se ejecuta código de aplicación en
absoluto".

Para responder eso hace falta **trazar**, no muestrear.

## 11.89 `StartPlayback` es otra etiqueta equivocada

```
pbin (0x08150d84):  2 entradas, a +22us y +16,19 s      <-- nunca al pulsar reproducir
pbfail (0x08151028): 0     pbgo (0x08150e00): 2
```

La función tiene una salida de fallo explícita —suelta el objeto con la misma rutina
del prólogo y devuelve 0— y **no la toma**. Pero es que tampoco corre cuando se
reproduce: sus dos entradas son a los 22 microsegundos y a los 16 segundos.

Con esto van cuatro nombres del mapa heredado que no son lo que dicen: `aacmath` (que
es un despacho de notificaciones, §11.61), `pullbail` (que es el epílogo), la cadena
`pull`/notificador/petición (que es código de arranque, §11.77) y ahora
`StartPlayback`. **El mapa de direcciones heredado hay que tratarlo como sospechoso
hasta que cada nombre se vuelva a comprobar con una medida.**

## 11.90 EL HALLAZGO QUE INVALIDA EL PLANTEAMIENTO: todo pasa en el segundo 0

`cap=8` en el arnés registra sólo los **ocho primeros aciertos de cada sitio**, así
que todos los tiempos leídos en esta investigación son los del principio, no los del
grueso. Subido el tope a 20 000 y hecho el histograma por segundo:

```
copy16     4808  ->  TODO en el segundo 0
dpump         8  ->  todo en el segundo 0
mixer         8  ->  todo en el segundo 0
pcmout        4  ->  todo en el segundo 0
fetchbail     4  ->  todo en el segundo 0
creditsig   325  ->  290 en el segundo 0, 32 en el 10, 3 en el 11
```

Se pulsa reproducir a los **~140 segundos**. **Ninguno de estos sitios se ejecuta al
reproducir: todos ocurren en el primer segundo del arranque.**

Con esto se cierra el círculo de §11.77, §11.81 y §11.89, y la conclusión es más
fuerte de lo que parecía: **toda la maquinaria de audio que esta investigación ha
estado midiendo —el render, el mixer, la copia de PCM, la bomba, la búsqueda de
búferes— es el empuje de silencio del arranque.** Los "ocho búferes", los "ocho
dpump", los "cuatro fetchbail" que se llevan meses persiguiendo no describen una
reproducción rota: describen la inicialización, que funciona.

Y por tanto: **al pulsar reproducir no se entra en el conducto de audio en absoluto.**
El reproductor se abre, aborta (§11.83) y ni siquiera llega a tocar el render. Eso
explica sin residuo por qué la CPU se queda en el WFI y por qué cada nivel que se
trazaba daba "ocho veces".

**Consecuencia para el trabajo futuro.** Los contadores heredados (`dpump`, `mixer`,
`copy16`, `pcmout`, `rendret`, `aacmath`, `framepull`) **no sirven para medir la
reproducción**. Cualquier conclusión anterior que los use como evidencia de qué pasa
al reproducir hay que releerla como evidencia de qué pasa al arrancar. Y toda medida
nueva tiene que llevar **marca de tiempo y tope alto**, o vuelve a mentir igual.

## 11.91 CORRECCIÓN a §11.90: generaliza de una sola corrida

§11.90 concluyó que "todo pasa en el segundo 0" a partir de la corrida `uc1`. Puesto
al lado de otra corrida de la misma sesión, no se sostiene tal cual:

```
uc1:  copy16 4808, dpump 8, mixer 8, fetchbail 4   ->  TODOS en el segundo 0
pt1:  copy16 8, dpump 8, mixer 8, fetchbail 8      ->  primera a los 142,16 s
```

Las marcas de tiempo del plugin son de **reloj de pared**, relativas a la primera
línea registrada (`t0` se fija en el primer log de cualquier tipo), así que las dos
lecturas son válidas y dicen cosas distintas: en `uc1` el conducto de audio corrió en
el arranque y no al reproducir; en `pt1` corrió al reproducir, a los 142 segundos.

**Lo que se sostiene** y sigue siendo importante:

- `cap=8` ocultaba la distribución temporal en todas las corridas anteriores, y sin
  marca de tiempo los contadores no distinguen arranque de reproducción. Esa parte
  de §11.90 es correcta y vale como aviso permanente.
- Los "ocho búferes" no son una medida estable de nada: **el mismo contador vale 8 en
  el arranque o 8 al reproducir según la corrida**, y compararlos entre corridas sin
  mirar el reloj es lo que ha sostenido conclusiones durante meses.

**Lo que NO se sostiene**, y queda retirado: la frase "al pulsar reproducir no se
entra en el conducto de audio en absoluto". En `pt1` sí se entra. Lo correcto es que
**a veces se entra y a veces no**, y esa variabilidad entre corridas es en sí misma
un hecho que hay que explicar antes de seguir.

## 11.92 Un ancla de tiempo de reproducción, y la trampa del toque de despertar en espejo

**El ancla.** Todos los sitios con nombre del mapa heredado disparan también en el
arranque, así que ninguno sirve para abrir una ventana de traza en el momento de
reproducir — y por eso cada intento se llenaba de ruido de boot (§11.90). Lo que sí
es exclusivo de la reproducción es una lectura de disco **dentro del fichero de la
canción**. Añadido al modelo del ATA (`ATALBA_LO`/`ATALBA_HI` acotan el rango en
bytes) y medido:

```
ATASONG #1..#256  byte=177778688..  pc~0x0805a70c
PCs distintos: 1
```

Un único PC del firmware pide todos los datos de la canción. Es código de tiempo de
reproducción por construcción, y es el punto de entrada que faltaba para atacar
"desde la UI hacia dentro".

**La trampa, en espejo de la ya documentada.** El toque de despertar de `iremote.py`
—necesario porque un SELECT en frío se ignora— es el par `e,q`, que neta cero en
medio de una lista. Pero **al final de una lista `e` es no-op y `q` sube uno: neta
−1**. Probando a reproducir por "Shuffle Songs" (la última entrada del menú), el
toque movió el resaltado a "Settings" y la pulsación abrió los ajustes; la captura
previa mostraba "Shuffle Songs / 4 Songs" correctamente resaltado, así que sin mirar
las dos capturas el resultado habría pasado por "Shuffle Songs no reproduce".

```
principio de lista:  q es no-op  ->  usar e,q  (neta 0);  q,e neta +1
final de lista:      e es no-op  ->  usar q,e  (neta 0);  e,q neta -1
```

Y una nota de navegación que sí es robusta: para llegar a la última entrada de un
menú, bajar de sobra y dejar que la lista tope, en vez de contar posiciones. Contar
seis desplazamientos cayó en "Settings"; topar la lista acierta siempre.

## 11.93 LA CADENA DE LLAMADAS DEL REPRODUCTOR, capturada en tiempo de reproducción

Con el ancla de §11.92 (una lectura de disco dentro del fichero de la canción) se
puede además **barrer la pila del invitado** en ese instante y quedarse con los
valores que caen en el rango de código del firmware. El SP es constante en todas las
lecturas (`0x0bee0374`), o sea misma tarea y misma pila, así que la lectura es
estable:

```
0x0805a70c   lectura de disco (driver ATA)
0x08326518   <- lr, retorno del `bl 0x805a6f4` en 0x08326514 (capa de ficheros)
0x0827a6fc
0x08246948
0x08246678
0x082465f0   retorno de un `blx r1` (despacho virtual)
0x0813b20c   retorno de un `blx r1` en 0x0813b208
0x0808dfa8   retorno de un `blx r2` en 0x0808dfa4
0x0813b220
0x0808df88
```

**Es el primer punto de apoyo real dentro del reproductor**, con marcos de pila
auténticos en vez de etiquetas heredadas —que han resultado equivocadas cuatro de
cuatro veces (§11.89)—. Y explica por qué ninguna búsqueda estática los encontraba:
casi todos los marcos son retornos de **despachos virtuales** (`blx` a punteros de
tabla), que no aparecen como `bl` a una dirección fija.

**Confirmado además que el aborto es general.** Reproduciendo por "Shuffle Songs" en
vez de por la lista de canciones —camino de entrada completamente distinto— el
resultado es idéntico: `copy16=8`, `dpump=8`, y a los 33 segundos de vuelta en el
menú. No es un problema de cómo se elige la pista.

**Por dónde seguir.** Los marcos altos (`0x0808df88`, `0x0813b220` y sus llamadores)
son el código que gobierna la reproducción. Ahí hay que buscar la condición que
decide abandonar, con la misma disciplina que ha hecho falta aquí: medir con marca de
tiempo, tope alto y un control conocido al lado de cada sonda.

## 11.94 REFUTADA la hipótesis de la cabecera Xing

El fichero de la canción se lee **445 veces** para 4,7 MB, cuando con bloques de
32-61 KB bastarían unas 118: se relee tres o cuatro veces entero. Eso no es cargarlo
en memoria, es **reintentar**. Y una causa clásica de que un reproductor relea entero
un MP3 es no encontrar la cabecera **Xing/Info** con la longitud del stream — que es
justo lo que se quitó al reencodear con `-write_xing 0` en §11.59, creyendo que "sin
ningún tag" era la configuración más limpia.

Reencodeados los cuatro con `-write_xing 1` y etiquetas ID3v2.3 (como escribe
iTunes), medido:

```
fetchok 0   fetchbail 8   copy16 8   dpump 8
WAV: solo clics, 27 456 muestras no nulas de 16 799 744, pico 11444
```

**Idéntico.** La hipótesis queda refutada, y con ella la lectura de que el patrón de
relectura venga de metadatos que falten en el fichero.

**Aviso de método, otra vez el mismo.** La misma corrida reportó "0 lecturas dentro
de la canción", que parece un hallazgo enorme. No lo es: al reescribir los ficheros
cambiaron de tamaño (~500 bytes) y de sitio en el disco, así que el rango
`ATALBA_LO/HI` —calculado con `filefrag` sobre los ficheros ANTERIORES— dejó de
cubrirlos. **Cada vez que se tocan los ficheros hay que recalcular el rango**, o la
sonda mide un sitio donde ya no hay nada.

## 11.95 CONTROL POSITIVO: el camino de salida SÍ puede llevar música

Faltaba un control que separa dos cosas que se venían confundiendo. Que el DMA
entregue 18 500 búferes y que los clics de rueda lleguen al WAV prueba que el camino
lleva **PCM corto** (un clic son 1 248 muestras); no prueba que sostenga música
durante minutos. Nunca se había verificado.

El shim `HLE_AUDIO_DATAPATH` decodifica en el anfitrión y mete el PCM en el mismo
canal de DMA del firmware. **No es una solución** —no es el reproductor de retailOS—
pero como control responde justo eso:

```
con decodificacion en el anfitrion:  14 987 242 muestras, 5 506 568 no nulas (36,74%), pico 32768
con el reproductor de retailOS:      16 799 744 muestras,    27 456 no nulas ( 0,16%), pico 11444
```

**Música real contra clics.** El camino de salida —DMA, I2S, ritmado, captura— queda
**demostrado** de punta a punta con audio sostenido, no inferido.

Con esto el problema queda acotado sin ambigüedad posible:

| lado | estado |
|---|---|
| entrada (disco -> memoria) | **demostrado**: el fichero entero, y `ff fb 90 64` en el destino correcto (§11.86) |
| salida (memoria -> altavoz) | **demostrado**: 36,74 % de muestras no nulas con audio real (aquí) |
| decodificación en el firmware | **es lo único que falla** |

**Consecuencia para el trabajo futuro:** no volver a investigar el camino de salida.
Está probado. Todo el esfuerzo va a por qué el reproductor aborta al abrirse
(§11.83), con la cadena de llamadas de §11.93 como punto de entrada.

## 11.96 REFUTADO el "bloqueo estructural" de `[engobj+0x44]`

`ipod_classic_hle_np.c` documenta el mecanismo que se dio por causa raíz durante
trece asaltos: la bomba de render aborta con −1 antes de hacer nada cuando
`[engobj+0x44] != 0`, y *un solo* −1 hace que el motor deje de re-postear, el
contador de trabajos decaiga a 0 y el lazo aparque para siempre. Encaja
perfectamente con la firma de ocho búferes que aparece en todas las medidas.

Medidas **las dos ramas de la puerta** por separado (no la comprobación, que es lo
que cuenta un `pc=` en `0x22006170` y habría dado un "8" engañoso):

```
22006170: ldrb r0,[r4,#0x44]     la bandera
2200617c: beq  0x22006194        <- CERO: camino vivo
22006180: mov  r0,#5             <- NO cero: aborta con -1

aborta (0x22006180) = 0
sigue  (0x22006194) = 8
```

**La rama de aborto no se toma ni una vez.** La puerta pasa las ocho veces. El
bloqueo estructural descrito ya no ocurre —probablemente lo resolvieron los arreglos
previos (línea 18 del VIC, motor SHA-1, RTC)— y esa nota del árbol describe un estado
anterior del emulador.

La bandera **sí se escribe** durante la reproducción (`0x22006888` a los 148,6 s, y
varios más a los 163,7 s), pero con valor que deja pasar la puerta.

Hipótesis dieciséis, refutada. Y una nota de método que casi se cuela otra vez: poner
la sonda en la instrucción de la comprobación cuenta cada evaluación, no cada aborto.
**Hay que instrumentar las ramas, no la condición.**

## 11.97 El camino real de la bomba, y la CAUSA acotada: el bloque de descriptores nunca se monta

**El recorrido interno de la bomba**, trazado con `range=0x22006100-0x22006600` y
disparo en su propia entrada (rango pequeño, una ventana por invocación):

```
0x22006118 entrada -> 0x2200615c -> 0x22006194 (la puerta de [engobj+0x44] PASA)
-> 0x220061d0 -> 0x220061e0 -> 0x220061f0 -> 0x22006204 -> 0x22006234
-> 0x2200624c -> 0x22006260 -> 0x22006294 -> 0x220062e4 -> 0x22006304
-> 0x22006324 (espera del evento, con exito) -> 0x2200633c (la busqueda de bufer)
-> 0x22006340 -> 0x2200634c -> 0x220061fc   <-- SALE
```

Sale por `bne 0x220061fc`, que es exactamente la rama de "la longitud devuelta es
cero". Esto **reabre** lo que §11.81 habia cerrado: aquel experimento puso el bit de
la guarda 25 segundos DESPUES de pulsar play, cuando la bomba ya no se invocaba. La
medida era correcta; el experimento estaba mal cronometrado y la conclusion que
colgaba de el, equivocada.

**Repetido bien** —sosteniendo el bit desde antes de pulsar y durante toda la
ventana— sigue fallando:

```
fetchok 0   vivo (0x22006370) 0   sale (0x220061fc) 8
```

Porque la busqueda tiene DOS guardas, y la segunda exige
`[0x22038000 + indice*4 + 10] == 1` en la tabla de ranuras. Y esa tabla tambien esta
a ceros.

**La medida que lo cierra.** Watch de escrituras sobre `0x22038000-0x22038040`
durante una corrida completa con reproduccion:

```
total escrituras: 0
```

**Nadie escribe la tabla de ranuras jamas** — ni en el arranque ni al reproducir.
Junto con el descriptor de `0x2203ff28`, cuyo unico escritor era `copy16` pisandolo
(§11.66), la conclusion queda establecida desde dos angulos independientes:

> **El bloque de descriptores de audio en lo alto de IRAM1 nunca se inicializa.**
> Por eso la busqueda falla el 100 % de las veces, la bomba sale sin producir, el
> motor deja de re-postear y el lazo aparca. Esa es la causa, y todo lo demas que se
> ha medido en meses son sus consecuencias.

Y como el DSP esta refutado (§11.74) y ningun canal de DMA escribe ahi (§11.58), lo
que falta es **codigo del firmware que no llega a ejecutarse**.

**El siguiente paso, concreto.** El global que guarda esos punteros (`0x0896a684`)
se referencia desde seis sitios del firmware:

```
0x080821b4   0x08085f38   0x0808afc8   0x080a3ccc (la busqueda)   0x080b5320   0x08272f34
```

Uno de ellos deberia montar la tabla. Poner un `pc=` en cada uno y ver cual **no**
corre al reproducir nombra al inicializador que falta, y de ahi a la condicion que
lo gatea — que es donde estara el fallo del modelo.

## 11.98 La semántica de la tabla de ranuras, y quién la consume

Sondeados los seis sitios que referencian el global `0x0896a684`, con marca de
tiempo:

```
ctx    (0x08082100)  n=4  +137,07 s    corre al reproducir
s2     (0x08085ef8)  n=3  +137,21 s    corre
evwait (0x0808af58)  n=4  +137,07 s    corre
fetch  (0x080a3c00)  n=4  +137,17 s    corre, y falla
s5     (0x080b5300)  n=0               NUNCA
```

El que nunca corre resultó ser, leído, la rutina de **"ranura consumida"**
(`0x080b52f0`):

```
80b52f4: ldr  r4, [pc,#36]        ; el global del audio
80b52fc: ldr  r0, [r4,#20]        ; indice de ranura
80b5300: ldr  r1, [r4,#12]        ; base de la tabla = 0x22038000
80b5304: add  r3, r1, r0, lsl #2
80b5308: strh r2, [r3,#10]        ; pone a CERO [tabla + indice*4 + 10]
80b5314: bl   0x8014848           ; envuelve el indice
80b5318: str  r1, [r4,#20]        ; y lo avanza
```

Escribe **cero** en exactamente el campo que la segunda guarda de la búsqueda exige
valer **uno**. Con eso la semántica queda fijada sin ambigüedad:

> `[tabla + indice*4 + 10]` es el flag **"ranura lista"**: **1 = hay datos**,
> y `0x080b52f0` lo baja al consumirla. `[global+0x14]` es el índice en curso.

Que no corra nunca es **coherente**, no un fallo: no hay nada que consumir. Y
desplaza la pregunta al **productor**: quién debe poner ese campo a 1 y llenar el
búfer en `0x22038100`.

De los seis sitios, tres corren al reproducir (`ctx`, `s2`, `evwait`) y ninguno es el
productor: `s2` es control del motor (escribe la segunda página de registros vía
`0x080915a4`), `evwait` es la espera del evento con su timeout de 500 ms, y `ctx` es
el constructor de contexto. Así que **el productor está fuera de esos seis**, o es
uno de ellos por una vía que no se ha seguido todavía.

Buscar `strh` con desplazamiento 10 no discrimina —es un offset demasiado común, más
de veinte candidatos genéricos—, así que el camino útil es al revés: partir de quién
escribe el **búfer** `0x22038100` (un watch como los que ya han funcionado tres veces
en esta sesión) y subir desde ahí.

## 11.99 CAUSA RAÍZ ESTABLECIDA: el productor del mecanismo de ranuras no existe

Tres watches independientes, en corridas completas con reproducción real:

```
tabla de ranuras   0x22038000-0x22038040  ->  0 escrituras
bufer PCM          0x22038100-0x22038180  ->  0 escrituras
palabra de estado  0x2203ff28-0x2203ff40  ->  0 escrituras (solo copy16 pisandola, s11.66)
```

**Nadie escribe nada en el mecanismo de ranuras, nunca.** La estructura existe —sus
punteros son datos estáticos copiados en el microsegundo cero (§11.79)— pero el lado
productor está sencillamente ausente.

Con eso la cadena queda cerrada de punta a punta, y cada eslabón con su medida:

```
el productor de ranuras            NUNCA CORRE          <-- LA CAUSA
la busqueda 0x080a3c00             falla el 100% (4-8)      consecuencia correcta
la bomba de render sale por 0x220061fc                       consecuencia correcta
el motor deja de re-postear, el contador decae               consecuencia correcta
el driver de salida hace memset de silencio (~4000)          comportamiento CORRECTO
el DMA entrega 18 500 buferes de ceros al I2S                comportamiento CORRECTO
el WAV trae solo los clics de la rueda                       consecuencia
```

**Todo lo que se ha perseguido durante meses aguas abajo son consecuencias.** El
consumidor funciona y hace lo correcto al no encontrar datos; el driver hace lo
correcto al sacar silencio; el DMA hace lo correcto al entregarlo. El fallo es que
nada produce.

Y esto cierra también, con evidencia, por qué el reproductor se abre y aborta
(§11.83): abre, pide un búfer, no lo hay, y se cierra.

**Lo que queda por saber, en una frase:** qué tarea del firmware debería llenar esas
ranuras y por qué no arranca. No es un problema del camino de audio —entrada y salida
están DEMOSTRADAS (§11.86, §11.95)— sino de que una tarea productora nunca llega a
existir o nunca se planifica.

## 11.100 Las dos regiones de PCM son DISJUNTAS

Juntando dos medidas de esta sesión que no se habían puesto una al lado de la otra:

```
copy16 escribe PCM en IRAM1     0x2203cef4 .. 0x2203d65a   (medido, s11.68)
el DMA de audio lee de IRAM0    0x22008c5c                 (medido, s11.58)
el unico escritor de 0x22008c5c es el memset a CERO        (medido, s11.63)
el bufer de ranuras esta en     0x22038100                 (medido, s11.65)
```

**Tres regiones distintas y ninguna conectada con la siguiente.** El render produce
PCM en un anillo de IRAM1; el DMA reproduce desde un búfer de IRAM0 que sólo recibe
ceros; y el mecanismo de ranuras, que es lo que la búsqueda consulta, vive en una
tercera zona que nadie toca.

Eso encaja con §11.99 y lo hace más preciso: no es sólo que "el productor no
escriba". Es que **el eslabón que debería copiar del anillo del render al búfer que
el DMA reproduce no existe en ninguna corrida**, y por eso el driver de salida
encuentra el búfer vacío y hace lo único correcto que puede hacer: rellenarlo de
silencio.

Conviene decir qué NO prueba esto. Que las tres regiones estén desconectadas es un
hecho medido; que la conexión que falta sea "una copia de IRAM1 a IRAM0" es una
lectura razonable pero **no medida** — podría ser que el firmware deba reprogramar el
DMA para que lea del anillo, o que el mecanismo de ranuras sea el camino bueno y el
anillo de `copy16` pertenezca a otra cosa (el empuje de silencio del arranque, por
ejemplo, que es cuando `copy16` acumula sus 4 808 pasadas).

**La medida que lo decidiría:** comprobar si en alguna corrida el descriptor del DMA
(`0x22010020`) llega a apuntar a IRAM1 en vez de a `0x22008c5c`. Un watch sobre esos
16 bytes lo contesta en una corrida, y distingue "falta una copia" de "falta
reprogramar el DMA".

## 11.101 RESUELTA la ambigüedad de §11.100: falta la COPIA, no reprogramar el DMA

Watch sobre el descriptor del DMA de audio (`0x22010020`, 16 bytes) durante una
corrida con reproducción:

```
+0us      escrito por las rutinas de copia del arranque (0x08000294/98)
+7 ms     reescrito desde IRAM (0x22000294/98)
+20,38 s  reescrito por 0x22004b4c..0x22004b60  <- la rotacion del ping-pong
400 escrituras en total
el descriptor al final:  src=0x22009c5c  dst=0x3ca00010  next=0x22010020
```

**El origen es SIEMPRE IRAM0** —`0x22008c5c` o su pareja `0x22009c5c`—, nunca IRAM1.
El firmware alterna entre los dos búferes del ping-pong y ahí acaba la historia.

Eso resuelve la ambigüedad que §11.100 dejaba abierta: **no falta reprogramar el DMA,
falta quien copie el PCM decodificado dentro de esos búferes de IRAM0.** Las dos
lecturas posibles quedan reducidas a una, con medida.

El código que rota el descriptor está en `0x22004b10..0x22004b64` y es el constructor
del LLI:

```
22004b14: ldr r0, [r0, r4, lsl #2]   ; bufer[indice] de un array de punteros
22004b4c: str lr, [r3, #12]          ; ctrl
22004b54: str lr, [r3, #4]           ; dst  (el FIFO del I2S)
22004b5c: str lr, [r3]               ; src  (el bufer elegido)
22004b60: str r3, [r3, #8]           ; next -> el mismo descriptor
```

Es decir: el driver elige `bufer[indice]`, lo pone como origen y lo encola. Lo que
**nadie** hace es llenarlo antes con audio — sólo el `memset` de silencio (§11.63) lo
toca.

**El punto exacto donde seguir**, y ya sin ambigüedad: quién debería escribir
`0x22008c5c` / `0x22009c5c` con PCM antes de que la rotación los encole. Ese es el
productor ausente de §11.99, localizado ahora en una dirección concreta y con el
código que lo consume a la vista.

## 11.102 Intento fallido de capturar la base de `copy16`, y lo que sí dio

Idea: en la cabeza del bucle (`0x08016efc`) `r3` contiene la base de destino
`[r4+12]`, pero **sólo en la primera vuelta** — luego la pisa la muestra leída. Filtro
propuesto: quedarse con las líneas de `r1` máximo (medias palabras que faltan), que
deberían ser los arranques de copia.

**No funcionó.** Con `regcap=3000` las capturas son una ventana CONTIGUA a mitad de
ráfaga, no los arranques, así que el `r1` máximo observado fue 0xff y los `r3`
correspondientes son muestras de audio (`0x00fffffe`, `0x0000003d`, `0x00ffef9e`),
no direcciones. La base sigue sin medirse.

Para conseguirla haría falta capturar en `0x08016edc` (que sí es destino de salto) y
leer `[r4+12]` de memoria, cosa que `regpc` no hace: da registros, no memoria. La vía
practicable es un breakpoint de gdb con `print` de esa palabra, o una sonda en el
modelo que publique la memoria como ya hacen `ATASONG`/`ATALND`.

**Lo que sí dio la corrida:** el rango de destinos de `copy16` va de `0x2203d502` a
`0x2203fa5a`. Cubre `0x2203ff28`, lo que confirma por tercera vez el pisotón del
descriptor con el envoltorio por defecto (§11.66), y confirma que la región que
escribe el render es disjunta de los búferes que el DMA reproduce (§11.100).

## 11.103 `[r4+12]` no es una base: es un desplazamiento

Leído con un breakpoint de gdb en `0x08016edc` (que sí puede leer memoria, cosa que
`regpc` no):

```
COPYBASE base=0x000119b4  count=20    r4=0x0bb12d54
COPYBASE base=0x000117ac  count=520   r4=0x0bb12d54
COPYBASE base=0x00011660  count=328   r4=0x0bb12d54
COPYBASE base=0x00010000  count=16    r4=0x0bb12d54
COPYBASE base=0x000100ac  count=112   r4=0x0bb12d54
```

Los valores de `[r4+12]` están entre `0x10000` y `0x119b4`: **no son direcciones,
son desplazamientos**. Y `[r4+16]` (el contador) va de 4 a 520 bytes por copia, no
kilobytes.

Eso corrige la lectura de §11.67. La suma real es:

```
destino = [r4+12] (desplazamiento pequeno)
        + [tabla+4] (una BASE que mantiene el firmware)
        + [bloque+0xa48] (NUESTRO registro de posicion del SM1)
```

O sea que la dirección de destino la dominan un valor del firmware **y nuestro
registro**, no un puntero de la estructura de copia. Eso refuerza que el envoltorio
del anillo (`S5L8702_SM1_POS_RING_FRAMES`, marcado como inferencia en el propio
header: *"IRAM1 is 128 KiB, so the position wraps every 32768 frames"*) es un
candidato serio a estar mal — pero también explica por qué cambiarlo movía el destino
sólo parcialmente (§11.68): hay dos sumandos más, y uno es del firmware.

**Y una nota de herramienta, que ya ha costado dos intentos:** `regpc` del plugin da
registros, no memoria. Para leer `[registro+desplazamiento]` hay que usar un
breakpoint de gdb con `printf`, o publicar la memoria desde el modelo como hacen
`ATASONG`/`ATALND`. Intentar deducirlo filtrando capturas de registros dio muestras
de audio disfrazadas de direcciones (§11.102).

## 11.104 El anillo del render está SANO, y el hueco queda caracterizado

Descompuesta la suma del destino con un breakpoint justo después del getter
(`0x08016e64`, donde `[sp]` ya contiene base_del_firmware + posición_del_SM1):

```
GET sp0=0x22025800   r4=0x0bb12d54
GET sp0=0x22025c00   r4=0x0bb12d54     <- avanza en pasos de 0x400
```

El getter devuelve direcciones de **IRAM1 alrededor de `0x22025800`**, y **avanzan
ordenadamente** de 1 KB en 1 KB. Eso no es un puntero desbocado: es un anillo que
progresa como debe. Sumado el desplazamiento `[r4+12]` (§11.103), el render escribe
sus trozos de 4 a 520 bytes dentro de esa zona.

**Con esto el cuadro queda cerrado y cada pieza medida:**

```
el render escribe PCM en el anillo de IRAM1   ~0x22025800, avanzando  <- SANO
el DMA reproduce desde IRAM0                   0x22008c5c/0x22009c5c  <- SANO
el driver rota los dos buferes de IRAM0        0x22004b10..0x22004b64 <- SANO
lo unico que toca esos buferes es el memset a CERO                    <- underrun correcto
FALTA: quien mueva el PCM del anillo de IRAM1 a los buferes de IRAM0
```

Y eso descarta, con medida, la sospecha de §11.103 de que el envoltorio del anillo
(`SM1_POS_RING_FRAMES`) estuviera desviando la escritura: la posición que el firmware
acaba usando es pequeña y ordenada, así que **el desvío no está ahí**. El anillo mal
dimensionado sí corrompe la zona de control (§11.66) y conviene arreglarlo, pero no
es la causa del silencio.

**La frase final del diagnostico, con todo medido:** la reproduccion no suena porque
**nadie copia el audio decodificado desde el anillo del render hasta el bufer que el
DMA reproduce**. Todo lo demas de la cadena —lectura de disco, aterrizaje en memoria,
decodificacion en el anillo, rotacion de buferes, DMA, I2S, WAV— esta verificado y
funciona.

## 11.105 EL MECANISMO COMPLETO: un byte de handshake, y quién lo pone

Siguiendo el llamador del relleno de silencio hasta el fondo, con breakpoints
condicionales de gdb (la única herramienta que filtra en el momento de capturar; las
sondas con tope se llenaban siempre en el arranque):

```
220077b8: bl 0x220002d4     borra el bufer elegido, 4096 bytes
220077bc: bl 0x220073c8     getter -> objeto 0x22010318
220077c0: bl 0x22007440     ldr r0,[obj+0x20] -> firmware 0x0816c150
0816c150: ldrb r0,[r0]      <-- UNA instruccion: devuelve *(byte*)
220077c4: cmp r0, #0
220077cc: beq 0x220077f4    CERO  -> no llena; queda el silencio del borrado
220077ec: bl 0x22006ac0     NO CERO -> LLENA el bufer con audio
```

**Todo depende de un byte.** Y ese byte, medido con watchpoint, **sí se activa**:

```
flag en 0x0bb169dc
SET flag=1  desde lr=0x081fef40   x4
SET flag=0  desde lr=0x220055b4   x1
```

`lr=0x081fef40` sitúa la llamada en `0x081fef3c` (`bl 0x8015b28`), dentro de la misma
función que **`0x081fef28`** — el sitio de envío del firmware que fue la PRIMERA
medida de esta sesión. Y lo pone a 1 **cuatro veces**, que es exactamente el "3
trabajos + 1 arranque" medido entonces.

**Dos correcciones que esto obliga:**

1. §11.63 decía que el borrado del búfer es la ruta de underrun. **No lo es:** es un
   "limpiar antes de llenar" incondicional. El fallo no es que alguien rellene de
   silencio, es que el paso de llenar con audio no se ejecuta.
2. §11.99 decía "el productor no existe". **Existe y funciona** — pone el flag cuatro
   veces. Lo que no hace es repetirlo al ritmo que el consumidor necesita (~43/s).

**El mecanismo, ya completo y sin huecos:**

```
el productor (0x081fef3c) marca "hay audio"     4 veces
el driver borra el bufer y consulta el flag     ~43 veces por segundo
   lo encuentra a 1 -> llena con audio          4 veces
   lo encuentra a 0 -> deja el silencio         el resto
el DMA entrega lo que haya                      18 500 buferes
```

Y con eso la pregunta de toda la investigación queda reducida a su forma mínima, en
el mismo sitio donde empezó: **qué debería hacer que `0x081fef28` se repita.**

## 11.106 LA CADENA COMPLETA, medida nivel a nivel

Bajando desde el silencio del WAV hasta el origen, con breakpoints condicionales de
gdb (la única herramienta que filtra al capturar; toda sonda con tope se llenaba en
el arranque):

```
0x081d05dc  setter: str [obj+0x18]; strb 1,[obj+0x67]   4 veces, solo al arrancar
     |      (asigna un bufer nuevo y levanta la bandera "hay trabajo")
     v
0x081feec8  puerta: ldrb [r4+0x67]; cmp #1; bne         EVALUADA MUCHAS VECES,
     |      (consume la bandera si esta a 1)             byte=0 siempre salvo 4
     v
0x081fef28  envio (blx a 0x081d0944)                     4 veces
0x081fef3c  pone el flag "hay audio disponible" = 1      4 veces
     v
0x220077c0  el driver consulta ese flag (0x0816c150,     ~43 veces/segundo
     |      que es "ldrb r0,[r0]; bx lr")
     v
0x22006ac0  llena el bufer con audio   SOLO si el flag era 1
0x220002d4  borra el bufer (4096 B)    SIEMPRE, antes de consultar
     v
DMA -> I2S -> WAV                                        18 500 buferes
```

**El dato que lo cierra:** la puerta `0x081feec8` **se evalúa repetidamente** —desde
`0x22003fc4` y `0x220072e8`, dos sitios de IRAM— y el byte está a **cero todas las
veces**. Así que el planificador funciona, la tarea corre y el código llega hasta la
puerta. Lo que no ocurre es que alguien vuelva a asignar un búfer.

**La frase final:** el reproductor no suena porque la asignación de búfer
(`0x081d05dc`) sucede **cuatro veces al arrancar la reproducción y nunca se repite**,
mientras el driver la necesita unas 43 veces por segundo. No falta código, no falta
estructura, no hay interbloqueo: falta el evento que reengancha el ciclo.

**Lo que queda por saber, y es lo único:** quién debería llamar a `0x081d05dc` de
forma recurrente. En hardware real ese llamador se dispara con cada búfer consumido;
aquí sólo lo hace al principio. Ahí estará el evento que el modelo no produce — y es
exactamente el mismo sitio al que apuntaba la primera medida de esta sesión
(`0x081fef28`, "tres trabajos donde harían falta cuarenta por segundo"), ahora con
todo el mecanismo intermedio explicado.

## 11.107 LOS DOS ASIGNADORES SON DE ARRANQUE: no existe el camino recurrente

Armando el breakpoint **antes** de pulsar reproducir (engancharlo 25 s después no ve
nada, porque las asignaciones ocurren AL pulsar — mismo error de cronometraje que
§11.81):

```
ASIGNA obj=0x0bae8ad8 valor=0x0ba77cf4 desde lr=0x0814ff50
ASIGNA obj=0x0bae8ad8 valor=0x09555db0 desde lr=0x08150ff8
ASIGNA obj=0x0bae8ad8 valor=0x0ba6f9f0 desde lr=0x0814ff50
ASIGNA obj=0x0bae8ad8 valor=0x09555db0 desde lr=0x0814ff50
ASIGNA obj=0x0bae8ad8 valor=0x0ba6f9f0 desde lr=0x08150ff8
ASIGNA obj=0x0bae8ad8 valor=0x093afe58 desde lr=0x0814ff50
```

**Dos llamadores, los dos de inicialización:**

- `0x08150ff8` está dentro de `0x08150d84`, la función de arranque de reproducción.
- `0x0814ff50` es el retorno de `bl 0x8183d74` en `0x0814ff4c`, precedido de una
  ristra de `strb` a `[r4+0x93d..0x941]` y llamadas de construcción — inicialización
  sin ambigüedad.

**No existe un camino recurrente de asignación.** En hardware real, quien vuelve a
asignar es el retorno del ciclo: el DMA consume un búfer, su interrupción llega al
driver, y el driver pide el siguiente al reproductor. Aquí ese último tramo —del
driver de vuelta al reproductor— es el que no ocurre.

Y encaja con todo: la interrupción del DMA llega (18 500 terminal-counts), el driver
corre (borra y consulta el flag ~43 veces por segundo), pero **no reengancha con el
productor**. Por eso el productor sólo trabaja las seis veces del arranque.

**El diagnóstico final de esta investigación, en una frase:** el ciclo de audio de
retailOS es reactivo —consumir un búfer debe pedir el siguiente— y en esta emulación
**el lazo se abre justo en el retorno del driver al reproductor**. Todo lo demás
—disco, memoria, decodificación, anillo, DMA, I2S— está medido y funciona.

## 11.108 El aviso del driver, y quién lo lee

La función que entrega búferes al DMA (`0x22007780`, cuyos literales son
`0x22008c5c` y `0x22009c5c`) publica su estado en las dos ramas:

```
hay datos -> bl 0x220043fc con 1     no hay -> bl 0x220043fc con 0
```

`0x220043fc` es un set/clear de bits sobre una **máscara de estado en `0x2200ae2c`**,
bajo sección crítica (`0x22001e58` / `0x22001e6c`). Así que el driver **sí publica**
que se ha quedado sin datos, en cada vuelta.

Con un watchpoint de LECTURA (`rwatch`, no de escritura: las escrituras ya se
conocían, lo que faltaba era saber si alguien escucha):

```
0x22004418   dentro del propio 0x220043fc
0x220029a0   el despachador del kernel
0x220029e4   idem
valor leido = 32  (bit 5)
```

Y el despachador hace:

```
2200299c: ldr r0, [r4]        la mascara
220029a4: and r0, r0, #70     0x46 = bits 1, 2 y 6
220029a8: orrs r0, r0, [r4+12]
220029b8: bne  -> despacha
```

**El planificador sólo considera los bits 1, 2 y 6; el driver de audio usa el bit 5,
que queda fuera de esa máscara.** Anotado como hecho medido, con la reserva de que
probablemente sea por diseño —un bit de estado, no de despertar—, ya que el mismo
código corre en hardware real. No conviene tratarlo como fallo sin más evidencia.

**Estado del cerco al cierre de esta línea:** el driver publica correctamente que no
tiene datos, y el planificador funciona. Lo que no se ha encontrado es el camino por
el que ese aviso debería llegar de vuelta al reproductor para que asigne el siguiente
búfer. Los dos asignadores conocidos (§11.107) son de inicialización, así que el
camino recurrente existe en el firmware pero no se ha localizado todavía.

## 11.109 LOS CUATRO SITIOS QUE NO SE ALCANZAN, y la función que los gatea

Búsqueda estática: **seis** sitios del firmware llaman a `0x08183d74`, que acaba
asignando un búfer y levantando la bandera de envío:

```
0x0814f1a8   0x0814ff4c   0x08150ff4   0x081516c8   0x081529bc   0x08152ca0
```

Sondeados los seis con gdb, armado ANTES de pulsar reproducir:

```
LLAMA 0x0814ff4c (init)      x4
LLAMA 0x08150ff4 (arranque)  x2
los otros cuatro: NINGUNA vez
```

**Sólo se ejecutan los dos de inicialización.** El camino recurrente existe en el
firmware y no se entra en él.

**Un fallo de herramienta que costó dos corridas, y que conviene no repetir:**
`commands 1-6` no es sintaxis válida en este gdb. Los breakpoints quedan armados SIN
bloque de comandos, así que al saltar gdb **detiene la máquina y espera**: la interfaz
se congela y la pulsación de reproducir nunca entra (`press ret x20: diff=0`). Eso se
lee como "ningún sitio se ejecuta" cuando en realidad el emulador estaba parado en el
primero. Hay que poner **un bloque `commands N` por breakpoint**, cada uno con su
`continue`.

**La estructura de los tres sitios hermanos** (`0x081516c8`, `0x081529bc`,
`0x08152ca0`) es idéntica:

```
ldrb r0, [sp,#0x38]      un booleano local
cmp  r0, #0
beq  <salta>             <-- si es 0, NO asigna
bl   0x81afbe0 ; bl 0x81b0030
ldr  r0, [r4,#0x948]
bl   0x8183d74           ASIGNA
```

Y ese booleano lo decide `0x08150ad0`, al que se le pasa su dirección
(`add r3, sp, #0x38 ; bl 0x8150ad0`):

```
8150ad4: sub  sp, sp, #1024      un bufer de 1 KB en pila
8150af4: strbne r9, [r8]         *flag = 0 al entrar
8150af8: bl   0x82685a0          primera comprobacion
8150b00: beq  0x8150b6c          si falla -> sale con flag = 0
```

**Ahí está el siguiente paso, ya sin ambigüedad:** `0x08150ad0` pone el booleano a
cero al entrar y sólo lo levanta si pasa una cadena de comprobaciones. Saber cuál
falla —empezando por `0x082685a0`— nombra la condición que impide que el reproductor
vuelva a pedir búferes, que es el único eslabón que queda.

## 11.110 LA CONDICIÓN EXACTA: el contenedor del reproductor está vacío

Bajando la última bifurcación de `0x08150ad0` (§11.109) con gdb sobre una instancia
viva:

```
8150c10: blx r1            llamada virtual [[obj]+0x1b8]
8150c14: cmp r0, sl        r0 = 0  y  sl = 0  -> IGUALES
8150c24: beq 0x8150c3c     <-- salta: "no ha avanzado nada", no pide bufer
```

El método resuelto en caliente es **`0x0814fe60`**:

```
814fe68: add  r0, r0, #0x60      el contenedor en [obj+0x60]
814fe6c: bl   0x82685a0          tiene elementos?
814fe74: ldrne r0, [r4,#0x64]    si SI  -> r0 = [obj+0x64]
814fe78: ldrne r0, [r0,#0x12c]   -> devuelve ese campo
814fe7c: moveq r0, #0            si NO  -> devuelve 0
```

Y leido en la maquina (obj = `0x0ba954b4`):

```
[obj+0x60].. = 0x088ef3e4 0x00000000 0x00000000    el contenedor
[obj+0x64]   = 0x00000000                          NULO
```

**El contenedor está vacío y no hay elemento actual.** Por eso el método devuelve
cero, la comparación da igualdad, y el reproductor concluye que no ha habido progreso
y no vuelve a pedir búferes.

**Reserva de método, explícita:** esta lectura se tomó con el reproductor YA cerrado,
así que por sí sola podría estar describiendo el estado posterior al aborto y no el
de la reproducción. Lo que la sostiene es la medida de §11.109, tomada **durante** la
reproducción, donde esa misma llamada devolvía 0 en las seis ejecuciones observadas.
Ambas apuntan al mismo sitio, pero conviene no presentar la lectura tardía como si
fuera prueba por sí misma.

**Lo que queda:** por qué el contenedor de `[obj+0x60]` está vacío mientras se
reproduce. Ahí se une con §11.99 —las estructuras de audio que nadie llena— pero
ahora con el objeto concreto y el campo concreto identificados.

## 11.111 RETRACTACIÓN de §11.110, y la cadena real hasta el silencio

§11.110 dijo que el contenedor del reproductor estaba vacío. **Es falso**, y la
reserva que la propia sección dejaba anotada era el motivo: aquella lectura se tomó
con el reproductor ya cerrado. Parando en `0x0814fe70` —el `cmp` que sigue a la
validación, que es donde se decide la rama— durante la reproducción:

```
[ 1..14] validacion PASA -> referencia viva; campo [0x0ba6d5d8+0x12c] = 0x00000000
```

La referencia débil está **viva** las catorce veces. Y el objeto al que apunta lleva
`vtable = 0` y la etiqueta `'plst'` en `+4`: es un objeto de **lista de
reproducción**. Esa rama era la lógica de cambio de pista, no la del relleno de
búferes. Vía muerta, y anotada como tal.

### El sitio donde se decide el silencio

El byte que el driver consulta antes de rellenar (§11.63) vive en `0x0bb169dc`,
dentro de un descriptor con `0x0000ac44` = 44100 y 2 canales. Vigilándolo:

```
ESCRIBE byte -> 1   pc=0x0816bdd4  lr=0x081fec00
ESCRIBE byte -> 0   pc=0x0816bdd4  lr=0x220055b4
```

**Corrección de método, y va la tercera vez que este error cuesta una conclusión:**
de ahí deduje "se activa una sola vez". Falso. Un watchpoint de gdb solo avisa cuando
el valor *cambia*, y la rama de envío inmediato escribe −1 sobre −1 y 1 sobre 1.
Contando EJECUCIONES con sondas del plugin:

```
decide 4   defer 4   submitnow 3   setavail 3   setavail2 1   gateraise 3
```

Cuatro vueltas, no una. La regla de §11.66 —instrumentar las ramas, no la
condición— tiene ahora una hermana: **contar ejecuciones, no cambios de valor.**

### Por qué se para en cuatro

El bucle de audio en IRAM:

```
22006324: bl 0x22003850     ; espera de credito de render -> 0x0808af58
22006328: cmp r0, #0
2200632c: beq 0x22006310    ; 0 -> duerme y reintenta
2200631c: cmp r7, #1
22006320: bgt 0x220061fc    ; a la segunda, abandona
```

El comentario del propio modelo (`ipod_classic_sm1.c`, `SM1_IRQ_EVERY`) sostenía que
el tope de 8 venía de que esa espera **agotaba su plazo**. **Medido, es falso:**

```
[1..8] la espera devolvio 0x00000001 -> EXITO
```

Se satisface las ocho veces. La bomba sale después, en `0x22006358`, que exige tres
cosas a la vez. Leídas las tres:

```
[sp+24]=0x00000000  OK      [r4+0x58]=1  OK      r0=0x00000000  <-- hace falta 2
```

`r0` es lo que devuelve `0x22003850`+8 → **`0x080a3c00`**, el obtenedor del siguiente
búfer de render. Devuelve 0 en vez de 2 las ocho veces.

### La guarda, y el estado que nadie mantiene

`0x080a3c00` cuelga de una tabla global en `0x0896a684`:

```
0x896a684:  0x00000000  0x2203ff28  0x2203ff00  0x22038000  0x22038100
              descriptor ^          bootinfo ^   ranuras ^    PCM ^
```

y tiene dos guardas:

```
80a3c2c: tst r2, r0     ; r2 = mascara de habilitacion, halfword en 0x2203ff28
80a3c30: beq salir      ; guarda 1
80a3c4c: cmpeq r3, #1   ; r3 = estado de ranura, halfword en 0x22038000+idx*4+10
80a3c50: bne salir      ; guarda 2
```

Medido: **corta la guarda 1**, `mascara=0x0000` con `bit=1`, las cuatro veces.

Y con `memw=` del plugin sobre TODA la ejecución (≈180 s, arranque incluido), los
únicos PCs que escriben en `0x2203ff00..0x2203ff60` son:

```
32 x pc=0x0800f54c -> 0x2203ff00..07     8 x pc=0x08081da0 -> 0x2203ff00 (halfword)
 2 x pc=0x0800f52c -> 0x2203ff08, 0x0c
```

**Nadie escribe `0x2203ff28` jamás.** El array de ranuras (`0x22038000`) sí se
escribe, pero solo una vez en el arranque (64 palabras desde `0x08085ebc`) y nunca
durante la reproducción.

### Prueba por intervención

Forzando la máscara en el punto de la guarda —sin tocar el firmware— la guarda 1 pasa
y se alcanza **por primera vez** la guarda 2, que revela la segunda pared:

```
[1..8] mascara forzada a 1  ->  guarda 2: estado de ranura = 0 (hace falta 1) -> CORTA
```

Forzando las dos, `0x080a3c00` entrega el búfer. La máquina se queda luego en negro,
que es lo esperable al entregar un búfer con estado inventado.

### Lo que esto nombra

Los dos campos que cortan —la máscara en `0x2203ff28` y el estado de ranura en
`0x22038000`— son estado que vive **en IRAM1** y que **ningún código de la CPU
escribe durante la reproducción**. Nuestro modelo del SM1 es un banco de registros:
no escribe memoria nunca. El firmware le manda 519 COMMIT y 518 RUN (§11.x) y luego
lee el resultado en esas estructuras.

Es decir: **el SM1 no es solo un periférico de registros, es un motor que mantiene su
estado en IRAM1**, y esa mitad del modelo no existe. Eso explica por qué cada guarda
que se forzaba revelaba otra debajo: no faltaba un bit, falta el productor de todo
ese estado.

### Lo que NO se ha probado

Cuál es exactamente el contrato de memoria del motor. Inventarlo campo a campo es
justo lo que la cabecera de `ipod_classic.h` advierte —"rellenarlos a partir de una
suposición verosímil es inventar con apariencia de rigor"— y las dos guardas ya
forzadas muestran que detrás hay más. No se ha reducido `SM1_POS_RING_FRAMES`: la
prueba con `SM1_POS=24576` dejó la máscara igual a cero y los contadores idénticos,
así que el anillo no era la causa (aunque siga siendo cierto que el puntero de
escritura se sale de IRAM1 y que `IRAMSPILL` lo tapa).

**La música sigue sin sonar.**

## 11.112 EL SM1 PUBLICA SU ESTADO EN IRAM1 — muro de la bomba de render roto

El descriptor de trabajo que el firmware entrega al motor, leído en caliente en
`0x08082194`:

```
descriptor @0x08ade494:  +0=0x08ad003c  +4=0x08ad003c  +8=1  +12=1  +16=0x38500100
cadena     @0x08ad003c:  +0=0 (fin)  +4=0x01a10f80  +8=0x88ad004c
bufer      @0x08ad004c:  ff fb 90 64 ...        <-- cabecera de trama MP3
```

y el registro correspondiente:

```
0x38500100 = 0x08ad003d      la direccion de la cadena | bit 0 = marcha
0x38500110 = 0x00000060      sub-bloque 4 = STOP (96)
```

Es decir: **el SM1 recibe una cadena de descriptores en SDRAM con tramas MP3**, por
el offset `+0x00` del sub-bloque 4, que nuestro modelo no atiende (solo trata `+0x10`
orden y `+0x14` estado).

### Lo que SÍ se puede fundamentar, y por qué

Barrida la página de 4 KiB entera, **ningún registro contiene `0x22038100` ni
`0x22038000`**: el destino de la salida del motor no se le programa por registro y no
se deduce de nada medido. Modelar la descodificación sería inventar.

Pero la **geometría de los búferes no es una constante oculta: la publica el motor**.
El firmware no la lleva encima, la lee de vuelta:

```
80a3c54: ldrh r1,[desc,#4]        ; marcos por ranura   <- lo publica el motor
80a3c58: ldrh r0,[ranura,#12]     ; muestras en ranura  <- lo publica el motor
80a3c68: add r1, pcm, r1,lsl#1    ; bufer = PCM + idx*marcos*2
80a3c64: lsl r0, r0, #1           ; tamano = muestras*2
```

Así que cualquier geometría coherente que quepa por debajo de los descriptores vale.
`SM1_SLOTS=1` publica 2048 marcos por ranura (la ráfaga del propio DMA de audio,
medida en el LLI autoenlazado) y 7 ranuras: 7 × 4096 = 28 KiB, que acaban en
`0x2203f100`, libres de `0x2203ff00`.

### Medido, con control

Misma navegación, mismo binario, única diferencia la variable:

| | sin `SM1_SLOTS` | con `SM1_SLOTS` |
|---|---|---|
| `rendret` | 8 | **2505** |
| `bufok` (búferes entregados) | **0** | **2504** |
| `dpump` | 16 | **2505** |
| `framepull` (tramas al códec) | 1 | 1 |

La bomba de render pasa de rendirse a las 8 invocaciones —el número invariante de
todo el proyecto— a **correr a ~38/s**, con 43/s como cadencia real. Y visualmente:
sin el cambio, a los 18 s el aparato ya ha vuelto a la lista *Songs*; con él, sigue
en el reproductor a los 18 **y a los 38**.

### El muro siguiente, y que no es mío

`framepull = 1` en AMBAS columnas: al descodificador se le pide **una sola trama**,
igual con el cambio que sin él. Es preexistente (ya estaba anotado como
"decoder single-frame one-shot") y ahora queda aislado: la bomba corre y rellena
silencio porque nadie le entrega tramas.

**Regresión honesta:** `decide` (la puerta de envío) baja de 8 a 1 y `setavail` de 7
a 0 con el cambio. No está explicado. Por eso `SM1_SLOTS` queda **apagado por
defecto**: es un avance medido, no todavía una ganancia neta.

**La música sigue sin sonar**, pero el reproductor ya no se rinde.

## 11.113 RETRACTACIÓN: el pico de 32768 no era del camino del invitado

En §11.112 di por bueno que, con el motor rellenando el anillo desde el
descodificador del anfitrión, **el pico del WAV pasaba de 11444 a 32768** y que eso
probaba que por el camino del propio invitado circulaba PCM auténtico.

**Es falso.** El control lo desmiente sin ambigüedad — misma decodificación, única
diferencia mi cambio:

```
HLE_AUDIO_DATAPATH solo            21.058% no nulas   pico 32768   (tramos 16..20 llenos)
+ SM1_SLOTS rellenando el anillo    0.20-0.47%        pico 11444
```

El 32768 era del **atajo del anfitrión**, que sigue alimentando el DMA hasta que mi
primera publicación de ranuras lo apaga con `hle_np_owns_feed`. Es decir: no solo no
lo aportaba mi camino, sino que mi cambio **quitaba** el audio que sí funcionaba.

El relleno de PCM queda retirado. Y con él retirado, la combinación sigue dando
0.258%: con la bomba de render viva, el render del invitado **escribe silencio
encima** del PCM que el atajo deja preparado en el mismo búfer del DMA. Los dos
mecanismos se disputan el mismo destino, y el que tiene datos reales solo gana
mientras la bomba está muerta.

### Lo que sí queda en pie

`SM1_SLOTS` publica el estado del motor en IRAM1 y eso **sí** revive la bomba de
render, con control y de forma repetida: `rendret` 8 → 1343 (~20/s, la cadencia
exacta de ranura a 44100 Hz) y el reproductor deja de abortar a los 5-18 s, sigue
abierto a los 38. Eso es real y está medido.

### El muro que queda, sin adornos

Reanimar la bomba solo sirve si la bomba tiene audio que mover, y el suyo lo produce
el descodificador del propio firmware, que **corre una sola vez**. Ese es el mismo
muro que el proyecto ya tenía, ahora aislado: ya no está tapado por el de la bomba.

**La música no sale del reproductor nativo.**

## 11.114 Con la bomba viva: el conducto entero funciona y el productor entrega ceros

Con `SM1_SLOTS=1` la bomba recorre por fin su rama de éxito (`0x22006370`), que nunca
se había ejecutado. Ahí está el punto donde pide el audio:

```
22006464: bl 0x22003808          ; -> 0x081db410
22006474: bl 0x22003810          ; -> 0x081db4c8   PIDE audio, salida en [sp+52]
22006478: cmp r0, #0
22006480: bne 0x22006500         ; si falla, la bomba sale
22006494: blne 0x22000020        ; si no, COPIA origen -> ranura, r5*4 bytes
```

Medido durante la reproducción:

```
de 300 peticiones: 300 con datos, 0 fallidas
[ 1] r0=0  muestras=1024  origen=0x08a873c0  destino=0x22038100
[ 2] r0=0  muestras=1024  origen=0x08a8b3c0  destino=0x22038100
     ... alternando dos buferes de origen, doble bufer en SDRAM
```

Y el índice de ranura rota como debe, módulo el contador que publica el modelo:

```
[1] modulo->1  [2] ->2  [3] ->3  [4] ->4  [5] ->5  [6] ->6  [7] ->0   contador=7
```

Es decir: **el conducto entero funciona**. La guarda entrega búferes, el índice rota
por las siete ranuras, la petición de audio nunca falla, la copia se hace.

### Y sin embargo

```
[  3] origen=0x08a873c0  muestras=1024
0x8a873c0:  0x00000000 0x00000000 0x00000000 0x00000000 ...
[ 60] origen=0x08a8b3c0  ... todo ceros
[150] origen=0x08a8b3c0  ... todo ceros
```

**El productor entrega 1024 muestras de silencio, cada vez.** No falla, no se rinde,
no se queda sin crédito: devuelve éxito y devuelve ceros.

Esto reemplaza a "el descodificador corre una sola vez", que se apoyaba en sondas mal
nombradas (`framepull` 0x081e93d8 resultó ser un drenaje de anillo de notificaciones,
no el códec). La formulación correcta y medida es: **todo lo que hay por debajo del
productor funciona; el productor, `0x081db4c8`, entrega silencio.**

Los búferes de salida del códec son `0x08a873c0` y `0x08a8b3c0` en SDRAM, en doble
búfer. Ese es el siguiente objetivo, y es concreto: quién debería escribir en ellos.

**La música no sale del reproductor nativo.**

## 11.115 SM1_DECODE: el sitio correcto, y un control que lo desmiente igual

El productor `0x081db4c8` **no es un descodificador**: es un repartidor de dos búferes
fijos, que devuelve éxito siempre.

```
81db4f0: ldrne r1, =0x08a873c0     ; dos direcciones, literales
81db4f4: ldreq r3, =0x08a8b3c0
81db504: str r1,[r2]               ; entrega una
81db510: mov r0,#0                 ; y siempre reporta exito
```

El firmware copia de ahí 1024 marcos estéreo (`r5*4` = 4096 bytes, medido). O sea que
esos dos búferes son el destino REAL donde el motor debe dejar su PCM — no el anillo
de IRAM1 que §11.113 probó y falló. `SM1_DECODE=1` los rellena.

Además, dos errores propios corregidos con medida:

- **Factor 2 en el ritmo.** Una ranura son `SM1_SLOT_FRAMES` *muestras* de 16 bits =
  4096 bytes, que en estéreo son 1024 **marcos**, y `pos` cuenta marcos. Dividir por
  2048 liberaba ranuras a la mitad del ritmo al que se consumen.
- **La posición venía del sitio equivocado.** Con decodificación, `sm1_stream_pos`
  tomaba el cursor del anfitrión en vez del I2S que mueve el invitado. Medido: la
  bomba se congelaba en ~44 activaciones incluso con ventana de 150 s.

Corregidas las dos, la medida es espectacular... y engañosa:

```
sin SM1_DECODE   0.258% no nulas   pico 11444   rendret   44
con SM1_DECODE  28.754% no nulas   pico 32768   rendret 1699   (tramos 15-20 continuos)
```

### El control que lo desmiente

`SM1_FEED_OFF=1` apaga el atajo del anfitrión, dejando el camino del invitado como
única ruta posible del PCM al altavoz:

```
28.754%  ->  0.181%      pico 32768 -> 11444
```

**La música era del atajo, no del camino del invitado.** El 28.75% frente al 21.06%
del atajo solo se explica porque `SM1_SLOTS` mantiene vivo al reproductor más tiempo,
así que el atajo alimenta durante más rato -- no porque el invitado aporte nada.

`SM1_DECODE` sí pone el PCM donde el invitado lo lee, y la bomba sí lo copia a las
ranuras. Lo que no ocurre es el **último salto**: de la ranura de IRAM1 a los búferes
de DMA de IRAM0, que hace el driver de salida y que está condicionado al byte de
"audio disponible" (§11.111). Ese byte lo levanta la ruta de envío, y `decide` sigue
valiendo 1.

**Ese último salto es todo lo que queda. La música no sale del reproductor nativo.**

## 11.116 Dónde queda la cadena, y el único hueco que resta

Con `SM1_SLOTS` + `SM1_DECODE`, medido durante la reproducción:

| eslabón | estado |
|---|---|
| byte de "audio disponible" (0x0bb169dc) | se pone a **1 y se queda en 1** -> el driver de salida rellena |
| repartidor `0x081db4c8` | entrega búfer, **300/300 con éxito** |
| bomba de render | **1699 activaciones**, ~26/s |
| índice de ranura | rota 0..6 módulo el contador publicado |
| copia origen -> ranura | se hace, 4096 bytes por vuelta |
| **contenido** | **ceros, en origen y en ranura** |

Es decir: **toda la cadena funciona y transporta silencio.**

Y el hueco queda acotado a un punto: los dos búferes de salida del códec,
`0x08a873c0` y `0x08a8b3c0`, los rellena el propio firmware con un `memcpy`
(0x22000198, 284 escrituras medidas) y lo que copia son ceros. Tanto, que
**sobrescribe incluso el PCM que `SM1_DECODE` deja ahí** -- comprobado: con la
variable activa, el búfer sigue leyéndose a cero en las peticiones 40 y 120.

Así que el descodificador real está aguas arriba de ese `memcpy`, y produce silencio.
Localizarlo con un breakpoint es inviable: `0x22000198` es el `memcpy` del runtime, se
ejecuta constantemente, y tres pasadas se perdieron porque congela la interfaz y la
pulsación de reproducir no llega. Con `sendex=` del plugin (filtro por registro, sin
parar la máquina) otras dos pasadas se perdieron por lo mismo: con `SM1_DECODE` +
decodificación real la máquina va tan cargada que el arnés no consigue pulsar. **Eso
es un límite del arnés, no un resultado**, y conviene anotarlo antes de que alguien
lo lea como "no hay copia".

### Balance de la sesión

- **`SM1_SLOTS`** es un avance real y atribuido con control: bomba de render 8 -> 1343
  (~20/s, la cadencia exacta de ranura a 44100 Hz) y el reproductor deja de abortar a
  los 5-18 s -- sigue abierto a los 38, con capturas.
- **`SM1_DECODE`** pone el PCM en el sitio correcto y medido, pero el firmware lo pisa;
  y su aparente ganancia de audio (28.75%) la desmintió `SM1_FEED_OFF` (-> 0.18%).
- Cinco intentos de arreglo: uno en pie, cuatro retirados o desmentidos por sus
  propios controles.

**La música no sale del reproductor nativo.**

## 11.117 SALE MÚSICA por el camino del propio reproductor

La dirección de la copia estaba al revés en mi lectura, y eso era todo. El llamante de
la entrega del búfer es `lr=0x220064f4` -- **la propia bomba**:

```
22006488: ldrne r0,[sp,#52]   ; destino = el bufer del codec que acaba de adquirir
2200648c: ldrne r1,[sp,#28]   ; origen  = la ranura de IRAM1
22006494: blne 0x22000020     ; ranura -> bufer del codec
220064f0: bl  0x081db520      ; y lo entrega  (lr = 0x220064f4)
```

Los búferes del códec son la **salida** de la bomba, no su entrada. El audio tiene que
estar ya en la **ranura de IRAM1** -- que es lo que "lista" significaba desde el
principio (§11.112). Rellenar los búferes del códec (§11.115) era escribir en el
extremo lejano de la copia, donde la bomba lo sobrescribía acto seguido con la ranura
vacía: por eso seguían leyéndose a cero.

`SM1_DECODE` ahora rellena la ranura. Y con `SM1_FEED_OFF=1`, que apaga el atajo del
anfitrión y deja el camino del invitado como **única ruta posible** del PCM al
altavoz:

```
relleno en el bufer del codec   0.181% no nulas   pico 11444
relleno en la RANURA            5.676% no nulas   pico 32768   (repetido: 5.632%)
                                                  tramos 19-20 con musica continua
```

Mismos indicadores, misma navegación, lo único que cambia es el destino del PCM.
**La música sale por: motor -> ranura IRAM1 -> bomba de render -> búfer del códec ->
driver de salida -> DMA -> I2S. Todo eso es el firmware de retailOS.**

Es el mismo sitio al que apuntaba el primer intento, y midió peor entonces por tres
razones desde corregidas: el ritmo de liberación estaba desviado por 2, la posición
venía del cursor del anfitrión en vez del I2S, y se apagaba el atajo sin que la bomba
estuviera viva para sustituirlo.

### Por qué empieza tarde

La música aparece en el último ~10% de la captura porque el descodificador tarda en
tener PCM: el registro dice `decoded track 3 -> 52187136 PCM bytes (295.8 s)`, y hasta
que eso termina `have_pcm()` es falso y la ranura se rellena con nada. No es un fallo
del camino: es latencia de arranque del códec emulado.

### El arnés, arreglado de paso

`press` verifica cada intento comparando capturas, y con esta configuración la máquina
va tan cargada que la captura falla y la pulsación se pierde -- **cinco pasadas
perdidas así**. `seq ret:120` envía la tecla sin captura y no depende de la carga; la
entrada se confirma con el contador `StartPlayback`, no con una imagen.

## 11.118 La pantalla de "Low Battery", y el reproductor que se congela

### El aviso de batería (RESUELTO)

El usuario informó de que el aparato mostraba una pantalla de batería y se apagaba.
Reproducido y capturado: es **"Low Battery -- Connect to Power"**, exactamente el
síntoma que `ipod_classic_pmu.c` daba por no resuelto ("subir el ADC de batería a
fondo NO evita que retailOS lo ponga, así que el veredicto no sale del canal que este
modelo contesta").

Con `PMUTRACE=1` se ve que el ADC **sí se lee bien**: `0x55 = 0xd3` = 211, que son los
844 de 1023 que el modelo publica. O sea, la tensión no es el problema. Lo que ocurre
es que `MBCS1_HEALTHY = 0x00` describe "a batería, sin cargador", y el firmware está
pidiendo justamente que lo enchufen -- y nada le dice nunca que lo esté.

`PMUCHARGER=1` reporta adaptador presente y correcto. **Medido:** desaparece la
pantalla, el icono de batería pasa a verde con el rayo de carga, y el aparato deja de
apagarse. Es una elección de modelo, no una medida, y va tras variable por eso: un
iPod emulado no tiene celda que descargar, así que "en la base" lo describe al menos
tan honestamente como "con una batería que nunca se mueve".

### El aviso de fin de fotograma (PARCIAL)

El bloque LCD tiene línea de interrupción conectada a `S5L8702_LCD_IRQ` y un `INTCON`
en +0x18 que el modelo guardaba y no usaba: **la línea no se había levantado nunca**.
`LCD_FRAME_IRQ=1` la pulsa al arrancar cada fotograma.

Medido: **la animación de entrada al reproductor pasa de congelarse a completarse.**
Sin la variable, la capa 1 se queda clavada a 320x240 en (0,0); con ella llega a
`width=-80, x=400`, que es el final del barrido documentado en el compositor. Pero no
basta para que aparezca la pantalla del reproductor, y en algunas pasadas deja la
bomba de audio en 3 activaciones en vez de 1451, así que **no está resuelto**.

### Lo que queda, acotado

Con `PMUCHARGER=1` el audio suena bien (21% de muestras no nulas, pico 32768,
`rendret` 1577) mientras **la interfaz se queda congelada en la carátula genérica**:

- SELECT, MENU y la rueda no cambian un solo píxel -- comprobado con hash de las
  capturas, cinco pulsaciones seguidas, imagen idéntica.
- La CPU está dormida en el WFI del bucle ocioso, no girando.
- El disco sigue leyendo la canción y el audio sigue saliendo.

O sea: **no es un fallo de dibujo, es la tarea de interfaz bloqueada**, esperando algo
que no llega, mientras el resto del sistema sigue vivo. Volcadas las cinco superficies
del compositor, ninguna contiene una pantalla de reproducción con barra de progreso:
capa 0 el reloj, capa 1 la carátula, capa 2 la lista con el icono de altavoz. El
firmware nunca la dibuja.

**No se ha conseguido ver el reproductor completo.**

## 11.119 EL REPRODUCTOR COMPLETO: el compositor tenía las capas al revés

La pantalla de Now Playing **siempre se estuvo dibujando**. La tapábamos.

Volcadas las superficies con el reproductor abierto, la capa 0 contiene el reproductor
entero -- barra "Now Playing", artista, álbum, "4 of 4" y la barra de progreso -- y la
capa 1 la carátula, opaca y a pantalla completa. El compositor dibujaba de atrás a
adelante empezando por la ranura 0, así que la carátula borraba el reproductor y
dejaba justo el lado derecho en blanco que hacía pensar en una pantalla a medio
dibujar.

Y hay una segunda mitad del mismo error: **el modelo descartaba el canal alfa**. La
capa de 32 bits lo lleva de verdad -- volcada mientras el reproductor está en
pantalla, su byte de alfa toma **243 valores distintos**, que ninguna lectura de
"ignóralo" sobrevive.

`DISP_ALPHA=1` compone de delante a atrás con *source-over*. Eso explica **las dos**
cosas a la vez: el reproductor queda encima de la carátula, y el barrido del menú de
idioma -- que fue de donde se dedujo el orden Z original, suponiendo capas opacas --
sigue funcionando porque el panel entrante es transparente allí donde debe verse el
saliente. Comprobado: el menú de idioma y las listas se dibujan igual que antes.

```
Now Playing                    [>] [bateria cargando]
+-----------+   Billie Jean
|  caratula |   Michael Jackson
|           |   Greatest Hits
+-----------+   1 of 4
 0:00  [============================]  -4:55
```

### El anillo era demasiado grande

Con `SM1_DECODE` la interfaz se congelaba a medio entrar mientras el audio seguía
sonando. Causa: publicamos 7 ranuras, o sea 28 KiB desde `0x22038100`, y **escribir
PCM en todo ese rango pisa memoria que no es nuestra**. Publicar el estado es
inofensivo; escribir 28 KiB no lo es. `SM1_NSLOTS` permite barrerlo, y con **2
ranuras** la interfaz aguanta y el audio sigue saliendo.

**Resultado, todo a la vez y medido:** pantalla completa del reproductor con nombre de
canción, artista, álbum, pista N de M y barra de progreso; audio 12.89% de muestras no
nulas con pico 32768 y el atajo del anfitrión APAGADO; y el reproductor no se cierra.

### Lo que falta

El tiempo transcurrido se queda en **0:00**. Hacer que el registro de posición del SM1
siga al I2S (como ya hace `sm1_stream_pos`) **no** lo mueve, así que ese contador no
sale de ahí: lo lleva la contabilidad interna del reproductor. Es lo único que queda
para que la barra avance.
