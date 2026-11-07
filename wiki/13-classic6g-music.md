# Putting music on the emulated Classic 6G

Written on 2026-08-11, after the retailOS booted, drew and could be navigated.
The next question was the obvious one: *what if we put a song on it?*

What follows is **VERIFIED** except where it says otherwise. The part that
remained unchecked is in section 5, and it is the one that decides whether
this is any use.

## 1. Why copying the MP3 is not enough

The iPod does not walk the disk looking for audio files. It reads **its
database**, the `iTunesDB` in `iPod_Control/iTunes/`, and shows whatever is in
there. A disk full of MP3s with no database gives an empty library.

And since this generation —the 2007 models, which include the classic 6G and
the nano 3G— Apple **signs that database** with a hash that depends on the
device's FireWire identifier. It is what for years kept third-party tools from
writing music to these models. `libgpod` ended up implementing it, and it is
the only free implementation that does.

## 2. The device data comes out of the NOR

`assets/classic6g/bootflash_nor_1MB.bin` starts with the `SysCfg` block, with
the four-letter tags **reversed** — that is why a search for `"SCfg"` finds
nothing and one for `"gfCS"` does. It is the same reversal used by the
entries of the firmware's image directory
([01-firmware-ipsw.md](01-firmware-ipsw.md)).

| Offset | Tag | Reversed | Value |
|---|---|---|---|
| `0x0000` | `gfCS` | `SCfg` | block header |
| `0x0018` | `mNrS` | `SrNm` | the device's serial number |
| `0x0090` | `NBLM` | `MLBN` | the board number |
| `0x00a4` | `#doM` | `Mod#` | `MB147` |

`MB147` is exactly what `libgpod` calls the *model number* and what it uses to
decide which iPod it is initializing. The other two identify the user's
device, so **they are not transcribed here**; they are in the dump, which is
outside git.

**What is NOT in the `SysCfg`: the FireWire identifier.** It was searched for
and does not appear. See section 5.

## 3. The recipe

`libgpod` comes with Ubuntu but **with no command-line tool** that adds tracks
— `libgpod-common` only brings `ipod-read-sysinfo-extended` and
`ipod-time-sync`, and `gtkpod` is not packaged. You have to call the library.
`libgpod-dev` brings `itdb.h`.

```bash
echo "admin" | sudo -S -p "" apt-get install -y libgpod-common libgpod4 libgpod-dev
# python3-gpod does NOT exist on this Ubuntu: the bindings are not an option
```

A fifty-line program against `itdb.h` is enough: `itdb_init_ipod()`,
`itdb_parse()`, `itdb_track_new()` + `itdb_track_add()` +
`itdb_playlist_add_track()` to the master playlist, `itdb_cp_track_to_ipod()`,
and `itdb_write()`.

**`itdb_init_ipod()` fails right away** with `Couldn't find the iPod firewire
ID` if there is no `SysInfo`. It has to be written beforehand, by hand:

```
iPod_Control/Device/SysInfo
    ModelNumStr: xMB147
    FirewireGuid: 000A270019B4C3D2
    pszSerialNumber: <the one from SysCfg>
```

With that it writes the library without complaining:

```
iPod_Control/Music/F21/libgpod408455.mp3
iPod_Control/iTunes/iTunesDB          6330 bytes, 1 track
iPod_Control/Artwork/ArtworkDB
iPod_Control/Device/SysInfo
```

And the complete disk is assembled as always, with the FAT32 already
populated:

```bash
python3 tools/ipod_classic/mkdisk6g4k.py ~/6g/Emusic.img \
        assets/extracted/Firmware-24.9.1.2 ~/6g/fat32m.img
```

## 4. Careful when booting it by hand

`tools/ipod_classic/navtest.sh` does `pkill -9 -x qemu-system-arm` before each
run, and `-x` is an **exact name** match. A copy of the binary with another
name survives — useful so that one session does not kill another's QEMU, and
annoying when that survivor eats a core and makes the guest miss its own boot
deadlines and reboot into *diskmode*.

That already cost four runs, and that is why `navtest.sh` now counts stray
processes and warns before starting. If you start a copy with another name to
look at the screen, **kill it yourself** before trusting a test run.

## 5. Why the music does not show up: the data volume does NOT mount

> **SUPERSEDED on 2026-08-12 — the volume DOES mount now. See §6 and §7.** This
> section described the state in which the firmware rejected the FAT32 at the
> header. That **is resolved** (§6): with three bytes `"FAT"` at `boot+0x34`
> the classifier accepts the volume and the firmware reads the directory, the
> FAT and `iPod_Control` (measured: 30 DMA reads inside the partition versus 2
> when it is rejected). The firmware **now does read and parse the
> `iTunesDB`**; where it stops today is at the **signature** of that database
> (§7) — exactly what the first version of §5 suspected and the second
> dismissed as premature. The wheel has come full circle. What follows is kept
> as the diagnosis that led to §6.

The first version of this section blamed the `iTunesDB` signature. It turned
out to be **the right question asked too early**: the firmware did not get as
far as looking at the database because **it rejected the entire FAT32 volume
when reading its header**, long before the signature. With the volume
rejection resolved (§6), the signature is the problem again (§7).

### What is VERIFIED, by running it

All of this is from 2026-08-12, measured, not assumed:

1. **The Music menu is reached.** Navigating with `sendkey` from the monitor
   (wait 38 s after boot and space the presses ~9 s apart), the retailOS
   enters Music and draws the list Cover Flow / Playlists / … / Songs with
   **"No Music"** in the panel. Screenshot looked at, not just counted.

2. **The FAT32 is read ONCE and rejected.** With `ATATRACE=1` on a boot that
   reaches Music: of the ~450 DMA reads, **only one** falls inside the data
   partition (its first 64 sectors of 512 B = the header), and the root
   directory, `iPod_Control` and the `iTunesDB` are **never** read. If the
   volume had mounted, the firmware would have read the directory looking for
   `iPod_Control`. It does not. It is rejected at the header.

3. **The signature, the hash and the `iTunesDB.p7b` are IRRELEVANT.** None of
   that gets validated because the volume does not mount. The previous section
   was worrying about the wrong problem.

4. **The rejection does not depend on** (all tested, same one-read trace):
   - the FAT sector size: **512, 2048 and 4096** fail the same;
   - the partition type in the MBR: **0x0B (CHS) and 0x0C (LBA)** fail the
     same;
   - the hidden sectors field of the boot sector, set to its correct value.

### Who reads the header, and the mount chain

Instrumenting the ATA model to print the guest's PC and stack on the read of
the data partition (LBA 181568), the read comes from a single call and the
chain, from inside out, is:

```
0x0805a6f4   block read primitive (registers at 0x38700000)
0x08326518   block read loop (split into sectors)
0x0827a6b0   filesystem read wrapper
0x0824690c   FS read -> compares the result with 31 at 0x08246948
0x082465xx   VFS DISPATCH: picks the driver by filesystem TYPE
             (cmp #1 / #4 / #5, each with its own function table)
0x0813b20c   RTXC worker task
```

The point where accept or reject is decided is **behind the function table**
that `0x082465xx` selects: the firmware identifies the volume type and
dispatches to the corresponding driver, and that driver is the one that
validates (or discards) the FAT header.

### Further in: the read succeeds; the rejection is about content

Going down the chain by hand (2026-08-12):

- **The boot sector read does NOT fail.** `0x0827a6b0` is the FS read and
  returns 0 (success); its wrapper `0x0824690c` only maps *read* errors (it
  compares the result with 31 and 0, I/O codes). So the header bytes arrive
  fine in the buffer. What rejects the volume is the **examination** of those
  bytes, not the read.

- **The partition table detector is `0x0827a980`.** It reads a sector and
  looks at its `+0x1FE`: if it is `0xAA55` it treats it as an **MBR** and
  jumps to the parser `0x080aee04`; if it starts with `"ER"` it treats it as
  **APM** (Apple Partition Map) and jumps to `0x080a38cc`; if neither, error.
  The addresses of the two parsers match the ones that §15 of
  [11](11-classic6g-retailos.md) had already seen executing.

- The examination that decides FAT-valid-or-not is **behind the vtable
  dispatch** at `0x082465xx` (it picks the driver by type, `cmp #1/#4/#5`),
  and that is where this session was cut off: resolving that pointer at run
  time needs the debugger.

### What is missing, scoped so it can be picked up again

Set a breakpoint (with `tools/ipod_classic/gdbpoke.py`) in the FAT driver that
the vtable selects at `0x082465xx`, dump the already-read boot sector buffer,
and see **which field rejects**. It is a single-session task. Candidates to
look at along the way: the byte that `0x0824659c` reads at `+6` and compares
with `8`, the string `"FAT32   "` at `+0x52` of the boot sector, and — to rule
it out — whether the firmware runs the `0x0827a980` detector on the data
partition itself and, on seeing the `0xAA55` that every FAT boot sector
carries, mistakes it for a nested MBR.

Instruments that helped and can be redone (they were temporary, they are not
in the tree): in `ipod_classic_ata.c`, print in `dma_go` the guest's PC and
stack (`current_cpu` → `ARM_CPU(cs)->env.regs[13..15]`,
`cpu_memory_rw_debug`) when `ide_get_sector(is) >= 181568`; that gave the
entire mount chain in one shot.

> **Working hypothesis, unverified:** that a factory iPod classic 6G formats
> its data partition with parameters our `mkfs.vfat` does not reproduce
> (alignment, FAT size, or a BPB field that Apple's driver requires). The way
> to settle it is the breakpoint above, not to keep trying formats blindly:
> 512/2048/4096 and 0x0B/0x0C have already been ruled out.

### Second unknown, independent: getting it to PLAY

Even if the track appears in the list, playing it is another problem. I²S is
not modeled —today some 6 000 accesses per boot are thrown on the floor— so
audio is a separate front. See [04-roadmap.md](04-roadmap.md).

---

## 6. The volume DOES mount: the `"FAT"` at `+0x34` trick (2026-08-12)

All **VERIFIED by running it**. Base VA = file + `0x07FF51B8`.

### The rejection chain, disassembled and measured

The firmware rejected the FAT32 **because its boot sector carries `0x55AA` at
`+0x1FE`**, the signature every FAT VBR has. The generic volume classifier
`0x0808253c` reads block 0 of the partition, sees that `0x55AA` and treats it
as if it might be a **partition table (MBR)** — not as a filesystem:

```
0x080825d4  ldrb ...[r4,#1] / [r4] ; buf[0..1]  ("ER" = Apple partition map?)
0x080825f4  ldrh r0,[r4,#0x1FE]    ; buf[0x1FE]
0x080825f8  cmp  r0,#0xAA55        ; -> r6 = 2 (type "sector with 0xAA55")
0x080826d8  bl   0x0806a128        ; handler for the "sector with 0xAA55"
0x080826f4  mov  r0,#31            ; classifier REJECTION
```

`0x0806a128` tries to parse the MBR (four entries at `buf+0x1BE`); in a FAT
VBR that area is **all zeros**, so there is no valid entry and it falls to an
ASCII last resort:

```
0x0806a1f8  bl 0x800f484 ; memcmp(buf+0x34, "FAT", 3)   -> beq SUCCESS
0x0806a210  bl 0x800f484 ; memcmp(buf+0x2b, "FAT", 3)
0x0806a218  bne 0x0806a240                              ; <<< REJECTS HERE
0x0806a240  mov r0,#0    ; return 0 -> classifier returns 31 -> "No Music"
```

`mkfs.vfat` leaves `+0x34` and `+0x2b` at zero (the real `"FAT32   "` is at
`+0x52`, which this path **does not look at**), so both `memcmp`s fail. The
chain above `0x0806a128`: `0x0827a6b0` (FS read) → `0x0824690c` (compares with
31) → VFS dispatch `0x082465xx`. The detector `0x0827a980` (the "nested MBR"
hypothesis) runs only on LBA0 at boot; the real "nested MBR" happens in
`0x0808253c`/`0x0806a128` running on the **data partition itself**.

### The fix, and the proof that it mounts

Writing three bytes `46 41 54` (`"FAT"`) at **`boot+0x34`** (the
`BPB_Reserved` area, reserved and ignored by any real FAT driver) makes
`0x0806a128` return success and the classifier go from **rejecting (31)** to
**accepting (0)**. `mkdisk6g4k.py` does this now (`mark_fat_volume`), in the
boot sector and in its copy (`BkBootSec`).

**Measured with a temporary instrument** (LBA of each DMA in `dma_go`, already
reverted), boot **with no keys**:

| Disk | Reads inside the data partition (LBA512 ≥ 181568) |
|---|---|
| Without the trick (`E4k.img`) | **2**, both at LBA 181568 (only the boot sector). Rejected. |
| With the trick (`E4kfat.img`) | **30**: FAT (181824), root directory (183872), `iPod_Control`, the `iTunesDB` area (200544), **and writes back**. Mounted. |

A mounted volume reads the directory; the rejected one does not. 30 versus 2
settles it.

> **Auditor's correction (2026-08-12).** The disks `E4k.img` and `E4kfat.img`
> in the table above do **not** differ only in the marker: `cmp -l` gives
> **518 different bytes** (FAT/directory contents), because they were built
> from different FAT images. That is, that A/B **does not isolate** the
> variable, even though the conclusion is correct. Redone with a clean A/B
> —two disks built from the **same** `fat4k.img`, one with the marker and the
> other with those 6 bytes set to zero, **only 6 bytes of difference**— and
> measured the same way (LBA of each DMA in `dma_go`, boot with no keys,
> 60 s): **marked = 85 reads** inside the partition (181568 boot, 181824 FAT,
> 183872 root directory, 200520–200552 `iTunesDB`); **unmarked = 3 reads, all
> at 181568** (only the boot sector). The marker is the only cause. The LBAs
> match the ones above; only the count changes because of the measurement
> window.

> **Honest nuance.** The `"FAT"@0x34` is **not** what Apple writes: its two
> format templates at `0x0884c3c4`/`0x0884c5c4` (OEM `MSDOS5.0`, labels
> `iPod`/`NO NAME`) have `+0x34` at zero (VERIFIED, dump). It is a **disk
> layout** change that satisfies Apple's classifier, sanctioned by the brief.
> Why the firmware probes the partition with `0x0808253c` instead of mounting
> it by the `0x0B` type of the LBA0 MBR remains an open question (the lever is
> upstream of `0x0806a128`), but for the goal —getting the volume to mount—
> this is measured and works.

## 7. Where it stops TODAY: the `iTunesDB` signature (2026-08-12)

With the volume mounted, the firmware **reads and parses** the `iTunesDB`. The
database that `libgpod` writes is structurally correct (VERIFIED,
`mhbd` → `mhsd` type 1 with `mhlt` count=1 and one `mhit`, referenced by the
master playlist `mhyp`/`mhip`). But parsing **fails at signature validation**
and the library comes out empty ("No Music", screenshot looked at in the main
menu: musical note + "No Music" in the panel).

Measured with `gdbpoke.py`/`fatprobe.py` (base VA = file + `0x07FF51B8`):

| Address | What it is |
|---|---|
| `0x08041360` | `iTunesDB` section parser (magic `"mhsd"` at `0x8041600`, loop over `r4[0x2c]` sections) |
| `0x08041364` | `bl 0x08082810` — fetches the FireWire GUID (8 bytes), *byte-swaps* it to big-endian and caches it at `device+0x400f4` |
| `0x08041384` | **`bl 0x08073cf8`** — **the signature validator**; uses the GUID area (`device+0x400f0/f4`) |
| `0x08041388` | measured: **`r0 = 0xffff5bda`** (error) on return from `0x08073cf8` |
| `0x080415a8` | parser exit with `sl = 0xffff5bda` → empty library |

**Where the GUID comes from, and why it is 0.** The getter `0x08323ac8` reads
`obj[0x38]`(low)/`obj[0x3c]`(high) from a `"SysI"` object (default `obj`
`0x0896d6fc`). That object lives in `.bss` (past the end of `osos`, which
measures `0x976A3C`) and **nobody populates it**: the boot is via `-kernel`
of the decrypted `osos`, **without the NOR**, which is where the real device
keeps its identity. Measured in the hash context (`sl="mhbd"`, `r1="guid"`):
the getter returns `r0=0, r1=0`. **The emulated firmware's GUID is
`0x0000000000000000`.**

**And it is not (only) the GUID value.** A disk `E4kG.img` was built whose
`iTunesDB` `libgpod` signed with `FirewireGuid: 000A270019B4C3D2`, and the
getter was **forced** via gdb to return that same GUID
(`r0=0x19B4C3D2, r1=0x000A2700`) on every call. The parser **failed
identically** (`sl=0xffff5bda`). That is: making the GUID *value* match is not
enough. The signature that this Ubuntu's `libgpod` (4.3.2) produces for model
`MB147` **does not satisfy this particular `retailOS` (24.9.1.2)** — whether
because of an algorithm/version difference, because of the exact encoding of
the GUID that goes into the hash (the firmware *byte-swaps* it at
`0x08082810`), or because of a second hash that is missing. This is the
protection that [§1](#1-why-copying-the-mp3-is-not-enough) describes: the one
that "for years kept third-party tools from writing music" to these models.

### How to pick it up again (scoped)

The algorithm-independent path: set a breakpoint inside `0x08073cf8` at the
final `memcmp`/comparison, **read the signature the firmware EXPECTS** (not
the one the disk carries), and write those bytes into the `iTunesDB`'s
signature field (the field is usually excluded from its own hash).
`0x08073cf8` allocates memory (`0x80c6614`), processes the GUID area with
`0x0803f4f0` (`device+0x400f0`, `0x6c` bytes) and that is where the
`0xffff5bda` comes from — that is the subtree to dissect. A deeper
alternative: model the NOR/`SysInfo` so the firmware has a real GUID and
**measure** whether with that `libgpod` (fed the same GUID) produces an
accepted signature — the gdb forcing experiment suggests it would not be
enough, but it does not rule out a GUID encoding problem.

### New tools (they do not touch the model)

`tools/ipod_classic/fatprobe.py` (breakpoints with register-relative dumps),
`fatrun.sh` (starts QEMU `-S -gdb` and drives `fatprobe`), `navsongs.sh`
(navigates to Music→Songs with a screenshot at each step), `navmusic.sh`. The
disk that mounts and carries the library: `~/6g/E4kfat.img` (GUID 0) and
`~/6g/E4kG.img` (GUID `000A2700…`). The `+0x34` trick is already applied by
`mkdisk6g4k.py`.

## 8. Resolved: the signature this firmware expects is 20 zeros, because SHA‑1 is unmodelled hardware (2026‑08‑12)

**The song now appears listed under Music → 曲 (Songs).** Screenshot inspected:
`~/6g/song_listed/53-songs.png` and `54-songs2.png` — the list shows the track
with its title (the MP3's name, as a marquee: "…Encore (Official Audio) - Linkin
Park _ JAY‑Z…") and, below it, the artist **Linkin Park**. VA base = file +
`0x07FF51B8`.

§7 left two suspicions —"`libgpod`'s algorithm is not the one the retailOS
expects" and "it's the GUID value"—. **Both are false.** Measured:

- **Only `hash58` counts.** Without `iTunesDB.p7b` (it doesn't exist on the
  disk) the validator `0x08073cf8` takes the `hash58` path and no other.
  VERIFIED (`~/6g/sigprobe1.txt`).
- **The firmware's algorithm IS `libgpod`'s `itdb_hash58`, byte for byte.** The
  two S‑boxes (`table1`/`table2`) and the `fixed[18]` magic of the key
  derivation are literally in `osos_decrypted.bin`, contiguous and in order
  (`0x08388a10` / `0x08388b10` / `0x08388c10`); and the validator zeroes *the
  same three fields* before hashing —`db_id` (`+0x18`, 8 B), `unk_0x32`
  (`+0x32`, 20 B), `hash58` (`+0x58`, 20 B)— as `itdb_hash58.c:265‑267`. It is
  HMAC‑SHA1 with a key derived from the FireWire GUID. VERIFIED (disassembly +
  `libgpod` 0.8.3 source).

**The real cause of the wall:** the S5L8702's SHA‑1 **is not computed by the
firmware, it is a hardware accelerator at `0x38000000`** (`SHA1_MEM_BASE`; its
driver is `0x08068b64`: `r6=0x38000000`, writes control `[r6+0x80]`, pointer
`[r6+0x84]`, length `[r6+0x8c]`, command `[r6]`, and *busy‑polls* `[r6]&1`).
**QEMU doesn't model it:** it is in the *"peripherals we know the address of but
have not modelled"* table in `hw/arm/ipod_classic/ipod_classic.c`
(`{ "s5l8702.sha1", SHA1_MEM_BASE, 0x100000 }`), and its reads return 0.
Consequence: **every SHA‑1 digest the firmware requests comes out as zero** →
the HMAC key comes out 0 → **the signature the firmware EXPECTS collapses to 20
zero bytes**. VERIFIED from both sides (guest trace + QEMU source).

**The decisive measurement** (`~/6g/sigprobe2.txt`, at the final `memcmp`
`0x08328598`): the signature the firmware **computes/expects** = `00 00 … 00`
(20 zeros); the one the disk carries (the one `libgpod` wrote for the
`SysInfo` GUID) = `28 db 9f 20 …`. That's why `E4kfat.img` —*correctly* signed
by `libgpod`— is rejected: the firmware never gets to compute `28db…`, it
computes zeros. And that's why the gdb forcing in §7 changed nothing: with the
SHA at 0 the key is 0 **whatever the GUID** (besides the fact that that forcing
wrote `r0/r1`, whereas the getter `0x08082810` leaves the GUID in memory at
`device+0x400f4`, not in registers).

**The fix, VERIFIED end to end:** set `mhbd+0x58 = 20 zeros` in both copies of
the `iTunesDB`. The validator's return at `0x08041388` goes from `0xffff5bda` to
`r0=0`; the parser `0x080415a8` exits successfully; the Music panel goes from
"No Music" to "1 曲/Song"; and the Music→曲 list shows the track. It is now
applied by `mkdisk6g4k.py` (`zero_itunesdb_hash58`, alongside the `"FAT"@0x34`
trick). Disk reproduced by the pipeline and checked:
`~/6g/E4kzero_rebuilt.img` (built with `mkdisk6g4k.py … Firmware‑24.9.1.2
fat4k.img`).

> **Convenience stub, annotated** (rule of the assignment). The 20 zeros are
> **not** what a factory iPod signs; they are the only signature this emulator
> accepts *as long as* `0x38000000` isn't modeled. It is written that way in
> `zero_itunesdb_hash58()`. When the SHA‑1 gets modeled it has to be removed
> and signed with `libgpod` for the device's real GUID.

### The other wall of this session: actually entering the list (keys)

The "1 Song" count was already showing; **entering** the list failed because of
lost SELECTs (wiki/11 §18.8/§19.6). Measured here, and it's the key to reliable
navigation: **a "cold" center press —with no prior wheel activity— is IGNORED
by the retailOS**; it only honors SELECT with the wheel active (just touched).
With that, navigation becomes deterministic: **"turn to touch the wheel, THEN
press center"**, and verify each step with a pixel *diff* of two `screendump`s.
Since Music is the top row and the menus don't wrap around, a `q` (up) touches
it without moving off it; to get to 曲 you go down five steps.

### New tools (they don't touch the model)

`tools/ipod_classic/iremote.py` (control via the TCP monitor: `press`/`seq`
with *diff* verification), `irun.sh` (launches a `qmus` copy —its own name, so
that `navtest.sh`'s `pkill -x` doesn't touch it— with the monitor on
`127.0.0.1:4455`) and `isongs.sh` (drives language→menu→Music→曲 with a
screenshot per step). They only read/send keys; **they don't modify the model
or rebuild QEMU** (the signature fix is on the disk, not in the emulation).

### The underlying path, pending (INFERRED)

Model the SHA‑1 accelerator at `0x38000000` (reference: the Touch already does
it in `hw/arm/ipod_touch_sha1.c`). With the real SHA the firmware would compute
a non-trivial HMAC‑SHA1, and then `hash58` would have to be signed with the
device's **real GUID** —today `0`, because it boots via `-kernel` without the
NOR/`SysInfo`—. Since the algorithm matches, `libgpod` would sign correctly
*once* (a) the SHA is modeled and (b) the model's GUID and the `SysInfo` GUID
are the same. Not tested — it is the route if hardware fidelity is wanted
instead of the 20-zeros shortcut.
