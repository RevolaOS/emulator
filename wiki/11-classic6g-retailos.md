# El retailOS del iPod classic 6G arrancando en QEMU

Qué hace el `osos` de Apple cuando se le deja correr en la máquina
`iPod-Classic`, en qué orden pide las cosas, y dónde se para hoy.

Todo lo que sigue está **medido** salvo donde diga `INFERIDO` (razonado a
partir de algo verificado) o `INDETERMINADO` (no lo sabemos y no fingimos
saberlo). Las trazas son reproducibles con las órdenes del final.

> Este documento es **qué hace el firmware y qué se sabe de cada bloque**, y
> está ordenado por bloque. Si lo que buscas es **cómo se averiguó, en qué orden
> y qué se creyó por error por el camino**, eso está en
> [12-historia-6g.md](12-historia-6g.md), que además lleva el catálogo de
> hipótesis retiradas y el capítulo de las tres veces que un descarte resultó
> ser la pista buena.

---

## 0. Estado

| Hito | Estado |
|---|---|
| 1. Se carga y ejecuta la primera instrucción | **hecho** |
| 2. Pasa la inicialización temprana sin colgarse | **hecho** |
| 3. Inicializa periféricos y llega a su bucle principal | **hecho** — kernel RTXC arrancado, planificador vivo, tarea idle en `0x22003b1c` |
| 4. Dibuja algo en pantalla | **hecho, con disco** — la pantalla de selección de idioma, 75 463 de 76 800 píxeles. Ver §13 |
| 5. Se puede **navegar** | **hecho** — rueda, SELECT y **MENU**: menú principal, entrar en Música, y volver con MENU al fotograma exacto del que se salió (§18) |

> **10 de agosto de 2026, aún más tarde — y ahora MENU vuelve.** Los dos
> síntomas que quedaban —MENU no volvía atrás y el panel derecho no seguía al
> resaltado— eran **el mismo fallo, y era nuestro**: el compositor dibujaba
> capas que el huésped había apagado, y desde el primer cambio de pantalla una
> capa muerta tapaba la interfaz entera. La puerta son **los bits 4..6 del
> registro `CONTROL` del compositor**. Con ella puesta: menú principal en el
> idioma elegido, la rueda mueve el resaltado **y el panel derecho lo sigue**,
> SELECT entra en Música y **MENU vuelve, con 0 píxeles de diferencia respecto
> al fotograma del que se salió** (§18).

> **10 de agosto de 2026, más tarde — y ahora se puede usar.** Con la rueda de
> clic modelada (§17), `e` mueve la barra de selección de **English** a
> **日本語**, `q` la devuelve, y Enter **cambia de pantalla**: aparece el icono
> de la nota musical con el texto **「ミュージックが…」**, es decir, en el idioma
> que estaba resaltado. Cada giro cambia **8 000 píxeles** sobre un suelo de
> ruido de **0**. Y hubo que arreglar una segunda cosa que no era la rueda: el
> **interruptor de bloqueo** del PMU, que tiraba en silencio todos los botones
> (§17.4).

> **10 de agosto de 2026 — hay imagen.** Con un disco construido por
> `mkdisk6g4k.py` a partir del `.ipsw`, el retailOS pinta **su pantalla de
> selección de idioma**: lista blanca a la izquierda con nueve escrituras
> distintas (incluidos 日本語 y Ελληνικά), barra de selección azul, batería, y
> el degradado con «Welcome» a la derecha. `screendump` + `pixcount.py`:
> **75 463 píxeles encendidos de 76 800, 404 colores**. Y **se ha mirado la
> imagen**, no sólo el número: es la interfaz de Apple, no basura (§13).
>
> **Sin disco sigue dando 0 píxeles**, y eso es del huésped, no del modelo: la
> interfaz se muere buscando tipografías antes de componer nada (§4).

Hicieron falta tres cosas a la vez, y las tres estaban en la lista de
sospechosos del §6:

1. **`0x38900000` modelado** — es el compositor de vídeo (§13).
2. **DMAC0/DMAC1 como PL080 de verdad** — es lo que destraba el arranque con
   disco (§14). Y corrige el §9: el ATA nunca fue el culpable.
3. Con esas dos, el volumen `rsrc` se monta, aparecen las fuentes y la interfaz
   compone. Medido con `-d in_asm`, y son las mismas direcciones que llevaban
   meses dando cero.

Con `-drive if=ide` el arranque es limpio: cero excepciones, cero reinicios.
Sin disco entra en un ciclo de caída y reinicio (§4).

Reproducido en auditoría independiente (agosto de 2026), con estas cifras:
con disco, 35 s de `-d int` dan 27 166 `Taking exception 5 [IRQ]` y **nada
más**; `PC = 0x22003b20`, `SP = 0x22010b70`, modo SVC; `0x22003b1c` es
`mcr p15,0,r0,c7,c0,4` y `0x22003b30` es `b 0x22003b0c`, comprobado en el
desensamblado. Sin disco, 25 s dan 23 *Data Abort*, 23 llamadas de semihosting
y 23 `SWRCON`. El DMA de ATA entrega de verdad: con una imagen que lleva
`55 aa` en `0x1FE`, `pmemsave 0x0be5d3e0` la encuentra ahí después del
arranque.

Y la advertencia que iba con el hito 3, que era cierta hasta el 10 de agosto de
2026: **«llegar al bucle idle» no es «arrancar»**. Un sistema quieto no es
necesariamente un sistema que funciona. Con disco eso **ya no aplica** (§13,
§14): la tarea de interfaz corre y compone. Sin disco sigue aplicando tal cual.

> **Nota de lectura, por una tercera auditoría (10 de agosto de 2026).** Los
> §3, §6 y §9 se escribieron cuando el arranque con disco se paraba, y describen
> ese estado. **No los leas como el estado de hoy**: los §13 y §14 los corrigen,
> y donde se contradicen manda lo de más abajo, que es lo medido después. Las
> partes de §6 y §9 que hablan del arranque **sin** disco siguen siendo válidas.

---

## 1. La imagen tiene dos regiones de enlace

`osos_decrypted.bin` no es un binario plano. Es una imagen con carga dispersa
(*scatter-load*) de ARM ADS/RVCT con **dos** bases:

```
fichero [0x00000, 0x0AE48)  ->  0x22000000   stub + núcleo de arranque (IRAM0)
fichero [0x0AE48, fin)      ->  0x08000000   el cuerpo del SO (DRAM)
```

En la práctica: `direccion = 0x08000000 + (offset_fichero - 0xAE48)` para todo
lo que esté a partir de `0x0AE48`. Desensamblar la imagen entera con una sola
base apunta a la instrucción equivocada en el 99 % del fichero.

Para desensamblar:

```bash
# la parte de IRAM
arm-none-eabi-objdump -D -b binary -m arm --adjust-vma=0x22000000 \
    --start-address=0x22003b0c --stop-address=0x22003b40 osos_decrypted.bin
# el cuerpo en DRAM
arm-none-eabi-objdump -D -b binary -m arm --adjust-vma=0x07FF51B8 \
    --start-address=0x08327760 --stop-address=0x08327780 osos_decrypted.bin
```

`0x07FF51B8` no es un número mágico: es `0x08000000 - 0xAE48`.

---

## 2. El bloque sin nombre en `0x3C100000`

**Ninguna fuente que tengamos lo documenta.** El único `0x3C100000` de
`s5l87xx.h` de Rockbox es `LCD_BASE`, y está bajo
`#if CONFIG_CPU==S5L8700` — otro chip. El modelo de freemyipod tampoco tiene
nada ahí. La única autoridad es el código de Apple.

Y es pequeño: buscando la palabra literal `0x3C100000` en la imagen y después
todos los `LDR` relativos a PC que resuelven a una de esas palabras salen
**exactamente tres sitios**, así que la lista es completa, no "lo que
apareció":

| Sitio | Qué hace |
|---|---|
| `0x08031d9c` | `str r0,[r1,#8]` — semilla: siete bytes de estado + `TECNT` + la palabra de `0x3C700008` |
| `0x08327738` | `str #0,[r1]` — escribe **cero** en `+0` |
| `0x08327760` | `while (!([+0] & 7)) ; return [+4]` |

> **Corrección (auditoría).** El tercer sumando **no** es «la semilla
> anterior», como decía la primera versión de esta página. El literal de
> `0x08031de0` es `0x3C700000`, o sea `TIMER_BASE`, y `+0x08` es `TADATA0`.
> La semilla se mezcla con **dos** lecturas de temporizador independientes, y
> nadie vuelve a leer nunca `+4` ni `+8` de este bloque: el registro de
> semilla es de sólo escritura desde el punto de vista del huésped.

En contexto: `0x08327714` toma una sección crítica y escribe ese cero;
`0x08031d30` siembra `+8`, saca veinte palabras por `0x08327760`
descartando diecinueve, y suelta la sección en `0x08327750`. Sembrar un
generador y tirar sus primeras salidas es un idiotismo de RNG, y la semilla se
mezcla con un contador libre (`0x08015748` es un *veneer* a `0x22001EC4`, que
es "devuelve `TECNT`"). Nada más encaja: entre las veinte lecturas no entra
ningún dato nuevo, así que un checksum o un hash devolvería la misma palabra
veinte veces y el bucle no tendría sentido.

- Que sea un **RNG**: `INFERIDO`.
- El contrato de tres registros: **verificado**.
- Cómo se llama y qué más tiene: `INDETERMINADO`.

### La trampa: no hay bit de habilitación

Ninguna instrucción escribe un valor distinto de cero en `+0`. La única
escritura es el cero de `0x08327738`, y ocurre **antes** de las veinte
lecturas, no después. Un modelo que condicione "hay dato listo" a que el
huésped haya habilitado el bloque se cuelga en la primera extracción, porque
`0x08327760` gira sin *timeout* y sin salida. Los bits de listo tienen que
subir solos.

Es el patrón 1 de [09-historia-5g.md](09-historia-5g.md) — *el bit de estado
que nunca llega* — con un agravante: aquí el bit parecía tener un habilitador
y no lo tiene.

### Cuánto pesa de verdad este bloque (medido en la auditoría)

**Con disco enganchado, el retailOS no toca `0x3C100000` ni una vez**:
`0x08327760` no aparece en un log de `-d in_asm` de ese arranque. El bloque
sólo se alcanza en el camino **sin** disco, una vez por intento de arranque, y
el sitio de siembra `0x08031d9c` no se ejecuta en ninguno de los dos — así que
el estado del generador arranca siempre de cero.

O sea: quitar el falso bit de habilitación desbloqueó de verdad un giro
infinito **en el estado del árbol de aquel momento**, pero el bloque no está
en el camino que hoy lleva al bucle idle. No se le puede acreditar el hito 3.

---

## 3. La interfaz de LCD en `0x38300000`

Es un puerto de órdenes tipo MPU/8080, **no** un motor de barrido: no hay
dirección de framebuffer ni temporización de vídeo. El panel guarda su propia
imagen. El bloque de `0x39200000` (`LCDBASE`, "CLCD CONTROLLER" en Rockbox) sí
tiene direcciones de framebuffer, y el retailOS apenas lo toca.

Los bits de estado (`LCD_STATUS`, `+0x1C`) son lo que importa, porque cada uno
es un giro sin *timeout* por los dos lados. **Rockbox y el retailOS coinciden
en los tres**, que es lo que los hace fiables:

| Bit | Significado | Rockbox | retailOS |
|---|---|---|---|
| 0 | hay dato de lectura en `LCD_DBUFF` | `lcd-s5l8702.c:241` | — |
| 1 | interfaz ociosa, se puede escribir control | `lcd-s5l8702.c:207` | `0x080b6f98`, `0x080b6ffc` |
| 4 | FIFO de escritura ocupada | `lcd-s5l8702.c:215` | `0x080a90b4` |

`s5l87xx.h` anota el valor de reset como `0x0106`: bits 1, 2 y 8 puestos y el 4
limpio — ocioso y no ocupado, coherente con lo anterior. Qué significan los
bits 2 y 8 es `INDETERMINADO`; ningún driver los mira.

Con la ventana sin implementar, los `while (!(LCD_STATUS & 0x2))` no vuelven
nunca. Es el muro que se comió 48 millones de lecturas en una traza de 45 s.

**El bloque es más ancho de lo que Rockbox sabe.** El retailOS escribe
`0x3830007C = 0x804` y `0x38300088 = 0x1000000` en su primerísima configuración
de LCD, antes de tocar nada que Rockbox nombre. Los dos son `INDETERMINADO`,
pero la ventana tiene que cubrirlos o se caen al suelo de E/S de la máquina.

### Por dónde pasan los píxeles: **cero llegan**, y ahora está contado

Corrección a las dos versiones anteriores de esta sección. Con el panel
modelado y `LCDTRACE=1` contando cada acceso, un arranque sin disco de 25 s da:

```
LCDTRACE totals: 63 writes to the command port +0x04, 168 to the data port
                 +0x40..0x5C, of which 0 were pixels; 21 frame-engine starts
```

Las 168 escrituras al puerto de datos son **21 arranques × 8 parámetros**: los
cuatro bytes de `0x2A` (`00 00 01 3f`) y los cuatro de `0x2B` (`00 00 00 ef`).
**Ni una sola palabra de píxel.** La frase «llega una palabra de píxel por
escritura de CPU» era ese byte `0x3f`, y la conclusión «luego el framebuffer va
por DMA» no se sostiene por otro camino: en un arranque sin disco entero,
**ningún canal de DMAC0 apunta jamás a `LCD_WDATA`** (`-d unimp`: el único
destino programado es `0x3CA00010`, el FIFO de I²S, y el LLI que se escribe es
`0x22010020`, también de audio).

Lo que sí hace el retailOS es arrancar un **motor de trama** dentro del propio
bloque de LCD, cuyos registros no conoce ninguna fuente externa:

| Registro | Qué se le escribe | Sitio |
|---|---|---|
| `+0x70` | `1` — arrancar | `0x08127f94` |
| `+0x74` | `(320 << 16) \| 240` | `0x08127f8c` |
| `+0x78` | `0x0005000a` `INDETERMINADO` | `0x08127f3c` |
| `+0x7C` | `0x00000804` `INDETERMINADO` | `0x080afb1c` |
| `+0x80` | `1` … `0` alrededor de fijar la ventana | `0x0808b000`/`0x0808b030` |
| `+0x88` | `0x01000000` `INDETERMINADO` | `0x080afb0c` |
| `+0x8C` | `while (LCD[0x8c] & 3);` — bits [1:0] = transferencia en curso, y es un giro **sin timeout** por partida doble | `0x080a4090`, `0x080b0128` |

**De dónde lee ese motor es `INDETERMINADO`**: no hay ningún registro de
dirección de origen en los 256 bytes de la ventana, y no hay DMA apuntándole.
El modelo lo acepta, lo cuenta y declara `+0x8C` libre, que es lo que ya hacía
la ventana sin implementar; la diferencia es que ahora lo dice en voz alta.

Dos cosas están verificadas y acotan la búsqueda:

- **La ventana de 256 bytes basta.** Con el LCD modelado en `0x38300000..0xFF` y
  el resto del bloque cayendo al *io floor* de la máquina, un arranque sin disco
  de 25 s no produce **ni un solo** acceso a `0x383000xx` por encima de `0xFF`
  (`-d unimp`, cero coincidencias de `addr 0x383`). Así que el registro de
  origen no está en el bloque de LCD.
- **Tampoco es DMAC0.** Ningún valor `0x38300040` aparece nunca en el log de
  `-d unimp`; los únicos destinos que DMAC0 recibe son `0x3ca00010` (FIFO de
  I²S) y descriptores en `0x22008c5c`/`0x22010020`. Rockbox **sí** empuja el
  cuadro por DMAC0 (`lcd-s5l8702.c:172`, `dstperi = S5L8702_DMAC0_PERI_LCD_WR`);
  Apple no. Apple usa el motor de `+0x70`, y ésa es toda la diferencia.

### La secuencia completa de "pinta un cuadro", desensamblada

`0x08127f14` es la función entera, y explica por qué salen cero píxeles sin que
nada falle:

```asm
08127f14: push {r3,r4,r5,lr}
08127f1c: bl 0x8015710          ; (0x2080, 0, 1)
08127f30: bl 0x8127268
08127f3c: str r0,[r4,#0x78]     ; LCD[0x78] = 0x0005000a
08127f54: bl 0x80bcef8          ; (64,64,0,0,0) -> escribe 0x389003AC
08127f58: ldr r0,[0x089115e4]   ; el registro de driver de panel
08127f64: cmp r0,#0 ; beq +     ; <- comprobación de nulo
08127f84: blx [r0+0x0c]         ; (0,0,0,320,240)   <- EL DIBUJADO
08127f8c: str r0,[r4,#0x74]     ; LCD[0x74] = 0x014000f0
08127f94: str r0,[r4,#0x70]     ; LCD[0x70] = 1     <- "presenta"
```

Ese puntero **está a cero en la imagen** (`0x089115e4` vale `0x00000000` en el
fichero), así que lo pone alguien en tiempo de ejecución, y la comprobación de
nulo de `0x08127f64` es real. Tres medidas dicen que pasa:

- Con disco, a los 45 s, `xp /1wx 0x089115e4` devuelve **`0x08911fe0`**, uno de
  los tres registros de driver de panel. `[0x08911fe0 + 0x0c] = 0x080c1164`.
- Sin disco, `-d in_asm` de 25 s: `0x08127f14` entra **22 veces** y
  `0x080c1164` entra **22 veces** — una por intento de arranque. La rama nula
  no se toma nunca.
- Y `0x080c1164` es justo la función que la cabecera de `ipod_classic_lcd.c`
  identifica como el `displaylcd_setup()` de Rockbox.

Es decir: **el retailOS sí llama a "dibuja la pantalla entera" en cada arranque,
y lo único que esa llamada emite es `0x2A`, `0x2B`, `0x2C` y ocho bytes de
parámetro.** Igual que en Rockbox: `displaylcd_setup()` sólo fija la ventana. Lo
que en Rockbox viene después es `displaylcd_dma()`; lo que en Apple viene
después es `LCD[0x70] = 1`. Los píxeles nunca pasan por la CPU en ninguno de los
dos casos.

### El candidato serio para el origen: `0x38900000`

> **RESUELTO, 10 de agosto de 2026 — ver §13.** Era. Es el compositor: cinco
> descriptores de capa con dirección de origen, stride, formato, tamaño y
> posición, en `+0x058 + k*0x18`. La sospecha de que `+0x208` fuese el puntero
> de origen era **falsa** (vale 0 siempre, escrito por `0x08127428`); las
> direcciones de origen están en `+0x060/78/90/a8/c0` y `+0x038..+0x044`. Lo que
> sí acertó de lleno fue el método: contar valores y no sólo accesos.

Una auditoría posterior retiró el «descartado» que llevaba este bloque. La
prueba que lo descartaba era «ningún registro recibe un valor con pinta de
dirección de DRAM», que es cierta pero no dice lo que parecía decir. Lo que hay
ahí, medido en un arranque sin disco de 25 s con `-d unimp`, son **793
direcciones distintas escritas**, y dos de ellas son inconfundibles:

| Registro | Valor | Qué es |
|---|---|---|
| `+0x400` … `+0xFFC` | `valor == offset - 0x400`, o sea `0,4,8,…,0x3FC` | **768 palabras = 3 × 256: una tabla de gamma/paleta identidad.** Sólo un camino de vídeo escribe eso |
| `+0x214` | `0x013f00ef` | **exactamente la geometría del panel**, `(319<<16)|239` |
| `+0x208` | `0` | escrito justo después de la geometría — la forma de un puntero de origen que está a cero |
| `+0x3AC` | campo de bits de `0x080bcef8(64,64,0,0,0)` | configuración |
| `+0x0d4..0xEC` | `1`, `0x1000` | |
| `+0x0E0` | `0x50004000` (desde `0x081459d4`) | `INDETERMINADO` |

Y el código que lo maneja es **el mismo** que maneja el motor de trama del LCD:
de los dieciocho literales `0x38900000` de la imagen, catorce están en
`0x08127xxx`, la misma función que escribe `LCD[0x74]` y `LCD[0x70]`.

- Que `0x38900000` sea el bloque de composición/salida de vídeo del que el motor
  de `+0x70` toma el cuadro: **`INFERIDO`**, con las dos medidas de arriba a
  favor y ninguna en contra.
- Que `+0x208` sea la dirección de origen: **`INFERIDO`**, sin confirmar.
- El siguiente experimento barato: mapear `0x38900000` como un dispositivo de
  verdad (hoy cae al *io floor*), registrar quién escribe `+0x208` y con qué, y
  ver si en algún arranque llega a valer algo distinto de cero.

### El panel, que sí está modelado

`ipod_classic_lcd.c` lleva una memoria de 320×240 en RGB565, ventana con
`0x2A`/`0x2B`, puntero de escritura tras `0x2C`, y un `qemu_console`. Todos los
números vienen de una medida, ninguno de un datasheet:

- **Juego de órdenes de 8 bits**: el huésped escribe `LCD_CON = 0x80000c21`, y
  `0x080b6f8c` sólo produce ese valor por la rama `lcd_get_type() ∈ {0,1}`, que
  es `LCD_CMDSET_8BIT` de Rockbox (`lcd-6g.c:201-214`).
- **320×240**: los tres registros de driver de panel entre los que elige el
  firmware (`0x08911f90`, `0x08911fb8`, `0x08911fe0`) empiezan los tres por
  `0x00f00140` = `(240<<16)|320`; la ventana que fija es `0..0x13f` × `0..0xef`;
  y `+0x74` recibe `(320<<16)|240`.
- **RGB565**: el modo de trama es `LCD_MODE_P16`.

Validación con caso de control, porque un panel que no se prueba no es una
medida: `tools/ipod_classic/lcdtest.S` emite exactamente esa secuencia y empuja
76 800 palabras. `screendump` + `tools/ipod_classic/pixcount.py` devuelven
**76 800 píxeles encendidos y 1920 colores distintos**, que es al píxel lo que
el test escribe. Si la aritmética de ventana o el formato estuvieran mal, ese
número saldría mal de forma evidente.

```bash
arm-none-eabi-as -o /tmp/l.o tools/ipod_classic/lcdtest.S
arm-none-eabi-objcopy -O binary /tmp/l.o /tmp/l.bin
qemu-system-arm -M iPod-Classic -kernel /tmp/l.bin -display none -monitor stdio
(qemu) screendump /tmp/shot.ppm
python3 tools/ipod_classic/pixcount.py /tmp/shot.ppm
```

Cuando el retailOS dibuja (ver §4, ocurre en el camino de error) emite
`0x2A`, `0x2B`, `0x2C` — *column address set*, *page address set*, *memory
write* — que es el juego de órdenes de un controlador tipo ILI9341. Cuál está
puesto de verdad se elige en tiempo de ejecución con el *strap* de GPIO
`PDAT(6)` bits [5:4], que no modelamos.

---

## 4. El ciclo de caída sin disco, y por qué era invisible

Sin `-drive`, la máquina no se quedaba ociosa: **se caía y reiniciaba**, unas
diecinueve veces cada veinte segundos. Con `-d int`: 19 *Data Abort*, 19
llamadas de semihosting y 19 escrituras de `SWRCON`.

No se veía porque el diagnóstico salía por **stderr** y las trazas lo tiraban.
El firmware lo estaba diciendo en voz alta todo el tiempo:

```
CPU_exception_handler
qemu-system-arm: warning: S5L8702: guest asked for a software reset (SWRCON = 0xaa5), next image "--------"
```

> Lección, y es la número 1 de [09-historia-5g.md](09-historia-5g.md): si el
> firmware imprime algo, escúchalo. El semihosting ya estaba encendido por
> defecto en esta máquina y aun así estuvimos un rato mirando histogramas de
> `-d unimp` en vez de leer las dos líneas que explicaban todo.

### La causa

```
...with DFSR 0x5 DFAR 0xea00002a
```

`0xEA00002A` no es una dirección: es una instrucción de salto ARM. Y es
`0xEA000002 + 0x28`, donde `0xEA000002` es exactamente la palabra que `main`
escribe en `[0x22000000]` (paso 3 de su inicialización).

La instrucción que aborta es un par de saltos de C++:

```asm
0815b230:  ldr r0,[sp]        ; puntero al objeto
0815b234:  bic r0,r0,#1
0815b238:  ldr r1,[r0]        ; puntero a la vtable
0815b23c:  ldr r1,[r1,#0x28]  ; <-- aborta
0815b240:  blx r1
```

Es una **llamada a método virtual sobre un objeto nulo**. Y aquí está lo
interesante: como el propio firmware mapea VA 0 sobre IRAM0, la desreferencia
nula **no falla** — lee tranquilamente una palabra de la tabla de vectores — y
sólo revienta la segunda. Un puntero nulo en esta máquina no da un fallo
limpio: da un valor plausible y una caída doscientos bytes más tarde.

**Corrección de agosto de 2026, y es una corrección de fondo: el nulo NO viene
de la búsqueda de recursos.** Medido con `gdbpoke.py` (§10), parando en cada
instrucción de esa función:

| Parada | Qué se lee |
|---|---|
| `0x0815b1e0` | `r0 = 0x086aef48` — la búsqueda de `"FONT"` **encuentra** el recurso |
| `0x0815b1f4` | `r0 = 0x0874a444` — la de `"Str "` **también** |
| `0x0815b20c` | `r5 = 0x0be6770c` — no nulo, no salta a la salida |
| `0x0815b230` | `[sp] = {0, 0}` — **aquí** aparece el nulo |
| `0x0815b23c` | `r1 = 0xea000002`, la palabra de VA 0; la siguiente carga aborta |

Y el registro `FONT` que devuelve, leído del huésped, es
`0dad0001 0000003a 01d58101 0dad0002`: id del nombre, cuerpo 58, estilo.

O sea que la cadena de proveedores funciona y el recurso existe. Lo que falla es
la construcción del objeto de texto, y la razón es concreta:

```
0x0815b22c  bl 0x08239658   "hazme una cadena con esta fuente"
0x08239658  -> 0x08238fa0 -> 0x082393ec
0x082393ec  busca la fuente por nombre, cuerpo y estilo en DOS listas:
              [0x08912cd8 + 8]  fuentes ya instanciadas
              [0x08912cd8 + 4]  ficheros de fuente disponibles
            si ninguna casa, devuelve {0,0}
las dos listas las llena una sola función, 0x08239220, y hace esto:
  0x080518a4(1, &vol, &path)   ->  vol = 4, path = "Resources/Fonts"  (0x08380628)
  0x08239134(vol, path)        ->  recorre ESE DIRECTORIO y registra cada fichero
```

`0x08239134` es un iterador de directorio de verdad — salta las entradas que
empiezan por `.` (compara con 46) y llama a `0x08107184` por cada nombre.

**Conclusión, VERIFICADA de punta a punta: el retailOS no lleva ninguna fuente
dentro de la imagen. Todas viven en `\Resources\Fonts\` de la partición `rsrc`
del disco** (`Helvetica.ttf`, `HelveticaBold.ttf`, `CJK11.ttf`, `CJK16.ttf`,
cuatro `MonoHope-*.sbit`). Sin volumen montado la lista está vacía, no casa
ninguna fuente, y el llamador llama a un método virtual sobre el `{0,0}`.

Lo confirma un experimento no destructivo: con `gdbpoke.py --skip
0x0815b230=0x0815b270:0` se salta esa llamada sin tocar la imagen de Apple, y el
arranque **vuelve a caer en el mismo ciclo**, ahora con `DFAR 0xEA00003E` en vez
de `0xEA00002A` — otro método virtual, ranura `+0x3C`, sobre otro nulo del mismo
origen. No es un fallo que se pueda esquivar: sin fuentes, todo el grafo de
objetos de la interfaz sale nulo.

**Con un disco enganchado, el ciclo entero desaparece** — pero por la razón mala:
la interfaz ni siquiera llega a intentarlo (§9).

---

## 5. El puente ATA de `0x38700000`

No es un *task file*: los ocho registros de dispositivo están en `0x54..0x74`
pero no se leen directamente. Leer uno arranca un ciclo de bus cuyo resultado
aparece después en `ATA_PIO_RDATA`. Rockbox lo escribe entero
(`storage_ata-6g.c`) y el driver de Apple es la misma función compilada de otra
fuente (`0x08096824`).

### El DMA

Especificado dos veces, y las dos coinciden: Rockbox en
`storage_ata-6g.c:851-880`, y el driver de Apple trazado registro a registro.

```
<orden ATA 0xc8 a ATA_PIO_CSD>
ATA_TBUF_START = buffer        (ATA_SBUF_START al escribir)
ATA_TBUF_SIZE  = bytes
ATA_XFR_NUM    = bytes - 1
ATA_CFG |= flags ; ATA_CFG &= ~(BIT(7)|BIT(8))
ATA_IRQ = BITRANGE(0,4) ; ATA_IRQ_MASK = BIT(0)
ATA_COMMAND = BIT(0)           <- y sólo ahora se mueve
<espera la interrupción>
ATA_COMMAND = BIT(1)           <- fin
```

`ATA_CFG` bit 4 elige el sentido: puesto = búfer de origen = escritura a disco.

**El orden es lo que da forma al modelo, y es el contrario del intuitivo.** La
dirección del búfer se programa *después* de la orden ATA, así que `start_dma`
no puede saber dónde va el dato: sólo puede quedarse con el *callback* que le
da el núcleo IDE, y es la escritura del bit de arranque la que lo dispara.

Dos trampas que cuestan una tarde cada una:

- `s->bus.dma` hay que reclamarlo **después** de `ide_bus_init_output_irq`, no
  antes: lo último que hace esa función es poner `bus->dma = &ide_dma_nop`.
  Reclamarlo antes hace que toda orden de DMA se acepte y se ignore en
  silencio, que se parece muchísimo a un disco que no contesta.
- Comprobar el DMA por la ausencia de quejas no vale. Se comprueba leyendo la
  memoria del huésped: tras la primera transferencia, `0x0be5d3e0 + 0x1fe`
  contiene `55 aa`, la firma del MBR del sector 0 de la imagen, en el byte que
  le toca.

---

## 6. Dónde se paraba con disco (histórico, **superado** por §13 y §14)

> **Esta sección describe el estado anterior al 10 de agosto de 2026.** Con
> disco ya no se para aquí: la tabla «Lo que NO se ejecuta» de más abajo es
> exactamente la que el §14 vuelve a medir y sale toda a **1**. Se conserva
> porque el método (medir entradas de función, no direcciones de mitad de
> función) es lo que hizo falta para llegar al §14. **Sin disco** todo lo de
> esta sección sigue siendo válido.

Con disco, el sistema llegaba a su **bucle idle** y se quedaba:

```
22003b1c:  mcr p15,0,r0,c7,c0,4    ; wait for interrupt (ARM926)
22003b30:  b   0x22003b0c
```

`PC = 0x22003b20`, modo SVC, `SP = 0x22010b70` (la pila SVC inicial). El kernel
RTXC está arrancado, el planificador corre, las interrupciones de timer entran,
y **todas las tareas de aplicación están bloqueadas**.

Lo que sí hace antes de dormirse:

- MIU, reloj, PMU por I²C, GPIO, click wheel, controlador de interrupciones
  externas, y el ATA.
- Dos lecturas por DMA, las dos en **LBA 0**: ocho sectores y luego sesenta y
  cuatro. Los bytes llegan (verificado, §5). Después, nada más.
- Sondea `WHEELINT` (`0x3C200014`) unas **120 veces por segundo** — medido en
  la auditoría con dos carreras de 20 s y 50 s: 3071 y 6770 lecturas, o sea
  123/s en régimen. (La primera versión de esta página decía «tres veces por
  segundo»; era falso por un factor de 40.) Sigue sin ser un giro bloqueante —
  un giro son millones — y sigue demostrando lo mismo: **hay tareas corriendo
  además de la idle**, que es la mejor prueba de que el planificador vive.

### Lo que NO se ejecuta, y esto es el dato importante

| Dirección | Qué es | ¿Se ejecuta? |
|---|---|---|
| `0x0805712c` | el parser de la partición de firmware: en `0x08057198` compara `]ih[` (`0x5B68695D`, verificado en `0x080572d4`) con el offset `0x100` de lo que leyó | **no, nunca** |
| `0x0815b1b8` | el código de UI que dibuja texto (el que caía sin disco) | **no** |
| `0x08239658` | la construcción del objeto de cadena | **no** |
| `0x08236130` | la búsqueda de recursos | sí, una vez |

> **Corrección (auditoría).** La primera versión de esta tabla medía el parser
> por `grep -c '^0x08057160'`, y **esa medida no valía**: `0x08057160` es
> `add r0,sp,#4` en mitad de una función, no una entrada, así que un log de
> `-d in_asm` habría dado cero aunque el código corriera en cada arranque (ver
> §8). La entrada real de la función es `0x0805712c` — `push {r1..fp,lr}` —, y
> medida por ahí la conclusión **se mantiene**: cero, con disco y sin él. Las
> otras tres direcciones sí son entradas de función (`push` como primera
> instrucción), así que ésas siempre fueron válidas.

O sea: **el bloqueo está aguas arriba tanto del parseo de la partición de
firmware como del dibujado**, y no es el almacenamiento, porque el
almacenamiento funciona.

### Hipótesis descartadas con medida

| Hipótesis | Descartada porque |
|---|---|
| "Le falta un MBR válido" | Con MBR y dos particiones (tipo 0 + FAT32) hace exactamente las mismas dos lecturas |
| "La partición de firmware tiene que estar en LBA 0" | Con la imagen `Firmware-24.9.1.2` cruda en el sector 0, y `]ih[` verificado en `0x100`, las mismas dos lecturas |
| "El disco es demasiado pequeño" | Con 64 MB, 289 MB, 512 MB y 80 GB: las mismas dos lecturas |
| "Está bloqueado en el DMA de ATA" | Lo estuvo, y ya no: las transferencias terminan y el huésped acusa la interrupción |
| "Está bloqueado en el LCD" | Lo estuvo, y ya no; ahora, **con disco**, toca el bloque exactamente cinco veces y para (`LCDTRACE=1`: `+0x00 = 0x80000000`, `+0x00 = 0x80100db1`, `+0x88`, `+0x20`, `+0x7c`, todas desde `0x080afaf4`) — no dos, como decía la versión anterior de esta línea |
| "El click wheel lo tiene parado" | Lo sondea ~123 veces/s desde una tarea periódica (medido: 3071 lecturas en 20 s, 6770 en 50 s); no es un giro bloqueante |

### Lo siguiente que hay que averiguar

Qué está esperando la tarea bloqueada. La vía más prometedora no es adivinar
periféricos, es preguntárselo al kernel: la imagen trae **RTXCbug** compilado,
el depurador interactivo de RTXC, con su REPL (`"RTXCbug> "`, `"Bad command"`,
`"G - Go to Multitasking Mode"`, `"** Task Register Snapshot **"`) y un
retarget completo de semihosting, incluido `SYS_READC`. Si se consigue entrar,
dice directamente qué tareas hay y en qué están bloqueadas.

Candidatos por descartar, en orden de coste. **Los números 1 y 4 eran los
buenos, y hacían falta los dos: ver §13 y §14.**

1. ~~El bloque `0x38900000`~~ **RESUELTO (§13): es el compositor.** Lo que la
   máquina cubría con el *io floor* — leer cero y tirar las escrituras — no era
   neutral: seis funciones hacen lectura‑modificación‑escritura sobre `+0x008`,
   así que cada una borraba lo que había puesto la anterior.
2. GPIO de verdad: hoy devuelve cero a todo. `PDAT(11)` bit 1 = 0 elige ATA
   paralelo, que es lo que queremos, pero `PDAT(6)` (tipo de panel) y
   `PDAT(10)` (auriculares) también leen cero y puede que no deban.
3. ~~El EIC (`0x39A00000`), que se programa y nunca dispara.~~ Modelled in
   `ipod_classic_eic.c`: the standby path waits on `ALVCON` and wakes through it.
4. ~~DMAC0 (PL080), que se programa con descriptores en DRAM y nunca completa.~~
   **RESUELTO (§14), y era la causa del bloqueo con disco.** Esta línea estaba
   escrita desde el principio y en el último lugar de la lista; el coste de
   arreglarlo resultó ser el más bajo de los cuatro, porque QEMU ya trae
   `hw/dma/pl080.c`.

---

## 9. Con disco: dónde se paraba la interfaz, instrucción a instrucción (histórico)

> **Superado por el §14, que corrige el diagnóstico final.** La cadena de
> llamadas de abajo está bien medida y sigue siendo la buena; lo que estaba mal
> era la última línea, «el agujero está entre *el DMA terminó* y *despierta a
> quien lo pidió*» leída como un problema del **ATA**. No lo era: quien no
> volvía era el `memcpy` por **DMAC0** que va dentro de esa misma llamada de
> lectura. Ver §14. Hoy, con `E.img`, la cadena entera se recorre y
> `0x081228d0` **sí** se alcanza.

Esto responde a «qué está esperando la tarea bloqueada» del §6, que
llevaba abierto desde el principio. Todo medido con `gdbpoke.py` y un disco de
64 MB a ceros; con `A.img` (MBR válido, partición de firmware real y FAT32 con
`iPod_Control`) sale exactamente igual.

La cadena, de fuera adentro. Cada eslabón es una parada real del depurador:

```
0x0811c008   crea la tarea RTXC "MP3ExampleTask"
0x0811bf68     cuerpo de la tarea de interfaz          <- llega
0x0811bfb8       blx [obj+0x204] -> 0x081a3134         <- entra y NO vuelve
0x081a3134         blx [obj+0x208] -> 0x081a3360       <- entra y NO vuelve
0x081a3360           bucle sobre 9 fábricas de subsistema; se para en la nº 8
0x0824caa8             new(0x124) + constructor 0x0824cc8c
0x0824cc8c               bl 0x08035bf8                 <- lector de SysInfo
0x08035bf8                 bl 0x080ca440
0x080ca440                   bl 0x0815f818
0x0815f818                     bl 0x0815f7c0
0x0815f7c0                       bl 0x0812289c
0x0812289c   81228b4: bl 0x0805c430      ; toma el mutex de [obj+0x2c]
             81228c4: ldrb r0,[r4,#0x28] ; <- LA BANDERA
             81228c8: cmp  r0,#0
             81228cc: beq  0x81228bc     ; 81228bc: bl 0x080c8968  = esperar
```

**`while (![obj+0x28]) wait();`** — la interfaz espera a que haya un volumen
montado. Con disco, `0x081228d0` (la salida del bucle) no se alcanza jamás; sin
disco sí. La CPU se va al bucle idle porque la tarea está bloqueada en un objeto
del kernel, no girando: por eso 27 000 IRQ y ninguna otra excepción.

Quién levanta la bandera: **sólo** `0x08122a04`, con dos llamadores,
`0x08059ae4` y `0x081229f0`, los dos dentro de `0x08059aa0` = «montar el
volumen». Y `0x08059aa0` tiene **un solo** llamador, `0x083287e0`, que es el
cuerpo de la tarea que consume la cola 4:

```
0x0832878c   for(;;) { msg = KS_receive(4); switch (msg[0x10]) {
                 case 0: 0x08059aa0(dev, 0);   // montar
                 case 1: 0x08126638(dev);      // sin medio
                 case 2: 0x081263dc(dev); ... } }
```

Sin disco, esa tarea recibe mensajes y monta (falla el sondeo, pero **levanta la
bandera igual** y la interfaz sigue). Con disco **no recibe ninguno**:
`0x083287b4` no se alcanza en 90 s.

### Y el mensaje sí se envía: el que no llega a escuchar es el receptor

Esto hay que decirlo con cuidado porque la primera lectura fue la equivocada. El
único emisor de mensajes de tipo 0 de la imagen es `0x0813b160` (mensaje
estático en `0x0813b21c`, `[msg+0x10] = 0`), al que se llega desde `0x0813b220`,
que es el cuerpo de la tarea **`"ATAWorkLoopTask"`** (cadena en `0x0813b2bc`,
creada en `0x0813b234` con prioridad 24). Parecía razonable pensar que con disco
no se ejecutaba. **Es falso, y medirlo cuesta un minuto:** con `A.img` y sin
disco las cuatro primeras paradas de `0x0813b160`, `0x0813b1ac` y `0x0813b1c0`
son **idénticas registro a registro**. El mensaje se envía en los dos casos.

Lo que cambia es el **receptor**. Antes de su primer `KS_receive(4)`, la tarea
`0x0832878c` hace dos llamadas:

```asm
832878c: push {r4,r5,r6,lr}
8328790: mov r0,#1
8328794: bl 0x081c207c        ; coger el dispositivo 1     <- vuelve
8328798: mov r5,r0
832879c: bl 0x08126638        ; <- CON DISCO NO VUELVE
83287a0: mov r1,#0 ; mov r0,#4 ; bl 0x080159a0   ; recibir  <- nunca se llega
```

Medido en las dos configuraciones: sin disco las paradas llegan hasta
`0x083287a0` y se repiten; con disco se paran en `0x0832879c` y ahí acaban.

Y de ahí hacia abajo, cada eslabón es otra parada real:

```
0x08126638   lock([dev+0x10]) ; blx [[dev+4]+0x18] -> 0x0823d420
0x0823d420     el sondeo de almacenamiento: reserva 4096 B, LBA 0
0x0823d52c       bl 0x0827aad0                       <- CON DISCO NO VUELVE
0x0827aad0         bl 0x0827a3f8 ; blx [r0+8]        <- vuelve (r0 = 0)
0x0827ab10         bl 0x0827aa1c -> 0x0827a980       ; leer y clasificar LBA 0
0x0827a9ac           blx r3 -> 0x080aea1c            <- AQUÍ SE PARA
                       r0 = 0x8be5d3e0 (alias sin caché de 0x0be5d3e0),
                       r2 = 1  -> "léeme UN bloque"
0x080aea1c             blx [[0x08966004+4]+0 +0x0c]  ; el read del almacenamiento
```

`0x0827a980` es corto y lo dice todo: llama al read, y si vuelve con cero mira
`buf+0x1FE` buscando `0xAA55` (→ `0x080aee04`, parser de MBR) y si no `buf+0`
buscando `0x4552` `'ER'` (→ `0x080a38cc`, parser de Apple Partition Map). **Nunca
llega a mirar.**

Y lo que hace que esto sea un hallazgo y no un callejón: **la lectura sí
ocurre**. `ATATRACE=1` enseña la orden `0xC8`, `ATA_TBUF_START = 0x0be5d3e0`,
`ATA_TBUF_SIZE = 0x1000`, la transferencia, el huésped leyendo `ATA_IRQSTAT`,
reconociéndola con un uno y parando el motor con `ATA_COMMAND = 2`. El disco
entrega, la interrupción llega y el huésped la acusa — y aun así la llamada de
lectura no vuelve. **El agujero está entre «el DMA terminó» y «despierta a quien
lo pidió»**, no en el ATA.

Es el mismo punto con disco a ceros, con `A.img` (MBR de 512 bytes, partición de
firmware real y FAT32 con `iPod_Control`) y con `E.img` (MBR en unidades de
4096): las tres se paran ahí. El contenido del disco sigue sin importar, y ahora
se sabe por qué: nadie llega a mirarlo.

> **Matiz de auditoría, porque la frase original decía «trazas idénticas de 682
> líneas» y eso es más de lo que se puede afirmar.** Repetido con `ATATRACE=1`
> sobre una imagen a ceros y otra con MBR válido: **680 líneas cada una y una
> sola diferencia**, `ATA_TBUF_START` (`+0x3c`), que sale `0x0be5d3c0` en una y
> `0x0be5d3e0` en la otra. Y no es el contenido del disco: la misma imagen a
> ceros da `0x0be5d3e0` en una carrera bajo `gdbpoke` y `0x0be5d3c0` en otra sin
> depurador. Hay **variación de carrera a carrera** en dónde cae ese búfer, así
> que «idénticas» hay que leerlo como «iguales salvo la dirección del búfer».
> La conclusión no cambia; la palabra «idénticas» sí era demasiado fuerte.

### Lo que se arregló por el camino: GPIO pin 86

Hay un segundo camino hacia la misma bandera y estaba roto por un bit.
`0x08328720` es un bucle de sondeo que decide entre «montar» y «sin medio»:

```asm
bl 0x08294d7c ; cmp r0,#0 ; bne sin_medio     (PMU 0x4b bit 2)   medido: 0
bl 0x08295508 ; cmp r0,#0 ; bne sin_medio     (PMU 0x12/0x4b)    medido: 0
bl 0x080a1474 ; cmp r0,#0 ; bne sin_medio     ([0x089120a1]!=100) medido: 0
bl 0x080aeca4 ; cmp r0,#0 ; movne r1,#0       gpio_get_pin(86)   medido: 0  <-
mov r0,#4 ; bl 0x080b2ce8                     ; anunciar {r1}
```

`0x080aeca4` es literalmente `gpio_get_pin(86)`, y `0x3CF00000` era
`create_unimplemented_device`, o sea cero: el firmware concluía «no hay medio»
aunque hubiera disco. Ahora hay un modelo de GPIO
(`hw/arm/ipod_classic/ipod_classic_gpio.c`) y el pin 86 sigue a si hay `-drive`;
medido después del cambio, `0x080aeca4` devuelve 1 y el mensaje pasa a ser de
tipo 0. **Necesario y no suficiente**: `0x080b2ce8` despacha por
`[[0x08911ad8]+0x10]`, y ese puntero sigue valiendo 0 a los 35 s, así que el
anuncio se tira. Lo que el pin 86 arregla es real y estaba mal; lo que queda es
otra cosa.

Qué es el pin 86 en la placa: `INDETERMINADO`. Qué hace en el firmware de Apple:
verificado arriba.

### El obstáculo concreto que queda

> **RESUELTO, 10 de agosto de 2026 — ver §14, y con una corrección de fondo.**
> El diagnóstico «el agujero está entre *el DMA terminó* y *despierta a quien lo
> pidió*» señalaba el hueco correcto pero el periférico equivocado: **el ATA no
> tiene nada que ver**, su ciclo se cierra entero y su semáforo despierta. Quien
> no vuelve es un `memcpy` posterior por **DMAC0**, que dormía en un semáforo sin
> *timeout* esperando una IRQ 16 que nadie generaba porque `0x38200000` era
> `create_unimplemented_device`. Todo lo que sigue en esta sección describe
> síntomas reales; la conclusión sobre el ATA, no.

> **Con disco, la lectura de un bloque de LBA 0 (`0x080aea1c`, llamada desde
> `0x0827a9ac`) no vuelve nunca, aunque el DMA de ATA se completa y el huésped
> acusa la interrupción.** Todo lo demás está comprobado y funciona: el ATA
> entrega los bytes, los proveedores de recursos están montados, el contenedor
> empotrado está en RAM, el panel dibuja lo que se le mande y la ventana se fija
> bien.

Lo que se sabe del ATA en ese momento (`ATATRACE=1`, disco a ceros, `A.img` y
`E.img`: **las tres trazas son idénticas, 682 líneas**):

- La orden `0xEC` (IDENTIFY) se completa.
- Dos lecturas por DMA, las dos en LBA 0: `0x1000` bytes a `0x0be5d3e0` y
  `0x8000` bytes a `0x0be55320` — que son justo el búfer de 4096 de
  `0x0823d420` y su segunda pasada.
- Después el driver cierra limpiamente (`ATA_CFG = 0x640`, lee `+0x70`/`+0x7c`)
  y **no vuelve a hablar**. No está girando sobre ningún registro: el bloqueo
  está por encima del driver.
- Sin disco la traza acaba en `WR +0x70 = 0xec` sin respuesta. El IDENTIFY falla,
  `0x0823d420` sale por su rama temprana (`0x0823d44c`) sin llegar nunca a
  `0x0827aad0`, y por eso el camino sin disco no se bloquea: **se bloquea el que
  encuentra un disco, no el que no lo encuentra.**

Lo primero que hay que hacer mañana es entrar en `0x080aea1c` con `gdbpoke.py`
—`blx [[0x08966004+4]+0 +0x0c]`, dos o tres niveles más— y averiguar en qué
objeto del kernel se queda dormida la petición. La sospecha, `INFERIDO`: la
notificación de fin de transferencia no llega a la tarea que la espera. Dos
cosas que descartar primero, las dos baratas: que la línea de interrupción del
ATA (`S5L8702_ATA_IRQ`) esté en el VIC que el firmware ha desenmascarado, y que
`0x0823d420` no esté esperando además una segunda interrupción del bloque de
ATA que el modelo no genera (el modelo sólo levanta `ATA_IRQSTAT` bit 0, desde
INTRQ del núcleo IDE; qué hacen los demás bits es `INDETERMINADO`).

`INFERIDO`, y hay que comprobarlo antes de creérselo: si eso se destraba, se
monta un volumen; si el volumen es la partición `rsrc` del `.ipsw`, aparece
`\Resources\Fonts\Helvetica.ttf`; con eso `0x082393ec` deja de devolver `{0,0}`
y la composición de la interfaz puede terminar. Sólo entonces se sabrá de dónde
lee el motor de trama de `+0x70`, que es la última incógnita del camino a
píxeles.

---

## 7. Cómo reproducirlo

```bash
wsl -d Ubuntu-24.04 -e bash -c "cd /mnt/c/Desarrollo/qemu-ios/build && ninja qemu-system-arm"
cd build

# Sin disco: el ciclo de caída. Mirar STDERR, que es donde habla el firmware.
./qemu-system-arm -M iPod-Classic -kernel ../assets/classic6g/osos_decrypted.bin \
    -display none -monitor none -serial null

# Con disco: arranque limpio hasta el bucle idle.
dd if=/dev/zero of=/tmp/ipod.img bs=1M count=64
./qemu-system-arm -M iPod-Classic -kernel ../assets/classic6g/osos_decrypted.bin \
    -display none -monitor none -serial null \
    -drive if=ide,format=raw,file=/tmp/ipod.img

# Qué periféricos pide y cuáles no están.
... -d unimp,guest_errors -D /tmp/u.log

# Toda la conversación con el ATA, que es como se sacó el protocolo del §5.
ATATRACE=1 ... 2>/tmp/ata.txt

# Dónde está parado.
(sleep 25; printf 'info registers\n'; sleep 2; printf 'quit\n') | \
    ./qemu-system-arm ... -monitor stdio
```

Para saber si un trozo de código llegó a ejecutarse — la pregunta más útil de
toda la sesión — `-d in_asm` y buscar su dirección:

```bash
./qemu-system-arm ... -d in_asm -D /tmp/asm.log
grep -c '^0x0805712c' /tmp/asm.log     # 0 = ningún bloque empezó ahí
```

> **Y aquí está la trampa, que costó una medida falsa.** En esta compilación
> `-d in_asm` no desensambla (no hay capstone): imprime **una sola línea de
> dirección por bloque de traducción**, la del principio, seguida de
> `OBJD-T: <bytes>`. Comprobado: `0x22003b1c` — el `wfi` del bucle idle, que se
> ejecuta miles de veces — sale **cero** veces, porque su bloque empieza en
> `0x22003b0c`.
>
> Luego `grep -c '^0xDIRECCION'` sólo responde «¿corrió esto?» si la dirección
> es **entrada de bloque**: destino de un `bl`/`b`, o el `push` con el que
> empieza una función. Para cualquier otra da cero siempre y no significa nada.
> Antes de usarlo, desensambla la dirección y comprueba que hay un prólogo.

---

## 8. Lo que se aprendió del método

- **`-d cpu` no sirve aquí.** Genera 2 GB y 45 millones de líneas en 25 s y ni
  siquiera llega al primer fallo. Para localizar una excepción, `-d int,in_asm`
  interleaved: el bloque de traducción que se imprime justo antes de
  `Taking exception` es el que la provoca.
- **`grep -c '^0xDIRECCION'` sobre un log de `in_asm`** responde "¿esto llegó a
  correr?" en un segundo, y esa pregunta reordenó toda la investigación: sin
  ella habríamos seguido arreglando el parser de la partición de firmware, que
  nunca se ejecuta. **Pero sólo vale para entradas de bloque** — ver el aviso
  del §7. La primera versión de esta página lo aplicó a una dirección de en
  medio de una función y sacó un cero que no significaba nada; dio la casualidad
  de que la conclusión era correcta por otro camino. Es exactamente el error de
  método que avisa [09-historia-5g.md](09-historia-5g.md): comprobar la
  herramienta antes de creerse la medida.
- **Un puntero nulo no siempre falla.** En esta máquina VA 0 está mapeado por
  el propio firmware, así que `ldr r1,[r0]` con `r0 = 0` devuelve una palabra
  de la tabla de vectores y la caída ocurre más tarde y en otro sitio. `DFAR`
  con pinta de instrucción ARM (`0xEAxxxxxx`) es la pista.
- **Contar es necesario, pero no basta.** "Una palabra de píxel en 45 s" es un
  hecho; "luego el framebuffer va por DMA" era una inferencia con al menos otra
  explicación igual de buena (que el firmware se cae antes de empujar píxeles),
  y se coló como si fuese medida. Cuando una cuenta sale casi cero, la primera
  hipótesis a descartar es "no llegó a intentarlo". (Resuelto: eran cero
  píxeles y ocho parámetros de orden; ver §3.)

---

## 10. `gdbpoke.py`, y por qué cambia el ritmo de trabajo

`-d in_asm` sólo responde «¿se tradujo un bloque que empieza aquí?». La pregunta
que hacía falta era «¿qué hay en `r0` cuando el huésped llega a esta dirección?»,
y para eso hace falta un depurador. Esta WSL no tiene `gdb-multiarch`, así que
hay uno nuevo de 200 líneas que habla el protocolo remoto de GDB directamente
contra el `-gdb tcp::PORT` de QEMU:

```bash
sh tools/ipod_classic/gdbrun.sh 1300 $HOME/6g/z64.img 90 \
   --break 0x0815b1e0 --break 0x0815b230 --hits 6 --mem 0x0bede7a4:16
```

`gdbrun.sh` arranca QEMU con `-S -gdb`, espera y lanza `gdbpoke.py`.
`QEMU_EXTRA` añade argumentos (`-d int -D ...`). Todo el §9 y la corrección del
§4 salen de esta herramienta; sin ella habrían sido días de `-d in_asm`.

Además de `--break`/`--mem`/`--step` tiene `--skip FROM=TO[:R0]`, que reescribe
el PC en una parada: sirve para **saltarse una llamada sin tocar la imagen del
huésped**, que es como se demostró que esquivar el fallo de `0x0815b230` no
lleva a ninguna parte (§4).

> **Trampa que costó una hora y que hay que conocer.** QEMU **no** tiene lógica
> de «ignora el breakpoint del que acabo de salir»:
> `accel/tcg/cpu-exec.c:346` dispara siempre que `pc == bp->pc`. Un `c` pelado
> desde una parada vuelve a parar en el mismo sitio **para siempre**, y la
> primera versión de la herramienta sacó ocho paradas idénticas de una función
> con 374 llamadores sin que saltara ninguna alarma. La salida está tres líneas
> más arriba en el mismo fichero: *«Singlestep overrides breakpoints»*. `cont()`
> hace `s` y luego `c`.
>
> Y el caso de control que lo destapó: poner el breakpoint en una función con
> muchos llamadores y mirar si `lr` cambia entre paradas. Si no cambia, la
> herramienta está rota, no el firmware.

Segunda trampa, de direcciones: **los breakpoints son sobre direcciones
virtuales**. El firmware mapea VA 0 sobre IRAM0, así que la tabla de vectores se
ejecuta en `0x00000000`, no en `0x22000000`, y el manejador de Data Abort está
en **`0x00003400`** (`[0x22000010] = 0xEA000CFA`). Poner el breakpoint en
`0x22003400` no dispara nunca.

## 12. Auditoría independiente (agosto de 2026, antes de que hubiera imagen)

> Esta auditoría es **anterior** al §13/§14: sus filas «el retailOS da 0
> píxeles» y «el bloqueo con disco está en `0x080aea1c`» describen el estado de
> entonces. La tercera auditoría, que sí ve imagen, está en el §16.

Todo lo de arriba se volvió a medir desde cero. Lo que **se confirma**:

| Afirmación | Cómo se comprobó |
|---|---|
| El panel dibuja | `lcdtest.S` → `screendump` → 76 800/76 800 píxeles, 1920 colores, y **mirando la imagen**: una rampa roja en X y verde en Y con el corte en x=256 que el `and #0x1f` del test predice |
| El retailOS da 0 píxeles, con disco y sin él | `screendump` + `pixcount.py` en las dos configuraciones |
| Con disco no hay ninguna excepción escondida | 40 s de `-d int` = 33 838 `Taking exception 5 [IRQ]` y nada más; `R15 = 0x22003b20`, SVC |
| Sin disco, ciclo de caída | 28 `CPU_exception_handler` + 28 `SWRCON` en 25 s de stderr |
| Cero píxeles por el puerto de datos | `LCDTRACE=1`: 29 arranques × (3 CMD + 8 parámetros), `0 were pixels` |
| …y **no** es un artefacto del modelo | `-d guest_errors` no da **ni un** «Invalid access»: no hay escrituras de 8/16 bits al LCD que el filtro de `valid.min_access_size = 4` esté tirando |
| El bloqueo con disco está en `0x080aea1c` | `gdbpoke.py`, dos *breakpoints*: `0x0827a9ac` para una vez (`r0=0x8be5d3e0`, `r2=1`, `r3=0x080aea1c`, `lr=0x0827a994`), `0x080aea1c` para una vez (`lr=0x0827a9b0`) y en 120 s no vuelve a parar ninguno |
| Nadie llega a mirar el sector | `-d in_asm` con disco: `0x080aee04` (parser de MBR) y `0x080a38cc` (parser de APM) = **0** bloques |
| `0x0805712c`, `0x0815b1b8`, `0x08239658` no se ejecutan con disco | `-d in_asm`, y las cuatro direcciones verificadas como prólogos (`e92d….` = `push`) antes de creerse el cero |
| GPIO pin 86 | `0x08325584` desensamblado: `[GPIO_BASE + ((pin>>3)<<5) + 4] >> (pin&7)`, y `0x080aeca4` es `mov r0,#86; bl 0x8325584`. `GPIOTRACE=1` con disco: `rd +0x144 -> 0x40`, o sea pin 86 = 1 |
| `"Resources/Fonts"` en `0x08380628` | leído del fichero |
| `aupd_decrypted.bin` no toca el LCD | `LCDTRACE=1`: cero accesos |
| Sin regresión en el 5G | `navtest.sh`: 76 548 px en los 7 fotogramas, el resaltado se mueve (12 312, 12 114, 31 632) |

Lo que **se corrige**:

1. «Con disco sólo toca el bloque de LCD dos veces» → son **cinco escrituras**
   (§6).
2. «Las tres imágenes dan trazas de ATA idénticas de 682 líneas» → 680 líneas y
   una diferencia, y la diferencia es ruido de carrera, no del disco (§9).
3. La cabecera de `ipod_classic_gpio.h` decía que el pin 86 era «toda la razón»
   por la que un arranque con disco no dibujaba. **No lo es**, y la propia §9 ya
   lo decía: el anuncio se tira después en `0x080b2ce8` porque `[0x08911ad8]`
   sigue a cero (verificado a los 45 s con `xp /1wx`). Corregido en la cabecera.
4. `0x38900000` estaba dado por descartado con la prueba «ningún registro recibe
   una dirección de DRAM». Es el bloque con la tabla de gamma identidad y la
   geometría del panel: **el mejor candidato que hay** para el origen del motor
   de trama. Retirado el descarte y escrito en §3.
5. La afirmación «de dónde lee el motor de `+0x70` es indeterminado» era
   correcta pero se paró demasiado pronto: `0x08127f14` estaba a ocho
   instrucciones de las dos que sí se documentaron, y enseña que el objeto de
   dibujado **no es nulo** y que la llamada de dibujado **sí ocurre** (§3).

## 11. Ficheros de esta tanda

| Ruta | Qué es |
|---|---|
| `tools/ipod_classic/findlit.py` | dónde está un literal de 32 bits en la imagen, en VA, y qué `ldr rX,[pc,#imm]` lo cargan |
| `tools/ipod_classic/audit_run.sh` | arranca el retailOS, hace `screendump` y resume stderr |
| `tools/ipod_classic/audit_peek.sh` | arranca, espera y lee palabras de memoria física por el monitor |
| `hw/arm/ipod_classic/ipod_classic_lcd.c` | ahora con panel: memoria 320×240 RGB565, ventana, `0x2A/0x2B/0x2C`, `qemu_console`, y los registros `+0x70..+0x8C` del motor de trama |
| `hw/arm/ipod_classic/ipod_classic_gpio.c` | GPIO. Un solo pin modelado, el 86, y la razón está en la cabecera |
| `tools/ipod_classic/gdbpoke.py` | cliente del protocolo remoto de GDB: breakpoints, registros, memoria, saltos |
| `tools/ipod_classic/gdbrun.sh` | arranca QEMU con `-S -gdb` y lo conduce |
| `tools/ipod_classic/lcdtest.S` | caso de control del panel: dibuja una rampa de 320×240 |
| `tools/ipod_classic/pixcount.py` | cuenta píxeles no negros de un `screendump` |

### De la tanda del 10 de agosto (§13, §14)

| Ruta | Qué es |
|---|---|
| `hw/arm/ipod_classic/ipod_classic_disp.c` | el compositor de `0x38900000`: registros con lectura honesta, los cinco descriptores de capa y la composición sobre el panel |
| `include/hw/arm/ipod_classic/ipod_classic_disp.h` | el mapa de registros entero, con la procedencia de cada línea marcada (V) verificado en el desensamblado, (M) además medido, (U) indeterminado |
| `tools/ipod_classic/ppm2png.py` | convierte un `screendump` a PNG **para mirarlo**, y también lee framebuffers crudos sacados con `pmemsave` (`--raw 320x240:argb8888`) |
| `hw/arm/ipod_classic/ipod_classic.c` | DMAC0 y DMAC1 pasan a ser `TYPE_PL080` con IRQ 16/17 y la memoria del sistema como puerto maestro |

### De la tanda del §18

| Ruta | Qué es |
|---|---|
| `tools/ipod_classic/uirules.py` | lee la **tabla de reglas de la interfaz** —(disparador, manejador, acciones)— del binario de Apple, con el formato documentado en su cabecera. Es lo que convierte «¿MENU hace algo en esta pantalla?» en una consulta |
| `tools/ipod_classic/evtrace.py` | arranca QEMU con monitor **y** gdbstub, deja que arranque suelto, engancha, arma los puntos de ruptura y **entonces** pulsa la tecla desde un hilo. `--str rN` sigue un puntero y enseña la cadena que hay al final |
| `tools/ipod_classic/tests/timer_audit.c` | prueba desnuda del bloque de temporizadores: mide tres contadores libres contra `USEC_TIMER` y aísla qué escrituras reinician la cuenta |

---

## 13. `0x38900000` es el compositor, y de ahí salen los píxeles

El §3 lo dejó como «el mejor candidato». Lo era. Es el bloque que compone el
cuadro a partir de capas en DRAM, y el motor de trama del LCD (`+0x70`) sólo lo
presenta.

### Cómo se pasó de «793 direcciones» a un mapa de registros

Con el bloque cayendo al *io floor* sólo se veía **qué** direcciones recibían
escrituras. Dos pasos lo abrieron:

1. **Los veintisiete `ldr rX,=0x38900000`.** Un escaneo de todos los `LDR`
   relativos a PC de la imagen cuyo literal cae en `0x389xxxxx` da 27 sitios
   para `0x38900000` más cinco bases desplazadas —`0x389000f0`, `0x38900180`,
   `0x3890031c`, `0x38900364`, `0x38900400`—, y desensamblando esos 27 sale casi
   todo el mapa. La lista es **completa**, no «lo que apareció».
2. **La traza con el PC del huésped.** `DISPTRACE=1` imprime cada acceso con
   `pc~`, y eso encontró lo que el paso 1 no podía: `+0x200..+0x214` se escriben
   desde `0x081273f8..0x08127428`, que llega al bloque por `r6` y no por un
   literal propio. Los dos métodos se necesitan.

### El mapa, con la evidencia de cada línea

Todo está en `include/hw/arm/ipod_classic/ipod_classic_disp.h`, registro a
registro. Lo que importa aquí:

| Registro | Qué es | Cómo se sabe |
|---|---|---|
| `+0x008` | registro de control, seis campos de bits | seis funciones distintas hacen lectura-modificación-escritura sobre él |
| `+0x058 + k*0x18` | **los cinco descriptores de capa**, k = 0..4 | `0x08126ffc..0x0812716c`, un `switch` de cinco ramas que escribe los mismos seis registros |
| `+0x0d8/+0xe0/+0xe8` | tres configuraciones de capa | `0x080b0140` las construye como `(fmt<<28)\|(a<<16)\|(b<<12)\|c` |
| `+0x214` | `0x013f00ef` = 319 y 239, **la geometría del panel** | literal en `0x08127468` |
| `+0x3ac` | `(w<<20)\|(h<<8)\|…`, bit 0 = rotado | `0x080bcef8`, y `0x08127f54` lo llama con `(64,64,0,0,0)` → `0x04004000`, que es exactamente lo que escribe el arranque |
| `+0x400..+0xffc` | tres tablas de gamma de 256 entradas | `0x08127268` construye `tbl[i] = i*4` y `0x0806fe9c` la escribe |

El descriptor de capa, que es lo que dibuja:

```
+0x00  stride de origen en bytes
+0x04  (align<<24) | (fmt<<8) | ...   bit 16 puesto si NO es de 32 bpp
+0x08  DIRECCION DE ORIGEN, ya avanzada al primer píxel visible
+0x0c  (W<<16) | H     tamaño de destino
+0x10  W * bytes_por_pixel / 4
+0x14  (X<<16) | Y     posición de destino
```

Y la tabla de bits por píxel sale del `switch` de `0x08126f90`, que convierte el
formato en un multiplicador `r1 ∈ {1,2,4,8}` y calcula el stride como
`r1 * srcW / 2` — o sea `r1` es el doble de los bytes por píxel:
`fmt 0 → 4 bpp, 1 → 8, 2..5 → 16, 6..7 → 32`.

### Lo que un arranque real programa, medido

```
capa 0: stride 0x500 (1280 = 320*4), fmt 7 (32 bpp), origen 0x09767a24,
        320x240 en (0,0)
capa 1: stride 0x280 (640 = 320*2),  fmt 3 (16 bpp), origen 0x0be2326c,
        108x240 en (212,0)          <- 212 + 108 = 320 al píxel
```

**Y ahí estaba la imagen todo el tiempo.** `pmemsave 0x09767a24 0x4b000` y
mirarlo como 320×240 de 32 bits da la pantalla de idioma entera, ya compuesta.
No había que inventar nada: había que copiarla al panel.

`W` y `H` son **con signo**: mientras el panel derecho se desliza fuera de la
pantalla el firmware escribe `+0x07c = 0xffc900f0`, o sea anchura −55. Un
modelo que los lea sin signo dibuja 65 481 columnas de basura.

### El orden de bytes, que es lo único INFERIDO del camino

Se lee la fuente de 32 bits como ARGB8888 *little-endian* (bytes B, G, R, A) y
la de 16 bits como RGB565. El argumento, por orden de peso:

1. **El panel es RGB565 con el rojo arriba**, y eso no lo decimos nosotros:
   `firmware/export/config/ipod6g.h:85` de Rockbox dice
   `#define LCD_PIXELFORMAT RGB565` para este aparato exacto. Luego una capa de
   16 bits entregada tal cual es RGB565, y leída así el panel «Welcome» es el
   degradado azul grisáceo que el iPod tiene de verdad.
2. **La capa de 32 bits lleva el mismo dibujo ya compuesto** — su tercio derecho
   *es* ese degradado. Sólo la lectura ARGB8888 hace que las dos coincidan; la
   otra vuelve marrones a las dos.
3. Y entonces la barra de selección sale `0xff0484da` = **#0484da**, el azul de
   selección de Apple. Al revés sale #da8404, un naranja que no aparece en
   ninguna parte de esta interfaz.

Sigue siendo una inferencia — no hay *datasheet*, y el bloque lleva tablas de
matriz de color con las que podría permutar canales. Son dos líneas de
`ipod_classic_disp.c` si aparece evidencia mejor.

### Cuándo se compone, y por qué no en `+0x70`

El modelo compone en `gfx_update`, o sea justo antes de enseñar el panel, no al
escribir `LCD[0x70] = 1`. La razón es una medida: **con disco, `LCDTRACE` cuenta
un solo arranque del motor de trama en 45 s**, y después la interfaz sigue
re-apuntando los descriptores de capa durante la animación sin volver a tocar
`+0x70`. Un modelo que sólo compusiera ahí enseñaría un cuadro congelado.

Es una **decisión de modelado**, no una medida: se comporta como un motor que
barre sus capas de forma continua, que es a lo que se parece el juego de
registros. Está dicho en voz alta en `ipod_classic_lcd_gfx_update()` y es lo
primero que hay que revisar si alguna vez la pantalla va un cuadro por detrás.

### Lo que se guarda pero no se honra

Las tablas de gamma, las cuatro tablas de coeficientes de matriz de color, el
escalador (`+0x04c/+0x50/+0x54`), el rectángulo de destino (`+0x030/+0x034`) y
el bit de rotación de `+0x3ac`. **Todos son la identidad en todos los arranques
medidos** — las tablas de gamma son literalmente `tbl[i] = i*4` —, así que
honrarlos hoy no cambiaría un píxel y no habría forma de comprobar si están
bien. Se leen y se devuelven; nada más.

---

## 14. Por qué el arranque con disco estaba parado: DMAC0, no el ATA

**Corrección de fondo al §9.** La frase «el agujero está entre *el DMA terminó*
y *despierta a quien lo pidió*» apuntaba al sitio correcto pero al periférico
equivocado. El ATA cierra su ciclo entero: las dos transferencias por DMA
terminan, la interrupción llega y el semáforo RTXC del ATA se levanta y despierta
a quien esperaba. Lo que no vuelve nunca es un `memcpy` **posterior**, hecho por
DMAC0.

La cadena, desensamblada:

```
0x2200844c   memcpy por DMA (r0 = destino, r1 = origen, r2 = 0x1000)
  0x22004808.., 0x220049c4.., 0x22004b64..   programa DMAC0 canal 0
  0x22008600   dma_wait(controlador, canal):
                 sema = [0x2200adec + ctrl*32 + canal*4 - 32]   -> 0x5f (95)
                 KS_wait(sema)        <- codigo 1, SIN nodo de temporizador
```

`KS_wait` sin *timeout* no expira nunca. El único que puede levantar ese
semáforo es la ISR de fin de cuenta de DMAC0, `0x220027b4`, registrada en
`0x080357f4` sobre **IRQ 16**, entrada `0x22002810`. Y `0x38200000` era
`create_unimplemented_device`.

Además hacía daño en silencio: `0x22004b68` hace lectura-modificación-escritura
sobre `C0Control`, y con el bloque devolviendo cero la escritura final salía
`0x0c000000` en vez de llevar el bit `I`, los anchos y la cuenta. **Un modelo que
sólo generase la interrupción sin devolver bien las lecturas seguiría fallando.**

Son PL080 de verdad, y lo dicen dos fuentes que coinciden: Rockbox los nombra
(`firmware/target/arm/s5l8702/dma-s5l8702.h:33-36`, «2 PL080 DMA controllers» en
`0x38200000` y `0x39900000`) y los desplazamientos que el retailOS usa cuadran
byte a byte con `hw/dma/pl080.h`. QEMU ya trae el modelo, así que fue cablearlo:

- región de `0x1000`, no `0x100000`, para que el resto del bloque siga cayendo
  al *io floor* y `-d unimp` lo diga si el huésped lo toca;
- IRQ 16 y 17, que el huésped ya tiene desenmascaradas
  (`VIC0 INTENABLE = 0x21a3010f`); †
- y el puerto maestro tiene que ser la memoria del sistema, porque el huésped
  programa los canales con el alias **cacheado** (`0x0be55320`) mientras la CPU
  corre sobre el no cacheado (`0x8be55320`).

> † **Dos lecturas distintas de `INTENABLE`, y no están reconciliadas.** Aquí
> son `0x21A3010F`; `ipod_classic.c:640` e `ipod_classic_wheel.h:149` dicen
> `0x01A3010F`, tomado en otro momento del arranque. Difieren **sólo en el bit
> 29**. Ninguna de las dos conclusiones depende de eso: los bits 16, 17 y 23
> están puestos en las dos (`3` en el nibble 19..16, `A` en el 23..20). Cuál de
> las dos es el estado en régimen es `INDETERMINADO`, y quien lo mida que corrija
> aquí y en los dos sitios del código.

### Lo que se destrabó, medido

`-d in_asm` con `E.img`, contando sólo prólogos de función verificados. Todas
estas direcciones daban **cero** en todas las trazas anteriores del proyecto:

| Dirección | Qué es | Antes | Ahora |
|---|---|---|---|
| `0x080aee04` | parser de MBR | 0 | **1** |
| `0x0808f8f4` | localizar la partición de firmware | 0 | **1** |
| `0x0805712c` | **el parser `]ih[`** | 0 | **1** |
| `0x08040cf8` | montar `rsrc` como volumen 4 | 0 | **1** |
| `0x08122a04` | levantar la bandera de almacenamiento | 0 | **1** |
| `0x081228d0` | la **salida** de `while (![obj+0x28]) wait();` | 0 | **1** |
| `0x08239134` | iterar `Resources/Fonts` | 0 | **1** |
| `0x08239658` | construir el objeto de cadena | 0 | **1** |
| `0x0815b1b8` | el código de interfaz que dibuja texto | 0 | **1** |

Es exactamente la cadena que predecían los dos informes previos, eslabón por
eslabón, y ahora está medida de punta a punta. Que la pantalla tenga texto en
nueve escrituras distintas es la prueba independiente: esas tipografías sólo
existen en `\Resources\Fonts\` del volumen `rsrc` del disco.

`0x080a38cc` (parser de Apple Partition Map) sigue en cero, y ahora por la razón
buena: `E.img` lleva MBR, así que se toma la otra rama.

---

## 15. Cómo reproducir la imagen

```bash
wsl -d Ubuntu-24.04 -e bash -c "cd /mnt/c/Desarrollo/qemu-ios/build && ninja qemu-system-arm"

# la segunda partición: un FAT32 VACÍO. No hace falta nada dentro -- las
# tipografías están en la partición de FIRMWARE, no aquí (ver la nota de abajo)
truncate -s 64M ~/6g/fat32.img
mkfs.vfat -F 32 -n IPOD ~/6g/fat32.img

# el disco, a partir del .ipsw, con MBR en unidades de 4096
python3 tools/ipod_classic/mkdisk6g4k.py ~/6g/E.img \
        assets/extracted/Firmware-24.9.1.2 ~/6g/fat32.img

# arrancar y medir
sh tools/ipod_classic/audit_run.sh px 45 -serial null \
   -drive if=ide,format=raw,file=$HOME/6g/E.img
#   => non-black: 75463 (98.3%) ; distinct colours: 404

# y MIRARLA, que es la mitad de la medida
python3 tools/ipod_classic/ppm2png.py /tmp/aud/px.ppm /tmp/px.png
```

> **De dónde salen las tipografías, y conviene decirlo porque el §4 se puede
> leer mal.** El volumen FAT32 de esa receta está **vacío** (`mdir` no lista
> nada) y aun así la pantalla sale con texto en nueve escrituras. El volumen 4
> `rsrc` con `Resources/Fonts` **no está en el FAT32**: es una de las cuatro
> imágenes de la partición de firmware, `fwtool.py list
> assets/extracted/Firmware-24.9.1.2` la enseña como
> `[0] rsrc off=0x00007000 len=0x04e00000` (81 MB). O sea que el único
> ingrediente real es el `Firmware-24.9.1.2` del `.ipsw`; el FAT32 sólo tiene
> que existir para que la segunda entrada del MBR apunte a algo. Comprobado
> creando el FAT32 desde cero con las dos órdenes de arriba: misma imagen,
> mismos 75 463 píxeles.

Tiempo hasta el primer píxel: entre 22 y 30 s de reloj de pared en esta máquina
(medido con tres `screendump` a 22, 30 y 38 s: negro, imagen, la misma imagen).
Después se queda quieta: la animación de la interfaz termina y no hay entrada de
rueda que la mueva.

Para ver qué hace el compositor:

```bash
DISPTRACE=1 ./qemu-system-arm -M iPod-Classic ... 2>&1 | grep -E 'SOURCE|LAYER'
```

### Regresiones comprobadas

| Prueba | Resultado |
|---|---|
| `lcdtest.S`, el caso de control del panel | 76 800/76 800 píxeles, 1920 colores — **igual que antes** |
| retailOS **sin** disco | 0 píxeles y el mismo ciclo de caída — igual que antes, y es del huésped |
| `tools/ipod_video/navtest.sh` (el 5G) | 76 548 px en los 7 fotogramas, el resaltado se mueve (12 312, 12 114, ~31 600) |

> Nota sobre el último número: varía de carrera a carrera (31 632, 31 603,
> 31 588 en tres pasadas) porque el test pulsa teclas por reloj de pared. Los
> 76 548 y los dos saltos de 12 312 / 12 114 sí son estables.

### Lo siguiente, en orden de coste

1. ~~**La rueda de pulsación** (`0x3C200000`, hoy sin implementar). El huésped la
   sondea ~120 veces por segundo, así que ya hay quien escuche; falta el modelo.
   Sin ella la interfaz se ve pero no se puede usar.~~ **HECHO, §17.** Y la
   frase estaba mal: el huésped **no** sondea. Hace una sola transacción
   sondeada en el arranque y después espera la **IRQ 23**. Corregido en §17.6.
2. **El arranque sin disco** sigue en el ciclo de caída del §4, y ahora se sabe
   que es irreparable por ese camino: no hay tipografías dentro de la imagen.
3. **Las capas de 4 y 8 bits** no se decodifican (falta la paleta) y ninguna se
   ha visto usada. `-d unimp` lo dirá si aparecen.
4. **El escalador y la matriz de color**, si alguna vez el huésped deja de
   escribir la identidad — por ejemplo al reproducir vídeo.

---

## 16. Tercera auditoría independiente (10 de agosto de 2026)

Se volvió a medir todo lo del §13, §14 y §15 desde cero, con el árbol tal cual
está y sin creerse ninguna cifra de antes.

### Lo que se **confirma**

| Afirmación | Cómo se comprobó aquí |
|---|---|
| **Hay imagen, y es la interfaz de Apple** | `audit_run.sh px 45` con `E.img` → **75 463/76 800 píxeles, 404 colores**, y la imagen **mirada**: barra de título «Language» con batería y candado, lista de nueve idiomas con 日本語 y Ελληνικά, barra de selección azul sobre «English», y el panel «Welcome/Bienvenue/Willkommen» a la derecha. No es basura ni un patrón de prueba |
| Reproducible | Cuatro arranques distintos dan el **mismo** 75 463; uno de ellos con un disco **construido desde cero** con la receta del §15 (`truncate` + `mkfs.vfat` + `mkdisk6g4k.py`) |
| **Dibuja y no se muere** | 50 s de `-d int,guest_errors,unimp` con disco: **0 `Taking exception 4`**, 0 `SWRCON`, 0 `CPU_exception_handler`, 43 840 IRQ y nada más. Los cambios de modo a `abt`/`fiq`/`und`/`irq` son uno de cada, del `msr cpsr` con el que el firmware monta las pilas por modo; no son excepciones |
| Sin disco no cambia nada | 30 s → **0 píxeles**, 30 *Data Abort* + 30 semihosting + 30 `SWRCON`: el mismo ciclo de caída del §4 |
| Caso de control del panel intacto | `lcdtest.S` → **76 800/76 800, 1920 colores**, y la rampa mirada |
| Sin regresión en el 5G | `navtest.sh` → **76 548 px** en los 7 fotogramas y el resaltado se mueve: 12 312, 12 114, 31 598 |
| La ventana de 4 KB basta | En el arranque de 50 s con `-d unimp` **no aparece ni una línea `s5l8702.io:`**: cero accesos al *io floor* en todo el arranque |
| El *switch* de descriptores de capa | Desensamblado a mano: tabla de saltos en `0x08127068`, cinco ramas en `0x08127080/b0/e0/110/140`, todas escribiendo `base + 0x00/04/0c/10/14` |
| La tabla de bytes por píxel | El *switch* de `0x08126f90` da `r1 ∈ {1,2,4,8}` para `fmt` 0 / 1 / 2‑5 / 6‑7 y luego `mul r2,r1,r2 ; lsr r2,r2,#1`, o sea `r1 = 2 × bpp`. Cuadra con `disp_bpp()` entrada por entrada |
| El bit 16 de `+0x04` | `cmp r1,#8 ; orrcc r3,r3,#0x10000` en `0x08127050`: puesto exactamente cuando **no** es de 32 bpp |
| Las direcciones del §14 son entradas de verdad | `0x0805712c`, `0x080aee04` y `0x08040cf8` empiezan por `push`; `0x081228d0` no es un prólogo pero **sí** es cabeza de bloque de traducción (es el destino de caída del `beq` de `0x081228cc`), así que medirlo con `-d in_asm` es válido |
| DMAC0/1 son PL080 y van en IRQ 16/17 | Rockbox, fuente independiente: `s5l87xx.h:1893`, `#define IRQ_DMAC(d) (16+(d))` |

### El experimento de control que faltaba: ¿de verdad hacían falta las dos?

El §13 y el §14 afirman que la imagen necesitaba el compositor **y** los PL080.
El compositor es evidente, es lo único que dibuja; lo del DMAC0 era una
inferencia sobre una cadena larga. Se comprobó quitándolo:

> Con el compositor puesto y los dos PL080 fuera, arranque con `E.img` de 45 s:
> **0 píxeles, 0 colores, y ni una sola línea de `SOURCE ADDRESS`**. El
> compositor está ahí y nadie le da nunca una dirección de origen.

O sea: **las dos hacían falta**, y el §14 no es una historia bonita sino una
causa. (El parche era un `getenv` provisional; se revirtió, no está en el árbol.)

### Lo que se **corrige**

1. **La cabecera de `ipod_classic_disp.c` decía lo contrario de lo que hace el
   fichero**: «un fichero de registros… deliberadamente menos que un motor de
   composición… un compositor aquí estaría inventando píxeles», treinta líneas
   encima de `ipod_classic_disp_compose()`. Reescrita.
2. **`ipod_classic_disp_compose()` decía «no two layers have ever overlapped in
   a boot we have traced». Es falso**, y era justo el dato que sostiene el orden
   Z. Medido con `DISPTRACE=1`: la capa 1 va `(x=80,w=240)`, `(82,238)`,
   `(95,225)`, `(122,198)`, `(168,152)`, `(232,88)`, `(310,10)`, `(400,−80)`, con
   `x + w = 320` en todos los pasos, encima de una capa 0 de 320×240 en (0,0).
   Es un **barrido** y solapa siempre. Corregido, y con ello el orden Z pasa de
   «no hay medida» a «hay una medida a favor»: la capa que se desliza tiene que
   ir **delante** o no hay transición.
3. **El aviso de fin de trama del LCD mentía.** `panel_frame_go()` seguía
   emitiendo por `LOG_UNIMP` «no source address exists anywhere in this window
   and no DMA channel points at LCD_WDATA, so where it would read from is
   UNDETERMINED», que dejó de ser cierto en el §13. Reescrito: lo que sigue
   siendo INDETERMINADO no es de dónde lee, es si `+0x70` **engancha** un cuadro
   o sólo habilita el barrido.
4. **El contador de direcciones de origen contaba transiciones, no escrituras.**
   `if (value != last_src)` con dos ranuras que se turnan imprime una línea por
   escritura, no «una por valor distinto» como decía el comentario: salían 21
   líneas por arranque a `stderr`, que es justo el canal que la lección 1 pide
   mantener limpio. Ahora se anuncia **la primera** y punto, y el total del final
   cuenta **todas** las escrituras. De paso, esas escrituras ya no se saltan la
   traza: `DISPTRACE=1` decía «cada acceso» y se dejaba fuera `+0x060` y `+0x078`
   por un `return` temprano.
5. **`composed_frames` se incrementaba y no se imprimía nunca.** Ahora sale en
   los totales de `LCDTRACE`, junto a los arranques del motor de trama.
6. **La cabecera decía que el *switch* de capas escribe «los mismos seis
   registros».** Escribe **cinco**: `+0x08`, la dirección de origen, viene de
   otro sitio (`0x081276dc`). Es la diferencia entre «esta capa tiene geometría»
   y «esta capa tiene algo que leer», que es exactamente lo que distingue un
   arranque sin disco de uno con disco.
7. **La receta del §15 no era reproducible**: pedía un `~/6g/fat32.img` sin decir
   cómo hacerlo. Se documenta, y con ello un hecho que no estaba escrito: ese
   FAT32 está **vacío** y da igual, porque las tipografías están en la imagen
   `rsrc` de la partición de **firmware**, no en el volumen FAT.
8. **`0x00001000` como valor de las tres configuraciones de capa** es de un
   arranque **sin** disco. Con disco `+0x0e0` y `+0x0e8` se reescriben 17 y 21
   veces. Anotado en la cabecera.
9. **Los §3, §6, §9 y §12 describían el estado anterior sin decirlo**, y el §6
   contradice de plano la tabla del §14 (las mismas nueve direcciones, «nunca» en
   uno y «1» en el otro). Marcados como históricos.

### Lo que **no** se pudo verificar, y hay que decirlo

- **La composición de varias capas nunca se ha visto en una imagen.** La capa 0
  sola ya contiene la pantalla entera: se volcó con
  `pmemsave 0x09767a24 0x4b000` y `ppm2png.py --raw 320x240:argb8888`, y es lo
  mismo que sale por pantalla. La capa 1, volcada igual (`0x0be2326c`,
  `rgb565`), es sólo el degradado **sin texto**. O sea que los 75 463 píxeles
  prueban muy bien la ranura 0 —dirección, *stride*, formato y orden de bytes— y
  **no prueban** el orden Z ni el recorte, porque el barrido de la capa 1 dura
  menos de un segundo y ningún `screendump` lo ha cogido (probado a 20, 21, …,
  28, 30 y 32 s: negro hasta 28 s, la pantalla ya quieta a 30 s). Si alguien
  quiere cerrarlo, lo barato es un contador por ranura en `disp_draw_layer()` y
  un `screendump` disparado desde el propio modelo la primera vez que dos capas
  se solapen.
- **Los «~120 sondeos por segundo» de la rueda** son una cifra de antes. En el
  arranque con disco de esta auditoría salen 3 175 lecturas de `0x3C200000` en
  50 s, pero con `-d int` puesto, que cambia el reloj de pared. La conclusión
  («ya hay quien escuche») no depende del número.
- **El orden de bytes sigue siendo INFERIDO**, y bien marcado en el código. La
  imagen lo respalda de una forma que cuesta discutir —la barra de selección sale
  `#0484da` y no `#da8404`—, pero eso es un argumento sobre los colores de Apple,
  no un *datasheet*.

---

## 17. La rueda de clic, y la pantalla que por fin se puede mover (10 de agosto de 2026)

Hasta aquí el retailOS **se veía**. Ahora **se navega**: la rueda mueve la barra
de selección por la lista de idiomas, y se puede demostrar con una medida que
distingue "se movió un elemento" de "parpadeó un icono".

Piezas nuevas:

| Fichero | Qué es |
|---|---|
| `hw/arm/ipod_classic/ipod_classic_wheel.c` | el bloque `0x3C200000`, IRQ 23, y el manejador de teclado |
| `include/hw/arm/ipod_classic/ipod_classic_wheel.h` | el mapa de registros y de dónde sale cada línea |
| `tools/ipod_classic/navtest.sh` | la prueba: arranca, espera, pulsa, captura y decide |

Y una corrección de una línea en `ipod_classic_pmu.c` que resultó valer tanto
como todo el modelo de la rueda; ver §17.4.

### 17.1 El mapa de registros

La base y los desplazamientos son de Rockbox (`firmware/export/s5l87xx.h:1642-1651`,
`WHEEL_BASE 0x3C200000`, y `IRQ_WHEEL 23` en la `:1901`). Los **nombres**, en
cambio, son casi todos nuestros: Rockbox sólo llama por su función a `WHEELINT`,
`WHEELRX` y `WHEELTX`, y a los otros cinco los llama `WHEEL00`, `WHEEL04`,
`WHEEL08`, `WHEEL0C` y `WHEEL10`. `WHEELCON`, `WHEELGO`, `WHEELDIV`,
`WHEELSTAT`, `WHEELMASK` y `WHEELPEND` son inventos de este puerto para decir lo
que creemos que hace cada uno. La semántica sí es del desensamblado de Apple,
con `--adjust-vma=0x07FF51B8`.

| Off | Nombre | Qué hace el retailOS | Estado |
|---|---|---|---|
| `0x00` | `WHEELCON` | init `0x08056198`: `&=0xFFFF0000`, `&=~0x70000`, `&=~0x80000`, `&=~0x400000`, `\|=0x100000`, `\|=0x200000`. Antes de cada TX apaga y enciende el bit 21 | secuencia **VERIFICADA**, bits **DESCONOCIDOS** |
| `0x04` | `WHEELGO` | `\|= 1` lanza la transferencia (`0x0832797c`) | **VERIFICADO** |
| `0x08` | `WHEELDIV` | `= 0x0003A980` una vez (`0x080561e4`) | valor **VERIFICADO**, sentido **DESCONOCIDO** |
| `0x0C` | `WHEELSTAT` | bit 0 «hay palabra», bit 3 «TX en curso» (`0x08327994`) | **VERIFICADO por uso** |
| `0x10` | `WHEELMASK` | init `\|= 1`; guardado/puesto a 0/restaurado alrededor de cada transacción sondeada (`0x080afb48`) | máscara **INFERIDA**; ver §17.4 |
| `0x14` | `WHEELPEND` | init `= 7`; el ISR escribe 7 al final (`0x08047a38`); el bucle escribe 1 para limpiar | **VERIFICADO**, escribir-1-para-limpiar |
| `0x18` | `WHEELRX` | la palabra recibida (`0x080479bc`) | **VERIFICADO** |
| `0x1C` | `WHEELTX` | `0x80000000 \| (cmd >> 1)`. Medido en el bus: `0xC000011D` | **VERIFICADO** |

Los cuatro pines: el init llama a `0x08325468(pin, 2, 0)` para `0x72..0x75`, que
es el `PCON14 = (PCON14 & ~0x00ffff00) | 0x00222200` de Rockbox
(`button-clickwheel.c:386`). Confirmado en el bus: el modelo de GPIO registra
`0xe0202/0xe0302/0xe0402/0xe0502` en `+0x200`.

### 17.2 Los dos paquetes

**El que mueve la interfaz** — el ISR `0x080479a8` lo acepta si
`(rx & 0xBC0000FF) == 0x8000001A`:

```
 31    30        25..16          12..8        7..0
 +---+------+---------------+-----------+-----------+
 | 1 | dedo |   posición    |  botones  |   0x1A    |
 +---+------+---------------+-----------+-----------+
```

Rockbox describe el mismo paquete con la máscara más floja `0x800000FF`
(`button-clickwheel.c:121`): dos fuentes independientes de acuerdo.

**La respuesta a una orden sondeada** — validada en `0x0832783c` con
`bic #0x7F000000` + `bic #0x00FF0000` + `cmp`: bit 31 y bits 15..0 iguales a la
orden, bits 30..16 libres para los datos.

Botones, bits 8..12 del paquete, y el código de tecla que emite `0x0805c11c`:
SELECT→3, adelante→1, atrás→2, PLAY→4, MENU→0. Los bits son los de Rockbox.

### 17.3 Por qué el modelo se marca el ritmo con un temporizador

El decodificador de giro `0x0807a670` impone tres condiciones duras:

1. **96 posiciones por vuelta** (los `±96` de `0x0807a6a4`, y el
   `WHEELCLICKS_PER_ROTATION` de Rockbox).
2. Hacen falta **dos paquetes seguidos con el bit 30 puesto** antes de que se
   calcule ningún delta: el primero sólo arma.
3. El umbral vuelve a **6** con cada paquete sin dedo, y la ventana aceptada es
   **−35..+35** (`0x0807a6dc`).

De la 2 sale la decisión de diseño que no es evidente: **los paquetes se
espacian con un temporizador, uno cada 40 ms de reloj virtual, en vez de
encolarse de golpe**. El ISR sobrescribe `0x089116d8+0x20` en cada paquete; si
dos entran antes de que la tarea de rueda corra una vez, el paquete de armado
desaparece y el movimiento se lee como un primer toque y se tira. Una FIFO no
basta: hace falta que el huésped vea cada paquete como su propia interrupción.

Una pulsación de tecla son `IPOD_WHEEL_STEP` posiciones, por defecto **6**, que
no es un número redondo elegido a ojo sino el umbral que el propio firmware
tiene en `0x089116d8+0x04` en reposo, leído por el monitor.

### 17.4 Las dos trampas, y la segunda costó una tarde

**Primera: `WHEELMASK` tiene que leerse honradamente.** El firmware lo guarda,
lo pone a cero alrededor de una transacción sondeada y lo restaura. Mientras el
bloque era `create_unimplemented_device` toda lectura daba cero, así que lo que
restauraba era **cero**, y en el log se ve: doce escrituras de `0` a `+0x10`
frente a una sola de `1`. Sin lectura honrada la interrupción queda enmascarada
para siempre y ninguna pulsación llega jamás.

**Segunda, y es la lección `HI_IRQ` del 5G otra vez: el INTERRUPTOR DE BLOQUEO
del PMU.** Con la rueda ya modelada, el giro funcionaba y **los botones no**.
Medido, con la tecla mantenida tres segundos y leyendo el bloque de estado del
propio firmware:

```
0x089116d8+0x1C = 1   el paquete llegó y el ISR lo decodificó
0x089116d8+0x14 = 1   la tarea de rueda corrió el detector de flancos
                      0x0805c0e4 y llamó al despachador 0x0803f390
```

...y la pantalla no cambiaba. La causa está dentro del despachador, en
`0x0803f3e0` —la instrucción **número 21** de la función, después de la cadena
de cuatro comparaciones que remapea el código de tecla; una versión anterior de
este párrafo decía «una instrucción después de la entrada» y era falso—.
`0x0803f3e0` llama a `0x08030d48`, que es un `b 0x080634a4` pelado, y de ahí a
`0x08294f80`, que lee **un byte del registro `0x87` del PMU por I2C**
(`mov r0,#0x87` / `and r0,r0,#2` / `lsr r0,r0,#1`) y se queda con
`(byte >> 1) & 1`; `0x080634a4` devuelve `1 − eso` (`rsbs r0,r0,#1` /
`movcc r0,#0`); y `0x0803f3e4` **vuelve inmediatamente si es distinto de cero**
(`cmp r0,#0` / `popne {r4,r5,r6,pc}`). Con el registro a cero, cada botón se
tiraba en silencio.

(Y hay una segunda puerta justo detrás, en `0x0803f3ec`, que también hace
`pop` si un byte de otro bloque vale cero. No estorba —los botones funcionan—
pero conviene saber que está ahí antes de culpar al interruptor de bloqueo la
próxima vez.)

Rockbox dice qué es ese bit, y llega a la misma polaridad desde el otro lado
(`firmware/target/arm/s5l8702/ipod6g/pmu-6g.c:244`):

```c
pmu_input_holdswitch = !(pmu_read(PCF50635_REG_GPIOSTAT)
                            & PCF50635_GPIOSTAT_GPIO2);
```

O sea: **bit puesto = bloqueo desactivado**. El arreglo es una línea en
`ipod_classic_pmu_reset()`, marcada como VALOR DE CONVENIENCIA: una máquina sin
interruptor físico que mover tiene que arrancar con él desactivado o no se puede
usar.

Y la moraleja, que es la misma del `HI_IRQ`: **el giro no pasa por
`0x0803f390`**, así que no se veía afectado. El fallo parecía de la rueda y era
del PMU. Comprobar el camino entero significa comprobarlo para cada tipo de
evento, no una vez.


### 17.5 Lo que se ve, medido y mirado

`sh tools/ipod_classic/navtest.sh`, con `~/6g/E.img`. Teclas: `q`/`e` giran,
Enter selecciona, `w` es MENU, `a`/`d` son atrás/adelante, `s` es play — las
mismas del 5G, más las dos que faltaban, para que una sola costumbre valga para
las dos máquinas.

```
00-idle             75463 px encendidos
01-idle-again       75463 px encendidos        0 px distintos
02-wheel-fwd        75432 px encendidos     8000 px distintos (8000 en la lista, 0 en el panel)
03-wheel-fwd2       75463 px encendidos     8000 px distintos (8000 en la lista, 0 en el panel)
04-wheel-back       75432 px encendidos     8000 px distintos (8000 en la lista, 0 en el panel)
05-select           76800 px encendidos    50540 px distintos (38400 en la lista, 12140 en el panel)
06-select-later     76800 px encendidos        0 px distintos
07-menu             76800 px encendidos        0 px distintos
```

Esa carrera es de una máquina con el arranque limpio (cero reinicios) y **pasa
todo menos la última línea**: `10-menu-back` se perdió la pulsación de MENU, que
es el fallo de entrada del §18.8 y no del compositor. **En otras dos carreras
del mismo test la última línea también pasó**, y con la medida que importa:

```
10-menu-back   0 px de diferencia con 08-menu-wheel-back  -> MENU VOLVIO A LA MISMA PANTALLA
10-menu-back  23 px de diferencia con 08-menu-wheel-back  -> MENU VOLVIO A LA MISMA PANTALLA
```

**Cero y veintitrés píxeles** —los 23 son el icono de la batería del §18.7—
contra los 6 088 que cuesta cambiar de pantalla. No es «cambiaron muchos
píxeles»: es **el mismo fotograma del que se salió**.

Y **mirado**, que es la otra mitad de la medida:

- `00-idle`: la lista de idiomas con la barra azul sobre **English**.
- `02-wheel-fwd`: la barra sobre **日本語**. Un golpe de `e`, un elemento.
- `04-wheel-back`: `q` la devuelve a **日本語** desde **Čeština**.
- `05-select`: **otra pantalla entera** — el icono de la nota musical sobre el
  degradado gris y el texto **「ミュージックが…」**. Es decir: la selección no
  sólo cambió de pantalla, cambió al **idioma que estaba resaltado**. Esa
  coincidencia es lo que convierte "cambiaron muchos píxeles" en "la interfaz
  hizo lo que se le pidió".

Los **8000 píxeles** son exactamente lo que cuesta mover la barra: dos cajas de
160×25 encendiéndose y apagándose. El suelo de ruido de esta pantalla es
**0** —`01-idle-again` lo comprueba en cada carrera antes de creerse nada—, así
que no hay forma de confundirlo con el reloj de la barra de estado, que además
esta pantalla no tiene.

Del lado del modelo, en la misma carrera:

```
WHEELTRACE first command: tx 0xc000011d -> cmd 0x8000023a
WHEELTRACE first packet queued: 0x8000023a
WHEELTRACE irq 23 raised for the first time (pend 0x1, mask 0x1)
WHEELTRACE totals: 14 packets queued, 0 dropped with the FIFO full,
                   1 commands, 13 rising edges on IRQ 23
```

Tres cosas que decir de ahí:

- **`pend 0x1, mask 0x1`**: la máscara vale 1 y no 0. Es la prueba directa de
  que la lectura honrada de `WHEELMASK` era lo que faltaba (§17.4).
- **`1 commands`**: antes eran **cinco** escrituras de `WHEELTX`, los cinco
  reintentos de `0x0832780c` contra un bloque mudo. Ahora la primera responde.
- **`0 dropped`**: la FIFO nunca se llenó, o sea que el huésped drena al ritmo
  al que el modelo produce. Ese contador está puesto justo para que, si algún
  día el ISR deja de correr, lo diga en vez de dejarnos adivinando.

Repintados: **unos 420** escrituras de la dirección de origen de la capa 0 en la
carrera (417 en la del parche, 419 en la de la auditoría: la cuenta depende de
dónde caiga la captura dentro del doble búfer y **no** es un número exacto que
reproducir), contra **10** en una carrera en reposo. Lo que importa del dato es
el orden de magnitud: cuarenta veces más que en reposo.

Los ocho contadores de `WHEELTRACE`, en cambio, **sí** son exactos y cuadran con
las teclas una por una: 14 paquetes = 1 respuesta sondeada del arranque + 3 giros
× (armado + movimiento + dedo fuera) + Enter (pulsar y soltar) + `w` (pulsar y
soltar). Y 13 flancos de subida = 14 − 1, porque el paquete del arranque se
encola con `WHEELMASK` a cero y ése no levanta la línea. Reproducido idéntico en
la auditoría.

> **Ese «pulsar y soltar = dos paquetes» dejó de ser cierto en el §18.8**, y por
> una razón medida: dos paquetes por pulsación perdían la pulsación entera
> siempre que la tarea de rueda no corría entre ellos —seis de cada diez—. Ahora
> el estado de los botones se **repite** mientras están pulsados y durante
> 400 ms después de cada flanco, así que la cuenta de paquetes de una carrera
> con teclas es bastante mayor y ya no se puede predecir a mano. Los giros no
> cambian.

### 17.6 Correcciones a lo escrito antes

1. **§6, §15 y §16: «el huésped sondea la rueda ~120 veces por segundo, así que
   ya hay quien escuche».** Retirado. El firmware hace **una sola** transacción
   sondeada en el arranque (con cinco reintentos) y a partir de ahí es
   **puramente por interrupción**, IRQ 23: registra `0x080479a8` en
   `0x08047a68` y su tarea se duerme para siempre en `wait_event(5)`. Las miles
   de lecturas de `+0x14` que se contaban eran el giro de espera de esa única
   transacción contra un registro que devolvía cero. La conclusión de entonces
   («ya hay quien escuche») era casualmente correcta; el mecanismo, no. Un
   modelo que sólo devolviera datos en `WHEELRX` sin levantar la línea no habría
   producido **ni un solo** evento.
2. **§15, «lo siguiente en orden de coste», punto 1.** Hecho.

### 17.7 Lo que sigue sin saberse

> **RESUELTO en el §18, y los dos primeros puntos de esta lista estaban mal
> planteados.** «La pantalla que sigue a elegir idioma» era **el menú principal
> con la lista tapada**: el huésped la dibujaba entera y nuestro compositor le
> ponía encima una capa que el huésped ya había apagado. Y el panel de
> «Welcome» **no tiene ninguna regla** que le diga que siga al resaltado: la que
> se citaba aquí es de otra pantalla. Se dejan escritos tal cual, con lo que se
> creyó entonces, porque el §12 vive de eso.

- **MENU no vuelve atrás desde la pantalla que sigue a elegir idioma.** Medido
  dos veces, con esperas de 15 y de 25 s, y en las dos: **0 píxeles**. Si es
  porque esa pantalla del primer arranque no tiene a dónde volver, o porque
  falta algo, es **DESCONOCIDO**. Lo que sí está medido es que la tecla llega
  entera: con `gdbpoke.py` y breakpoints a lo largo de `0x0803f390`, el código
  remapeado (`0x6f` para SELECT) atraviesa el despachador, llega a
  `0x08170e94`, construye el mensaje `'kDwn'` y lo mete en la cola del
  EventManager, y la suelta hace lo mismo con `'kUp '`. El evento llega; lo que
  la pantalla decide hacer con él es de la aplicación. **Es el hito que falta.**

  **Medido en la auditoría, y estrecha bastante la busca:** esa pantalla **no
  está sorda**. Con la misma carrera, después de Enter:

  ```
  04-select        76800 px    la nota musical y 「ミュージックが…」
  05-post-wheel    76800 px   50307 px distintos   <- una `e`, la rueda
  06-post-menu     76800 px       0 px distintos   <- una `w`, MENU
  07-post-select   76800 px       0 px distintos   <- otro Enter
  ```

  La **rueda sí la mueve**: la nota desaparece y queda el degradado liso. Y no
  es que la animación siga sola, porque en la carrera de `navtest.sh` esa misma
  pantalla se queda **idéntica** 40 s sin tocar nada (`06-select-later`, 0 px).
  O sea: la capa de entrada entrega **giros y botones** a esa pantalla, ella
  reacciona al giro y **decide no hacer nada** con MENU ni con un segundo
  Enter. Eso apunta a que la pantalla es la del asistente de primer arranque y
  no tiene a dónde volver, no a que falte nada por debajo — pero sigue siendo
  **INFERIDO**, no verificado.

  Y en la **pantalla de idiomas** MENU tampoco hace nada (0 px), ni `a`
  (izquierda/anterior): también medido en la auditoría, con esperas de 8 s.
- **El panel derecho no cambia al girar** (0 píxeles a la derecha en los tres
  giros). La regla `list.pid.….delayedselected1 -> ShowSetting_Language` dice
  que debería. Que no se dispare apunta a la mitad temporizada de la interfaz
  —`TimerEventManager`, `0x081059c4`— y es el hilo más prometedor que queda.
- **Los bits 1 y 2 de `WHEELPEND`/`WHEELMASK`**, el significado de cada bit de
  `WHEELCON`, el `0x3A980` de `WHEELDIV` y la orden `0x8000062A`: todos
  **DESCONOCIDOS**, y ninguno hace falta.
- **Por qué el hardware real acepta las dos codificaciones de `WHEELTX`**
  (`0x8000023A` de Rockbox y `0xC000011D` de Apple): **DESCONOCIDO**. El modelo
  acepta las dos y lo dice en el código.
- **El ritmo real de la rueda** (los 40 ms del temporizador) es una constante de
  ajuste, no un dato del aparato. `IPOD_WHEEL_TICK_MS`, `IPOD_WHEEL_UNTOUCH_MS`
  e `IPOD_WHEEL_STEP` la dejan mover para experimentar.

### 17.8 Regresiones comprobadas

| Prueba | Resultado |
|---|---|
| `tools/ipod_classic/navtest.sh` | los tres giros dan 8000 px cada uno, y el suelo en reposo sigue siendo 0 |
| retailOS en reposo, dos capturas seguidas | 0 px de diferencia, igual que antes de la rueda |
| **`tools/ipod_video/navtest.sh` (el 5G)** | **76 548 px en los siete fotogramas, y el resaltado se mueve: 12 312, 12 114, y ~31 600** (31 602 en la carrera del parche, 31 609 en la de la auditoría). Los dos primeros son estables; el tercero **no lo es y no puede serlo**, porque la pantalla del 5G a la que entra Enter sí tiene reloj —el §15 lo registra como «~31 600» por eso—. Sin regresión |
| El bloque `0x3C200000` ya no está en `ipod_classic_unimplemented[]` | la ventana modelada son 4 KB, no 1 MB: el resto del bloque sigue cayendo al *io floor* y `-d unimp` lo diría. No lo dice |
| `stderr` de la carrera entera | ni una excepción, ni un reinicio, ni un `unimp`: quitando `DISPTRACE` y `WHEELTRACE`, la carrera imprime **cuatro** líneas y las cuatro son informativas. Navegar y morir no sería navegar |

### 17.9 Cómo reproducirlo

```bash
wsl -d Ubuntu-24.04 -e bash -c "cd /mnt/c/Desarrollo/qemu-ios/build && ninja qemu-system-arm"

# el disco, igual que en el sec.15
truncate -s 64M ~/6g/fat32.img && mkfs.vfat -F 32 -n IPOD ~/6g/fat32.img
python3 tools/ipod_classic/mkdisk6g4k.py ~/6g/E.img \
        assets/extracted/Firmware-24.9.1.2 ~/6g/fat32.img

# la prueba entera: arranca, espera, comprueba que la pantalla esta quieta,
# pulsa, captura y decide. Sale 0 si los tres giros movieron el resaltado.
sh tools/ipod_classic/navtest.sh

# y MIRAR los PNG que deja en ~/6g/nav, que es la otra mitad
```

A mano, para jugar:

```bash
cd build && ./qemu-system-arm -M iPod-Classic \
    -kernel ../assets/classic6g/osos_decrypted.bin -serial null \
    -drive if=ide,format=raw,file=$HOME/6g/E.img -snapshot
# q / e giran la rueda, Enter selecciona, w es MENU, a / d atras / adelante,
# s es play. WHEELTRACE=1 cuenta paquetes, ordenes e interrupciones.
```

---

## 18. La puerta que faltaba: los bits de habilitación de capa, y el menú que sí vuelve (10 de agosto de 2026)

Los dos síntomas que quedaban abiertos en el §17.7 —**MENU no vuelve atrás** y
**el panel derecho no sigue al resaltado**— tenían **una sola causa**, y estaba
en nuestro modelo, no en el firmware de Apple:

> `ipod_classic_disp_compose()` dibujaba **toda capa que tuviera dirección de
> origen**. El huésped, al terminar una transición, **apaga la capa pero deja
> el descriptor entero puesto**. Desde el primer cambio de pantalla, la **capa
> 2** —320×240 en (0,0), origen `0x0bfb4ff8`— se quedaba encima de la capa 0
> **para siempre**, y tapaba la interfaz completa. Las teclas funcionaban; lo
> que hacían no se veía.

Con la puerta puesta, el retailOS **se navega**: menú principal en el idioma
elegido, la rueda mueve el resaltado **y el panel derecho lo sigue**, SELECT
entra en Música y **MENU vuelve al fotograma exacto del que se salió**.

### 18.1 La medida que lo destapó, y por qué ninguna anterior podía

Las investigaciones anteriores midieron **lo que la pantalla enseñaba**. Ésta
miró **lo que el huésped había dibujado**. Con la pantalla mostrando la nota
musical y «No Music» sobre un degradado liso —sin lista y sin barra de título—:

```bash
(qemu) pmemsave 0x08000000 0x04000000 "/tmp/dram.bin"
# los dos búferes que la capa 0 alterna, sacados del volcado:
python3 tools/ipod_classic/ppm2png.py fb0.bin fb0.png --raw 320x240:argb8888
```

`0x09767a24` y `0x089d42d4` contenían **el menú principal completo**: barra
«iPod» con la batería, la lista (Music, Videos, Photos, Podcasts, Extras,
Settings, Shuffle Songs) con Music resaltado y su flecha, y el panel derecho
con la nota y «No Music».

Y `0x0bfb4ff8`, el origen de la **capa 2**, contenía exactamente el fotograma
que se veía: el degradado con la nota y el texto, **sin lista y sin barra de
título**.

O sea: **el huésped dibujaba bien; nosotros tapábamos.** Eso convierte «MENU no
hace nada» en «MENU sí hace, y no se ve», que es otra avería y de otro sitio.
La trampa nº 4 del método —contar accesos sin mirar qué valores llevan— tiene
aquí su hermana: **mirar la pantalla sin mirar el búfer**.

### 18.2 Dónde está la puerta, con dos pruebas independientes

**1. El escritor, desensamblado.** `0x081271c8` recibe un índice en `r1` y un
booleano en `r2`, salta por la tabla de `0x081271d8` y hace `bic` / `orrne`
sobre la palabra que leyó de `+0x008`, guardándola en `0x08127250`:

| índice | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| bit | `0x40` | `0x20` | `0x10` | `0x08` | `0x04` | `0x80` |

**2. El orden de las escrituras, trazado con `DISPTRACE=1`.** Siempre igual: se
programa el descriptor, se escribe la **dirección de origen**, y sólo entonces
se enciende el bit:

```
+0x060 = 0x089d42d4  (capa 0)  ...  +0x008: 0x41118101 -> 0x41118141   bit 6
+0x078 = 0x0be2326c  (capa 1)  ...  +0x008: 0x41118141 -> 0x41118161   bit 5
+0x090 = 0x0bfb4ff8  (capa 2)  ...  +0x008: 0x41118161 -> 0x41118171   bit 4
```

y al acabar la transición los apaga —`0x41118151`, luego `0x41118141`— **sin
tocar ni la dirección de origen ni el tamaño**. Una pantalla en reposo se queda
en `0x41118141`: **la capa 0 sola**, que es justo la que lleva el fotograma
bueno.

**VERIFICADO** por las dos vías y por el resultado: componer sólo la capa 0
enseña el menú principal, que es lo que el huésped había dibujado.

No confundir con los **bits 16, 20 y 24**, que también parecen uno por capa y
que la versión anterior de `ipod_classic_disp.c` descartó **con razón**: ésos se
ponen una vez en el init y no se mueven nunca. La puerta son los bits 4..6, que
se mueven todo el rato. El comentario de entonces decía «y nada más la
condiciona»; era esa frase la que estaba mal, no el descarte.

### 18.3 El arreglo

`hw/arm/ipod_classic/ipod_classic_disp.c`, en `disp_draw_layer()`:

```c
if (k < S5L8702_DISP_NLAYER_GATED &&
    !(s->reg[S5L8702_DISP_CONTROL >> 2] & S5L8702_DISP_LAYER_EN_BIT(k))) {
    return false;
}
```

con `S5L8702_DISP_LAYER_EN_BIT(k) = 1u << (6 - k)`. Las capas 3 y 4 **no** se
filtran: no sabemos qué bit las gobierna, ningún arranque ha escrito jamás sus
descriptores, y por eso caen antes en la comprobación de `src == 0`. Está
anotado en el código.

### 18.4 Y entonces, ¿qué pasaba de verdad con MENU?

Dos cosas distintas, y hay que separarlas.

**En la pantalla de idiomas del primer arranque, MENU no debe hacer nada, y eso
lo escribió Apple.** Herramienta nueva, `tools/ipod_classic/uirules.py`, que lee
la tabla de reglas de la interfaz directamente del binario:

```
$ python3 tools/ipod_classic/uirules.py assets/classic6g/osos_decrypted.bin \
      --grep button.menu.up --raw
0073e628 SettingsMenus_ResetAllSettings_Language_Layout  button.menu.up1 ->
         (no handler)  (no actions)
```

Manejador de longitud 0, cero acciones: una regla **vacía**. En toda la imagen
hay **108 reglas** `button.menu.up`; **62 hacen algo y 46 no**. Medir MENU ahí
no mide nada. **VERIFICADO** leyendo los bytes a mano en
`0x0073e620..0x0073e71d` antes de escribir el analizador, y el formato entero
está documentado en la cabecera de `uirules.py`.

**En cualquier pantalla que sí tenga a dónde volver, MENU vuelve.** Medido en el
§18.6.

### 18.5 Con el panel derecho, lo mismo

La regla que el §17.7 daba por incumplida,
`list.pid.….delayedselected1 -> ShowSetting_Language`, **no es de esta
pantalla**: es de `list.pid.229444534`, el elemento «Idioma» del **menú de
Ajustes**. La lista del primer arranque es `list.pid.229444595`, que aparece
**una sola vez en los 9,9 MB de la imagen** y **sólo** con `.chosen1`:

```
$ uirules.py osos_decrypted.bin --grep 229444595 --raw
0073e6be SettingsMenus_ResetAllSettings_Language_Layout
         list.pid.229444595.chosen1 -> HandleLanguageAfterReset
         navigator.PopToMainScreen()
```

No hay **ninguna** regla que diga que el panel de «Welcome» siga al resaltado.
**VERIFICADO**, búsqueda exhaustiva.

Donde el firmware **sí** lo dice es en el **menú principal**: 48 reglas
`list.pid.2294433NN.delayedselected`, 24 de ellas con
`navigator.SwitchLayout(MainMenus_Main_Screen_XXX)` y el resto con manejadores
`Handle…Hilited`. Y ahí, ahora, **lo sigue**: un clic de rueda mueve la barra
unos 7 950 px en la lista **y** repinta unos 7 200 px del panel, que pasa de la
nota musical de «ミュージック» a la tira de película de «ビデオ». Mirado, no
sólo contado.

Y de paso: `delayedselected` es la mitad **temporizada** de la interfaz, la que
el §17.7 sospechaba muerta. Está viva, y ahora se ve.

### 18.6 Lo que se ve ahora, medido y mirado

`sh tools/ipod_classic/navtest.sh`, ampliado para llegar al menú principal y
probar MENU allí:

```
00-idle               75463 px encendidos
01-idle-again         75463 px encendidos        0 px distintos (0 en la lista, 0 en el panel)
02-wheel-fwd          75432 px encendidos     8000 px distintos (8000 en la lista, 0 en el panel)
03-wheel-fwd2         75463 px encendidos     8428 px distintos (8428 en la lista, 0 en el panel)
04-wheel-back         75432 px encendidos     8000 px distintos (8000 en la lista, 0 en el panel)
05-select             76074 px encendidos    19503 px distintos (12842 en la lista, 6661 en el panel)
06-menu-idle          76074 px encendidos      875 px distintos (0 en la lista, 875 en el panel)
07-menu-wheel         76046 px encendidos    15028 px distintos (7952 en la lista, 7076 en el panel)
08-menu-wheel-back    76074 px encendidos    15151 px distintos (7975 en la lista, 7176 en el panel)
09-into-music         75802 px encendidos     7045 px distintos (6088 en la lista,  957 en el panel)
10-menu-back          75802 px encendidos      933 px distintos (0 en la lista, 933 en el panel)   <- MENU se perdio en ESTA carrera

  suelo de la pantalla de idiomas: 0 px (limite 200)
  suelo de la LISTA del menu principal: 0 px (limite 200)

VEREDICTO
  02-wheel-fwd           8000 px  -> el resaltado se movio
  03-wheel-fwd2          8428 px  -> el resaltado se movio
  04-wheel-back          8000 px  -> el resaltado volvio
  05-select             19503 px  -> SELECT cambio la pantalla
  07-menu-wheel        lista  7952 px + panel  7076 px  -> el resaltado se movio Y el panel lo siguio
  08-menu-wheel-back   lista  7975 px + panel  7176 px  -> el resaltado se movio Y el panel lo siguio
  09-into-music          7045 px  -> SELECT entro en Musica
  11-menu-at-top       lista     0 px + panel   875 px  -> no navego, que es lo que toca
```

Y **mirado**, que es la otra mitad de la medida:

- `05-select`: el **menú principal** en japonés —ミュージック, ビデオ, 写真,
  Podcast, エクストラ, 設定, 曲をシャッフル— porque 日本語 era el idioma
  resaltado. Es la misma prueba de «hizo lo que se le pidió» que ya daba
  SELECT. En otra carrera, con **Čeština** resaltado, salió *Hudba, Videa,
  Fotografie, Podcasty, Dodatky, Nastavení, Náhodná hudba* y el panel dijo
  *Žádná*.
- `07-menu-wheel`: la barra sobre **ビデオ** y el panel derecho con la **tira de
  película**.
- `09-into-music`: la pantalla de **Música** (Cover Flow, プレイリスト,
  アーティスト, アルバム, コンピレーション, 曲, ジャンル, 作曲者,
  オーディオブック).
- `10-menu-back`: **el menú principal otra vez**, y el mismo fotograma que
  `08-menu-wheel-back`.

El veredicto fuerte no es el tamaño del cambio sino **a dónde va**:
`10-menu-back` se compara contra `08-menu-wheel-back`, el fotograma del que se
salió. «Cambiaron muchos píxeles» no demuestra que se vuelva; «volvió al
fotograma del que salimos» sí.

### 18.7 El suelo de ruido: dos cosas se mueven solas, y las dos deben

`navtest.sh` suspendía la carrera si dos capturas seguidas de una pantalla
quieta diferían en algo. Ya no vale tal cual, y las dos razones están medidas:

1. **El icono de batería**: **23 px** en `x=145..151, y=4..13`, con el verde de
   la carga entre los colores. Se repinta cuando le parece: una carrera de este
   script midió 0 px ahí y la siguiente 23.
2. **La etiqueta del panel del menú principal**: unos **960 px** en
   `x=184..293, y=165..182`, o sea las **dos líneas de texto bajo el icono**.
   La etiqueta japonesa 「ミュージックがありません」 no cabe en una línea y **se
   desplaza sola**. La mitad izquierda de esos mismos fotogramas difiere en
   **0 px**.

Así que el suelo se comprueba contra un **límite de 200 px** en vez de contra
cero, se **imprime siempre** el valor medido, y el menú principal se enrasa
**sólo por su lista**. Mover el resaltado cuesta 8 000 px, cuarenta veces el
límite, y el umbral más bajo de todo el veredicto pide 3 000: no hay solape.

### 18.8 La otra mitad del problema: un botón que se enviaba UNA vez, y una pérdida que sigue abierta

Con la pantalla ya visible salió a la luz algo que antes no se podía ni medir:
**SELECT y MENU se pierden a veces**. Prueba dedicada —entrar en Música y salir,
cinco veces, una pulsación cada 13 s, `~/6g/inout`—:

```
carrera 1 (modelo de entonces)   in1 6343 | out1    0 | in2    0 | out2 6343
                                 in3    0 | out3    0 | in4 6343 | out4 6343
                                 in5    0 | out5    0
WHEELTRACE totals: 23 packets queued, 0 dropped, 1 commands, 22 rising edges
```

**23 paquetes es exactamente lo esperado** —1 del sondeo de arranque + 11
pulsaciones × 2 flancos— y **0 descartados**. El modelo entregó todo y el
huésped hizo caso a cuatro de ocho pulsaciones con destino. Nada en el modelo
distingue las que funcionaron de las que no.

**Lo que sí se encontró y se ha arreglado** es la *constraint 2* del §17.3
aplicada a los botones, que era la mitad que se había quedado exenta:

> El ISR sobrescribe el mismo bloque de estado en cada paquete. Una pulsación
> eran **exactamente dos paquetes**, uno con el bit puesto y otro con el bit
> quitado. Y el retailOS actúa **al soltar** —sus reglas se llaman
> `button.menu.up` y `button.select.up`—, así que necesita que la tarea de
> rueda corra y vea el bit **puesto**, y vuelva a correr y lo vea **quitado**.
> Si la tarea no corre en ese hueco, la pulsación entera desaparece sin dejar
> rastro.

El aparato de verdad no tiene ese problema porque **transmite**: mientras algo
pasa informa a su ritmo, y el estado de los botones va en **todos** los
paquetes. Ahora el modelo también (`S5L8702_WHEEL_BTN_REPEAT_MS`, 400 ms, diez
ticks; ajustable con `IPOD_WHEEL_BTN_REPEAT_MS`): repite el estado un paquete
por tick mientras hay un botón pulsado y durante la ventana posterior a cada
flanco, para que el de subida reciba el mismo trato que el de bajada.

Dos carreras más, ya con la repetición:

```
carrera 2   in1 0 | out1 0 | in2 0 | out2 0 | in3 0 | out3 0
            in4 6343 | out4 6343 | in5 6343 | out5 6343      (134 paquetes, 0 descartados)
carrera 3   in1 6343 | out1 6665 | in2 0 | out2 322 | in3 6343 | out3 6343
            in4 6343 | out4 6343 | in5 0 | out5 0            (134 paquetes, 0 descartados)
```

Contando **sólo las pulsaciones que tenían a dónde ir**:

| | SELECT desde el menú principal | MENU desde la pantalla de Música |
|---|---|---|
| sin repetición (carrera 1) | 1 de 3 | 2 de 3 |
| con repetición (carreras 2 y 3) | 5 de 10 | **5 de 5** |

**Lo honesto es decir las dos cosas.** La repetición **elimina un modo de fallo
que el modelo tenía demostradamente** —dos paquetes por pulsación contra un ISR
que sobrescribe su bloque de estado— y **MENU no ha fallado ni una vez** desde
que está. Pero **no arregla la pérdida de SELECT**, que sigue ahí, y por tanto
**la pérdida tiene otra causa que no está encontrada**. No se vende como
arreglada.

**El sospechoso número uno para lo que queda**, y es donde miraría el siguiente:
la puerta del §17.4. El despachador de teclas lee por I2C el registro `0x87` del
PMU **en cada tecla** y **se vuelve sin hacer nada** si el bit sale distinto de
cero (`0x0803f3e0` → `0x08294f80`, y el `popne` de `0x0803f3e4`). Una lectura
que devuelva otra cosa —el modelo del PMU tiene un puntero de registro que
autoincrementa y se comparte entre transacciones, `ipod_classic_pmu.c:131`— tira
la tecla en silencio y con exactamente esta pinta. El experimento: punto de
ruptura en `0x0803f3e4` con `evtrace.py`, leer `r0` en cada pulsación y ver si
las perdidas son las que traen `r0 != 0`. **No está hecho.**

Consecuencia práctica inmediata: `navtest.sh` puede suspender por esto sin que
nada se haya roto. Si suspende **sólo** en `09-into-music`, repítelo antes de
buscar culpables.

### 18.9 Una trampa de método que costó dos carreras: los QEMU huérfanos

Dos carreras seguidas de `navtest.sh` fallaron con la pantalla en negro y
**116 y 68** líneas de `guest asked for a software reset (SWRCON = 0xaa5),
next image "diskmode"` en `stderr`. No era el parche. Era esto:

```
$ ps aux | grep qemu
... 24 procesos qemu-evt de una sesion anterior, al 80 % de CPU cada uno
$ uptime
load average: 16.00     # en 12 nucleos
```

`navtest.sh` hace `pkill -9 -x qemu-system-arm`, que por diseño **no** toca un
binario llamado de otra forma —la copia privada `~/evt/qemu-evt` que usa
`evtrace.py` existe justamente para que una sesión no mate a otra—. El precio
es el simétrico: **los huérfanos no se limpian solos**. Con la máquina así de
cargada el huésped no llega a tiempo a sus propios plazos de arranque y se
reinicia a *diskmode* en bucle.

Antes de creerse un fallo intermitente: `ps aux | grep qemu` y `uptime`.

**Pero no todo es carga, y esto es nuevo.** Con la máquina vacía —carga 0,4, un
solo QEMU, 7 GB de memoria libre— dos carreras seguidas se reiniciaron **1 y 3
veces**, y el reinicio cae **entre los 48 y los 52 s**, o sea **antes de la
primera tecla**. La huella en las capturas es exactamente la que la
investigación de eventos describió sin nombre: donde debería estar la lista de
idiomas aparece la pantalla **Low Battery** (76 797 px) y poco después **negro**
(0 px). Ahora tiene nombre, porque `stderr` lo dice:

```
qemu-system-arm: warning: S5L8702: guest asked for a software reset
                 (SWRCON = 0xaa5), next image "diskmode"
```

O sea: **la pantalla «Low Battery» de los 46 s y el reinicio a modo disco son
el mismo suceso**. Sigue sin explicación —el ADC del PMU devuelve 844 de 1023,
que no es poca batería (`ipod_classic_pmu.c:47`), así que el disparador es otra
cosa—, pero ya no es un misterio sin bordes: es un camino concreto del huésped
que decide reiniciarse a `diskmode`, y por dónde empezar es por qué registro del
PMU lee antes de decidirlo.

Por eso `navtest.sh` cuenta ahora esos reinicios y **avisa a gritos** cuando hay
alguno: una carrera que se reinicia tiene todos los tiempos corridos y su
veredicto no vale para nada. Repítela.

### 18.10 Regresiones comprobadas

| Prueba | Resultado |
|---|---|
| `navtest.sh`, la mitad de siempre: los tres giros en la pantalla de idiomas | **8 000, 8 428 y 8 000 px**, y el suelo en **0 px**. Las mismas cifras del §17.8 (los 428 de más son el icono de batería del §18.7). Sin regresión |
| `navtest.sh`, la mitad nueva | el menú principal aparece, sus dos mitades se mueven juntas, SELECT entra en Música y MENU vuelve al mismo fotograma. Ver §18.6 |
| **`tools/ipod_video/navtest.sh` (el 5G)** | **76 548 px en los siete fotogramas y el resaltado se mueve: 12 312, 12 114 y 31 598.** Los dos primeros, clavados a los del §17.8; el tercero cae en el «~31 600» que esa misma tabla ya registraba como no reproducible al píxel porque esa pantalla tiene reloj. **Sin regresión** |
| retailOS **sin disco** | **0 px**, `CPU_exception_handler` y el reinicio por software de siempre: el ciclo de caída del §4, igual que antes. El compositor no compone porque nadie le da dirección de origen, y ahora tampoco porque nadie enciende una capa |
| `tools/ipod_classic/lcdtest.S` | no programa ninguna capa, así que la puerta no decide nada para él: sigue pintando por el puerto de datos del panel |
| Migración | `vmstate_ipod_classic_wheel` pasa a versión 2 al añadir `button_repeat_until`. Nada migra esta máquina hoy, así que el formato viejo no se mantiene vivo |

### 18.11 Lo que sigue sin saberse

- **El byte bajo de `LAYERn_CFG`** (`+0x0d8`, `+0x0e0`, `+0x0e8`) vale `0xff` en
  las capas 0 y 1 y **`0x00`** en la capa 2, en las 106 escrituras de una
  carrera. Parece una **alfa constante**, y si lo es, la capa 2 es invisible
  incluso mientras su bit de habilitación está puesto. **No se modela**: el bit
  ya explica el síntoma entero, y esto sólo afectaría a los fotogramas **de
  mitad de transición**. **INFERIDO**, sin resolver. Si algún día una
  transición se ve tapada un instante, es esto.
- **Los campos de 4 bits de `LAYERn_CFG` en `[31:28]` y `[15:12]`**, que
  `0x081278fc` escribe en parejas `(1,0)`, `(0,1)`, `(5,4)`, `(9,8)`, `(11,10)`:
  parecen modos de mezcla. **DESCONOCIDOS**, y no hacen falta.
- **Los índices 3, 4 y 5 del escritor de `CONTROL`** (bits `0x08`, `0x04`,
  `0x80`): sólo se ve usado el 5, dos veces en una carrera entera.
  **DESCONOCIDOS**.
- **Las capas 3 y 4**: nadie las programa y no se les conoce bit.
- **El byte de sufijo de cada disparador** en la tabla de reglas: es ASCII y
  vale `'1'` en las 1 380 reglas de la imagen. **DESCONOCIDO** para qué es.
- **La pantalla «Low Battery» intermitente** de los 46 s sigue sin explicación
  propia; ver la sospecha del §18.9.

---

## 19. Quinta auditoría independiente: el temporizador, medido de una vez (10 de agosto de 2026)

La tanda del §18 dejó `tools/ipod_classic/tests/timer_audit.c` en el árbol **sin
escribir en ninguna parte lo que la prueba contesta**. Esta sección lo arregla:
la prueba se ejecutó, dice cinco cosas, y **tres de ellas son fallos del
modelo**.

```
$ sh tools/ipod_classic/tests/build.sh
$ ./build/qemu-system-arm -M iPod-Classic -kernel assets/tests/timer_audit.bin \
      -display none -monitor none -serial stdio
```

### 19.1 La hipótesis de partida queda REFUTADA, y con la medida en la mano

La orden de trabajo traía una sospecha fuerte heredada de una auditoría
anterior: «`TBCNT` no avanza en el modelo, lee `0x1` dos veces», y de ahí que
cualquier cosa **retardada** —`delayedselected`, `TimerEventManager`— se quedase
esperando para siempre. **No es cierto.** Los tres contadores libres avanzan, y
**a la frecuencia correcta**, que es la mitad que de verdad importa:

| contador | programado | medido en 200 ms | esperado | error |
|---|---|---|---|---|
| `TECNT` | 1 MHz (`ECLK/12`) | 200 091 | 200 000 | +0,05 % |
| `TFCNT` | 12 MHz (`ECLK/1`) | 2 401 271 | 2 400 000 | +0,05 % |
| `TGCNT` | 10 kHz (`ECLK/16/75`) | 2 001 | 2 000 | +0,05 % |

Y `tools/ipod_classic/tests/timer_irq.c`, el que dio pie a la sospecha, hoy
imprime `counter runs: yes 0x00000003 0x00000004`.

**Y no es un contador cualquiera: es el que usa el retailOS.** Buscando el
literal de la base del bloque en la imagen (`findlit.py ... 3C700000`) salen
tres sitios, y dos son el temporizador E:

```
0x08324e48   TECON   = 0x440        (ECLK, /1)
             TEPRE   = 11           -> 12 MHz / 12 = 1 MHz
             TEDATA0 = 0xffffffff   (libre)
             TECMD   = 3            (CLR | EN)
0x08324e38   ldr r0, [r0, #0xb4]    <- lee TECNT, y no hace nada mas
```

O sea: el retailOS se fabrica **exactamente el `USEC_TIMER` de Rockbox**
(`clocking-s5l8702.c:401`) y lo lee con una función de una instrucción. Ese
contador corre bien. La hipótesis del temporizador queda **descartada con medida
directa sobre el contador que el huésped consulta de verdad**, no por parecido.

### 19.2 Fallo real 1: un `ack` reiniciaba la cuenta, y costaba el 14 % del reloj

En los temporizadores de 16 bits los bits de estado viven **en `CON`**, así que
acusar recibo obliga a escribir `CON` —`TBCON = TBCON`, que es literalmente lo
que hace `INT_TIMERB` de Rockbox (`kernel-s5l8702.c:31`)—. Y en el modelo
**toda** escritura de `CON` llamaba a `chan_reload()`, que termina en
`ptimer_set_limit(..., reload = 1)`: **la cuenta volvía arriba**. En la máquina
de verdad sólo `TB_CLR` de `CMD` inicializa el contador.

Consecuencia: cada periodo salía **una latencia de interrupción más largo**.

```
ack TBCON=TBCON: TBCNT 3289 -> 139   COUNTER WAS RESET
timer B interrupts in 2000042 us: 1713   expect 2000
timer F interrupts in 2000040 us: 1997   expect 2000 (ack via TSTAT, CON untouched)
```

La tercera línea es **el experimento de control**, y es lo que convierte esto en
una prueba en vez de una sospecha: el temporizador F tiene el mismo periodo, el
mismo VIC y el mismo manejador, y se diferencia **sólo** en que su estado vive
en `TSTAT` y su `ack` no toca `CON`. F mantuvo la hora; B no.

**ARREGLADO** en `ipod_classic_timer.c`: el caso `CON` sólo recarga si los bits
de **configuración** —fuente de reloj, modo, habilitación de interrupciones—
cambian de verdad. Una escritura que sólo limpia estado deja la cuenta en paz.

```
ack TBCON=TBCON: TBCNT 3364 -> 3513   kept counting
timer B interrupts in 2000047 us: 1999   expect 2000
```

**Sin regresión, y con la razón de por qué:** el retailOS **no usa** los
temporizadores de 16 bits —sus únicos usos del bloque son el temporizador E de
arriba y una lectura suelta de `+0x08` dentro de una función que suma bytes—,
así que este arreglo le es inerte, y `navtest.sh` da después las mismas cifras
al píxel que antes (§19.5). Quien lo cobra es Rockbox y cualquier prueba
desnuda.

### 19.3 Fallo real 2: `TICNTH`/`TICNTL` no están decodificados

`s5l87xx.h:782` pone un contador libre de 64 bits en `TIMER_BASE+0x80/0x84`, y
`s5l87xx.h:793` lo llama `FIVE_USEC_TIMER`. `decode_offset()` cubre A..D en
`0x00..0x7f` y E..H desde `0xa0`: **`0x80` y `0x84` caen en el hueco**, así que
se registran como «bad offset» y **leen cero para siempre**.

```
TICNTH/L 0x00000000/0x00000000 -> 0x00000000/0x00000000  FROZEN
```

Esto sí es, literalmente, la avería que la orden de trabajo describía —un
contador de tiempo que no corre— pero **no le pasa al retailOS**: no hay ni un
literal `0x3c700080` ni `0x3c700084` en los 9,9 MB de la imagen. Le pasaría a un
huésped que girase sobre `FIVE_USEC_TIMER`, que se quedaría colgado para
siempre.

**NO ARREGLADO, a propósito.** Añadir el registro es fácil; saber qué vale al
reiniciar, si se puede parar y a qué frecuencia va exactamente no lo dice
ninguna fuente que tengamos, y **un contador inventado que corre al ritmo
equivocado es peor que uno que no está**, porque el segundo se nota y el primero
no.

### 19.4 Fallo real 3: escribir `DATA0` o `PRE` también reinicia la cuenta

Mismo origen que el §19.2 y misma discrepancia con la máquina real:

```
rewrite TBDATA0: TBCNT 3872 -> 403   COUNTER WAS RESET
rewrite TBPRE:   TBCNT 5411 -> 119   COUNTER WAS RESET
```

**NO ARREGLADO**, y la razón está anotada en `chan_reload()`: ni Rockbox ni el
retailOS escriben `DATA0` o `PRE` con el temporizador en marcha —los escriben
parados, justo antes de `CMD_CLR`—, así que no hay nada medible que ganar, y
diferir el límite rompería el caso contrario (un controlador que ponga `DATA0` y
luego `EN` sin `CLR` y necesite el límite nuevo ya vivo).

### 19.5 Cuatro carreras de `navtest.sh`, y lo que dicen de verdad

Dos antes del arreglo del temporizador y dos después. **Las cifras no se mueven
al píxel**, que es lo que se esperaba del §19.2.

| carrera | temporizador | veredicto |
|---|---|---|
| 1 | antes | **pierde SELECT en `09-into-music`** (875 px). Todo lo demás pasa |
| 2 | antes | **pasa entera.** `10-menu-back` a **898 px** de `08-menu-wheel-back` |
| 3 | después | **pasa entera.** `10-menu-back` a **0 px** de `08-menu-wheel-back` |
| 4 | después | **pierde SELECT en `05-select`** (0 px), y con eso arrastra toda la segunda mitad |

Las cifras de la carrera 3, que es la buena y la de después: giros **8 000 /
8 000 / 8 000** px sobre suelo **0 px**, `05-select` **19 340** px, `07` y `08`
**lista 7 952 + panel 7 176** px, `09-into-music` **6 963** px, `10-menu-back`
**6 963** px y **0 px de diferencia** con `08-menu-wheel-back`, `11-menu-at-top`
**0 px** en la lista.

**Mirado en PNG, no sólo contado**, que es la otra mitad de la medida:

- `08-menu-wheel-back`: el menú principal, barra «iPod», **ミュージック**
  resaltado con su flecha, y el panel con la nota musical.
- `09-into-music`: la pantalla de **Música** —barra ミュージック, **Cover Flow**
  resaltado, y debajo プレイリスト, アーティスト, アルバム, コンピレーション,
  曲, ジャンル, 作曲者, オーディオブック—. SELECT entró de verdad.
- `10-menu-back`: **otra vez el menú principal**, barra «iPod», ミュージック
  resaltado. Lo único que lo separa de `08` es la etiqueta que se desplaza sola
  en el panel. **MENU vuelve, y vuelve a la pantalla de la que se salió.**

Y **`stderr` de esa carrera son dos líneas**: la carga de la imagen y el primer
`SOURCE ADDRESS`. Ni una excepción, ni un reinicio por software. *Volver y morir
no es volver*, y aquí no murió.

Otras regresiones:

| Prueba | Resultado |
|---|---|
| `tools/ipod_video/navtest.sh` (el 5G) | **76 548 px** en los siete y **12 312 / 12 114 / 31 629**. Los dos primeros clavados a los del §17.8; el tercero dentro del «~31 600» que esa pantalla mueve por su reloj. **Sin regresión** |
| `timer_audit.bin` y `timer_irq.bin` | ambas pasan; las cifras están arriba |

**La trampa del §18.9 volvió a cobrar, y con un nombre nuevo.** Dos carreras más
salieron con las capturas en pantallas equivocadas y **3 y 1 reinicios por
software**. No era el parche: era un proceso `ipod-classic-run` —una copia
privada del binario, con otro nombre— al **93 % de un núcleo** durante veinte
minutos. `pkill -9 -x qemu-system-arm` no lo ve, igual que no ve `qemu-evt`, y
por el mismo motivo deliberado: que una sesión no mate a otra.

Lo que sí valió: **el aviso de reinicios que el §18 metió en `navtest.sh`
funcionó las dos veces** y evitó que esas cifras se leyeran como una regresión
del temporizador. Se le añade ahora el aviso que faltaba —**contar procesos tipo
QEMU antes de arrancar y decirlo**—, porque el que había sólo se enteraba
*después*, cuando la carrera ya se había gastado. No los mata: pueden ser el
trabajo en curso de otra sesión.

### 19.6 La pérdida de SELECT es más ancha de lo que decía el §18.8

El §18.8 y la cabecera de `navtest.sh` avisaban de que la carrera puede
suspender **en `09-into-music`**. Medido aquí: **también se pierde el primer
SELECT, el de la pantalla de idiomas** (carrera 4, `05-select` = 0 px, sin
reinicio ni excepción en `stderr`). Y cuando se pierde ése, **arrastra toda la
segunda mitad de la prueba**, porque las capturas siguientes caen en la pantalla
de idiomas: `07`/`08` siguen dando ~8 000 px en la lista y **0 en el panel**, que
es exactamente lo correcto para esa pantalla (§18.5) y parece un fallo del panel
si no se mira dónde se está.

Recuento de esta auditoría, contando sólo pulsaciones con destino:

| | aciertos |
|---|---|
| SELECT (`05` y `09` juntos) | **5 de 7** |
| MENU con sitio a donde volver (`10`) | **2 de 2** |
| MENU en lo alto de la pila (`11`), donde lo correcto es no hacer nada | **4 de 4** |

La conclusión del §18.8 se mantiene y se refuerza: **la repetición del estado de
botones no arregló la pérdida, y la pérdida no distingue pantalla**. La cabecera
de `navtest.sh` queda corregida para decir que puede suspender en `05-select`
igual que en `09-into-music`.

### 19.7 Lo que se confirma de la tanda del §18, comprobándolo aparte

- **La puerta de las capas es real y los bits son los que dice.** Desensamblado
  otra vez desde cero, con la base de carga del §1 (`0x08000000 + (offset -
  0xAE48)`; con la base ingenua `0x08000000` se cae en un *thunk* de C++ y
  parece que la evidencia esté inventada): `0x081271c8` carga `0x38900000` del
  pozo de literales de `0x081272ac`, lee `[r3, #8]`, salta por
  `cmp r1, #5 / addls pc, pc, r1, lsl #2`, y las seis ramas hacen `bic`/`orrne`
  con `0x40, 0x20, 0x10, 0x08, 0x04, 0x80` y guardan en `0x08127250`.
  **VERIFICADO instrucción a instrucción.**
- **El orden de escrituras también.** En una carrera propia: `+0x060 =
  0x089d42d4` y después `+0x008: 0x41118101 -> 0x41118141` (bit 6); `+0x078 =
  0x0be2326c` y después `-> 0x41118161` (bit 5). **VERIFICADO.**
- **El descriptor de la capa 2 es el que se quedaba encima.** `+0x094 =
  0x014000f0` (320×240), `+0x09c = 0` (en el origen), `+0x090 = 0x0bfb4ff8`,
  escrito **una sola vez en toda la carrera** y nunca borrado. **VERIFICADO.**
- **La regla vacía de MENU es de esa pantalla, y ahora por una razón mejor que
  la etiqueta.** `uirules.py` avisa de que el nombre de la izquierda es «el más
  cercano hacia atrás», y hace bien: bajo la misma etiqueta salen también
  `button.menu.up1 -> HandleExit` (`0x0073e774`) y una regla de **cronómetro**
  (`button.playpause.up1 -> Stopwatch_Screen_Running`), que no puede ser de una
  pantalla de idiomas. Leyendo los bytes, los grupos son `u32 count;
  RULE[count]`, y el `count = 3` que empieza en `0x0073e624` contiene **la regla
  vacía de MENU, `controller.GotoScreen_Language1` y
  `list.pid.229444595.chosen1`**: las tres de la pantalla de idiomas.
  `HandleExit` cae en el grupo **siguiente**, de `count = 5`, que es el del
  cronómetro. La conclusión del §18.4 se sostiene, pero **la prueba buena es la
  pertenencia al grupo, no la etiqueta**.
- Los recuentos de `uirules.py` salen: **1 380** reglas, **108**
  `button.menu.up` de las que **46** no tienen ni manejador ni acciones, y
  `list.pid.229444595` aparece **una sola vez** y sólo con `.chosen1`.

### 19.8 Una corrección menor de anotación

`S5L8702_WHEEL_BTN_REPEAT_MS` se presentaba apoyado en «el aparato de verdad
transmite», cuando dos constantes más arriba en el mismo fichero dicen que **el
ritmo de la rueda real es INDETERMINADO**. Lo comprobable es sólo que el estado
de los cinco botones viaja en cada palabra de estado
(`button-clickwheel.c:273`); que la rueda siga informando **después** de soltar
no lo dice ninguna fuente. Los 400 ms quedan marcados como **elección de
modelo**, como sus vecinos.

### 19.9 Lo que sigue abierto

- **La pérdida de SELECT**, §19.6. Sigue sin causa, y el experimento del
  interruptor de bloqueo del PMU del §18.8 **sigue sin hacerse**.
- **`TICNTH`/`TICNTL`**, §19.3.
- **La recarga en `DATA0`/`PRE`**, §19.4.
- **Los modos de `CON[MODE]` distintos de 0**, que se registran como no
  modelados y se tratan como intervalo.
- Todo lo del §18.11, que sigue igual.
