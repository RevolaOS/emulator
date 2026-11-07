# Building and running on this machine

## Environment

- Repo on Windows: `C:\Desarrollo\qemu-ios` (branch `ipod_classic_6g`)
- Build in **WSL Ubuntu-24.04**, accessing it via `/mnt/c/Desarrollo/qemu-ios`
- QEMU base: 8.2.0
- 12 cores available

Building happens in WSL and editing on Windows, on the same tree. Building on
`/mnt/c` is slower than on WSL's native filesystem, but it avoids having
two copies.

## Dependencies

```bash
sudo apt install -y libsdl2-dev libssl-dev libglib2.0-dev libpixman-1-dev \
                    ninja-build python3-venv \
                    dosfstools mtools
```

Already installed: glib 2.80.0, pixman 0.42.2, SDL2 2.30.0, OpenSSL 3.0.13.
Verified on 2026-08-09, together with gcc 13.3.0, Python 3.12.3, ninja 1.11.1 and
meson 1.3.2 (in `build/pyvenv/`).

`dosfstools` and `mtools` are **not** for QEMU: `mkdisk.sh` needs them to
build the FAT32 of the disk image. `python3` is used by `navtest.sh`,
`appletest.sh`, `fwtool.py`, `extract-vc.py` and `mkdisk.sh` itself.

## Trap: CRLF line endings

`git clone` on Windows applies `core.autocrlf=true` by default and converts
**all** text files to CRLF, including `configure`. The result from
WSL is:

```
bash: line 1: ../configure: cannot execute: required file not found
```

...which is the shebang `#!/bin/sh\r`. Fix, already applied to this repo:

```bash
git config core.autocrlf false
git config core.eol lf
git rm --cached -r . -q
git reset --hard
```

### The trap is not fully closed

`core.autocrlf=false` stops **git** from converting, but **it does not stop a
Windows editor from writing CRLF**. Counted on 2026-08-09, **three tracked
files were 100 % CRLF**, violating the repo's own `.editorconfig`
(line 16: `end_of_line = lf`):

| File | Situation |
|---|---|
| `hw/arm/meson.build` | **From upstream QEMU.** It was LF up to `fb7c1039a` and became CRLF in `5b5af8966`. Result: **adding a single line to the source list produced a diff of 77 insertions + 77 deletions**. **Converted to LF**; verified that the contents do not change |
| `hw/arm/ipod_video/ipod_video.c` | Ours. 1384 lines. Left as it is |
| `include/hw/arm/ipod_video/ipod_video.h` | Ours. 364 lines. Left as it is |

The painful case is the first one: it is an upstream file, so any
rebase or diff against QEMU drags that noise along. Our two are cosmetic and
are left alone so as not to drop a 1,700-line pure-whitespace diff in the
middle of other work; if they are ever converted, it should be in a commit of
their own that does only that.

> **To count them, `grep -c $'\r'` does not work** in Git Bash: it gives false positives.
> This does:
>
> ```bash
> for f in $(git ls-files); do
>   n=$(tr -cd '\r' < "$f" | wc -c)
>   [ "$n" -gt 0 ] && echo "$n CR  $f"
> done
>
> sed -i 's/\r$//' <file>   # convert
> ```

## Trap 2: git symlinks on Windows

The QEMU tree has 7 symlinks, all in `subprojects/libvduse` and
`subprojects/libvhost-user`. Without Developer Mode, git on Windows materializes them
as **text files with the target path inside**. From WSL the compiler
finds an `atomic.h` whose contents are literally
`../../../include/qemu/atomic.h` and fails:

```
subprojects/libvduse/include/atomic.h:1:1: error: expected identifier or '(' before '.' token
```

Fix applied (replace them with real copies and tell git to
ignore them):

```bash
cd /mnt/c/Desarrollo/qemu-ios
git ls-files -s | awk '$1=="120000"{print $4}' | while read -r p; do
  # Already converted (skip-worktree): don't touch it. See the warning below.
  [ "$(git ls-files -v -- "$p" | cut -c1)" = "S" ] && continue
  tgt=$(cat "$p"); real="$(dirname "$p")/$tgt"
  rm -rf "$p"; cp -r "$real" "$p"
done
git ls-files -s | awk '$1=="120000"{print $4}' | xargs git update-index --skip-worktree
```

> **Warning: run this ONCE, on a freshly made clone.** The original
> snippet **was not idempotent and running it again deleted the files**.
> Checked on 2026-08-09 on a copy: once converted, `cat "$p"` no longer
> returns the target path but the 14 KB of C code, so `cp` fails
> with *"File name too long"*… **but the `rm -rf` on the same line does
> run**. And since they are in `skip-worktree`, `git checkout` does not restore
> them either. The `[ ... ] = "S"` guard above is what prevents it.

These subprojects (vhost-user/VDUSE) have nothing to do with emulating an iPod;
they only get in the way.

## Trap 3: `--disable-werror` does not work (configure bug)

QEMU 8.2's `configure`, around line 1750:

```sh
# Only enable by default for git builds and on select OSes
if test -e "$source_path/.git" && \
    { test "$targetos" = linux || test "$targetos" = "windows"; }; then
    echo 'werror = true' >> $cross
fi
```

It writes `werror = true` into meson's **native file**
(`build/config-meson.cross`). A built-in option set there **wins over
`-Dwerror=false`** on the command line, so `./configure
--disable-werror` has no effect at all in a git tree under Linux. You can tell
because `meson configure` shows `werror true` for the main project and
`false` for the subprojects.

The qemu-ios device models do not compile cleanly (unused variables,
`hwaddr` printed with `%x`, shadowed parameters), so the block is
neutralized in this fork. If `-Werror` is ever wanted, request it
explicitly.

Corollary: **after touching `Kconfig` or `meson.build`, ninja reruns
`configure` via `config.status`**, and that is where the problem reappeared even
if the initial build had gone fine.

## Commands

```bash
# Configure (once)
wsl -d Ubuntu-24.04 -e bash -c 'cd /mnt/c/Desarrollo/qemu-ios && mkdir -p build && cd build && \
  ../configure --enable-sdl --disable-cocoa --target-list=arm-softmmu \
               --disable-capstone --disable-slirp \
               --extra-cflags=-I/usr/include/openssl --extra-ldflags=-lcrypto \
               --disable-werror --enable-pie'

# Build
wsl -d Ubuntu-24.04 -e bash -c 'cd /mnt/c/Desarrollo/qemu-ios/build && ninja qemu-system-arm'
```

The binary ends up in `build/qemu-system-arm`.

`make -j10` also works —it delegates to ninja—, but it adds about 8 s of
`meson introspect`; `ninja qemu-system-arm` is the direct route.

### How long it takes, and a trap that looks like a broken build

Measured on 2026-08-09 on this machine, on a clean `build/`:

| Operation | Time |
|---|---|
| `configure` + `ninja` **from scratch** | **16 min** (configure ~3, ninja ~13) |
| `ninja` with nothing to do, **warm** | 21 s |
| `ninja` with nothing to do, **first time after editing from Windows** | **6 min** |
| Recompile one `.c` and link | ~3 min |

> **That six-minute "no-op" is not a broken build: it is the filesystem.** WSL
> has to `stat` the whole tree through 9p, and the first pass
> after touching files from Windows is extremely slow. The second drops to 20 s.
> Without knowing this it is easy to conclude that something is wrong.

## Rockbox reference

Local copy of the S5L8702 sources in `assets/ref/`
(names flattened with `__` instead of `/`). They were downloaded with:

```python
# starting from https://api.github.com/repos/Rockbox/rockbox/git/trees/master?recursive=1
# filtering firmware/target/arm/s5l8702/**, firmware/export/config/ipod6g.h,
# firmware/export/i2c-s5l8702.h and bootloader/ipod-s5l87xx.c
```

The complete register map is in `assets/ref/s5l87xx.h` and its version resolved for the
S5L8702 in `assets/ref/s5l8702_macros.h`.

## Running and testing

### The iPod Video machine

```bash
./build/qemu-system-arm -M "iPod-Video,entry=0x735a00" \
    -kernel assets/images-5g/osos_full.bin \
    -drive if=ide,index=0,file=assets/ipod-video-disk.img,format=raw,file.locking=off \
    -snapshot -display sdl -serial null
```

- `entry=0x735a00` boots the Rockbox bootloader; `entry=0` Apple's
  retailOS. It is the `entryOffset` from the firmware partition's directory.
- `-snapshot` and `file.locking=off` avoid touching the image and allow several
  instances at once. Use them always.
- `-display sdl` opens a window. From WSL it works through WSLg with `DISPLAY=:0`;
  the binary is built with `--enable-sdl`, and `-display help` lists the
  available backends.
- With `remap=on` the DRAM appears at 0 from reset, which is what is needed
  to load the Rockbox application directly (it is linked for
  address 0 and normally relies on the bootloader setting up the mapping first).
  It reaches the menu in ~20 s instead of 28.

  > **Corrected**: this used to say "load a `rockbox.ipod` directly". **It does not
  > work as-is**: its 8-byte header (checksum plus the target id `"ipvd"`)
  > has to be stripped first. Tested on 2026-08-09 — with the header in place
  > the screen stays black, 0 pixels. What boots is
  > `assets/images-5g/rockbox_app.bin`, which is `rockbox.ipod[8:]`.

### Machine properties

`entry`, `remap` and `semihosting` are registered in `instance_init`, so they **do not
appear in `-M iPod-Video,help`**, which only lists class properties.
Anyone who does not read the source or this wiki will not find them.

| Property | Default | What it does |
|---|---|---|
| `entry` | `0` | Offset within the `osos` where execution starts |
| `remap` | `off` | DRAM at address 0 from reset |
| `semihosting` | `on` | Answers ARM semihosting; it is how the retailOS prints its panics |

### The iPod classic 6G machine

You need the **decrypted** `osos` (see [10](10-classic6g-emcore.md)) and a
disk built from the `.ipsw`'s firmware partition:

```bash
# the disk. The FAT32 is EMPTY: the fonts are in the FIRMWARE
# partition, not here.
truncate -s 64M ~/6g/fat32.img && mkfs.vfat -F 32 -n IPOD ~/6g/fat32.img
python3 tools/ipod_classic/mkdisk6g4k.py ~/6g/E.img \
        assets/extracted/Firmware-24.9.1.2 ~/6g/fat32.img

cd build && ./qemu-system-arm -M iPod-Classic \
    -kernel ../assets/classic6g/osos_decrypted.bin -serial null \
    -drive if=ide,format=raw,file=$HOME/6g/E.img -snapshot
```

- It takes **between 22 and 30 s** of wall-clock time until the first pixel.
- **Without `-drive` there is no image**, and that is down to the guest: the interface dies
  looking for fonts and enters a crash-and-reboot cycle. There are no
  fonts inside the firmware image.
- **There is no `entry` property**: the decrypted `osos` is loaded raw at
  `0x08000000`, which is its `addr` in the firmware directory. The two
  properties that do exist, and that do not show up in `-M iPod-Classic,help` either, are
  `bootrom` (path to a bootrom binary, which **we do not have**) and `semihosting`
  (on by default — **it is how the retailOS prints its panics**).

Trace environment variables, all for the Classic:

| Variable | What it traces |
|---|---|
| `DISPTRACE=1` | the compositor at `0x38900000`, with the guest PC and totals |
| `LCDTRACE=1` | the panel: commands, pixels and frame engine starts |
| `ATATRACE=1` | the whole conversation with the ATA, register by register |
| `WHEELTRACE=1` | packets, commands and IRQ 23 edges |
| `GPIOTRACE=1` | GPIO accesses |
| `IPOD_WHEEL_STEP` (6), `IPOD_WHEEL_TICK_MS` (40), `IPOD_WHEEL_UNTOUCH_MS` (1500) | tune the wheel. **None of them is a value from the device** |

### Keys

The keyboard is mapped to the click wheel, **the same on both machines**:

| Key | Button |
|---|---|
| `w` | MENU |
| `a` | LEFT / previous |
| `s` | PLAY |
| `d` | RIGHT / next |
| `Enter` | SELECT |
| `e` | wheel clockwise |
| `q` | wheel counterclockwise |

On the 5G, the first wheel press after changing direction moves
nothing: Rockbox resets its accumulator when the direction reverses. On the Classic that
does not happen — each press moves one item — because the model spaces the
packets with a timer to satisfy Apple's decoder, which
requires an arming packet before each movement.

### Automated navigation tests

There are **three**. All three report how many pixels each frame changes
relative to the previous one; the two 5G ones capture to `assets/` and the Classic one to
`~/6g/nav`.

```bash
./tools/ipod_video/navtest.sh       # 5G, Rockbox, ~55 s
./tools/ipod_video/appletest.sh     # 5G, Apple's retailOS, ~3 min
sh tools/ipod_classic/navtest.sh    # Classic, the retailOS, ~2 min
```

**The Classic one is an assertion**, unlike the other two: it checks
that the screen is still before touching anything, goes through the language screen
(`e`, `e`, `q`, Enter) and **continues to the main menu**, where it checks again
that it is still, turns, enters Music with Enter and leaves with `w`.
It **fails** if any turn does not move the highlight, if the noise floor exceeds
**200 px** —which is the premise all its thresholds hang from—, or if
MENU does not return **the same frame** that was left. It leaves the PNGs in
`~/6g/nav` and **you have to look at them**, which is the other half of the measurement.

Reference figures, **from after the layer gate in §18 of
[11](11-classic6g-retailos.md)**: **8 000 px per turn** over a floor of
**0 px**, **~19 340 px** when entering with Enter from the language screen,
**~7 950 px in the list + ~7 180 px in the panel** per turn in the main menu,
and **less than 1 000 px** between the frame MENU returns to and the one that was left.
The old figures floating around —**76 800 px lit and 50 540 on
select**— are from **before** that gate, when a dead layer covered the
interface: they are not a regression, they are a different image.

And **one run in three or four fails on its own** because the guest drops a
SELECT, without anything being broken; the script's header says how to recognize it.
Before believing a failure: `ps aux | grep qemu` and `uptime`, because with the
machine loaded the guest reboots into *diskmode* and the script warns loudly
that the verdict is not valid.

`navtest.sh` boots, leaves *Now Playing* with MENU, turns the wheel and
selects. `appletest.sh` does two boots of the retailOS: one that photographs
the screen at 10, 20, 40, 60, 90 and 120 s to prove that the interface comes up and
stays, and another that goes language → main menu → *Videos* → back.

**Be careful interpreting the figures.** Rockbox's *Now Playing* screen
animates by itself (the title scrolls), so a difference of ~2,000 pixels
proves nothing. Only the moved selection bar counts, which is ~14,000, or a
new screen, which exceeds 18,000. A handful of pixels is the clock or the
battery icon.

> **Neither of the two 5G scripts is an assertion: they are evidence
> generators.** Both close the QEMU invocation with `|| true` and their
> Python post-processing checks nothing. Verified: if no
> PPM is generated, the loop prints nothing **and the script exits with code 0**. A QEMU that
> dies on boot gives a silent "pass". You have to look at the figures, or
> give them a threshold.

> **Rockbox frames are not bit-for-bit reproducible** against a saved
> baseline, because its status bar has a clock: a couple of
> hundred pixels change depending on the time. The retailOS ones **are**, because it does not
> draw a clock.

The `-semihosting-config enable=on,target=native` that `appletest.sh` carries
inside is **redundant**: the machine enables semihosting on its own.
Checked by removing it — the capture comes out with the same hash. The script's
comment is still good advice (without semihosting panics are silent),
but the flag is not needed.

### Taking a capture without a window

```bash
(sleep 30; echo "screendump /tmp/shot.ppm"; sleep 2; echo quit) \
  | ./build/qemu-system-arm -M iPod-Video,entry=0x735a00 ... \
        -display none -serial null -monitor stdio
```

QEMU writes PPM. `navtest.sh` contains a pure-Python PNG converter
(only `zlib`), in case it is needed on its own.

### Finding out where the firmware got stuck

In order of usefulness:

1. **`-d unimp` with a count.** Almost every blocker on this SoC is a
   tight loop on an unmodeled register, and it is obvious at a glance:

   ```bash
   ... -d unimp,guest_errors 2>&1 | sort | uniq -c | sort -rn | head
   ```

   > **Warning**: in a tight loop that log reaches tens of millions of
   > lines and slows emulation so much it looks like a hang. Before concluding
   > that something is not progressing, measure **without** `-d`.

2. **`-d in_asm`** and look at the last translated blocks: that is the furthest
   the code has got. With `--disable-capstone` there is no disassembly, only
   `OBJD-T` with the bytes, which can be decoded by hand without much fuss.

3. **`info registers` via the monitor**, several times in a row. If `R13`
   changes between samples there are several threads alternating, so the scheduler
   is alive and the problem is something else.

4. **QEMU trace points**, which are gold for the IDE:

   ```bash
   ... -trace "ide_*" -D /tmp/tr.log
   ```

   `ide_bus_exec_cmd` gives the ATA commands, `ide_sector_read` the PIO reads and
   `ide_dma_cb` the DMA ones. Careful: **DMA transfers do not emit
   `ide_sector_read`**; looking there and not finding it does not mean it is not reading.

5. **`info mtree`** to check that a region is where you think it is.
