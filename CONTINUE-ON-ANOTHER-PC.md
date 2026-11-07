# Continuing the work on another PC

Guide for resuming the iPod Classic 6G emulator (with the HLE Now Playing
player) on a new machine. Work status: `wiki/19-classic6g-dsp-hle.md`
section **§11.23 "WHERE WE ARE"**.

## 1. Clone

```bash
git clone https://github.com/SoftwareEscarlata/qemu-ios.git
cd qemu-ios
git checkout dsp-deadlock-break   # the branch with all the work
```

## 2. Environment (WSL Ubuntu 24.04 recommended, or native Linux)

Build dependencies and dependencies of the session tools:

```bash
sudo apt update
sudo apt install -y build-essential ninja-build meson pkg-config python3 \
  libglib2.0-dev libpixman-1-dev libsdl2-dev libpulse-dev zlib1g-dev \
  ffmpeg gdb-multiarch
# ffmpeg and ffprobe are required for the audio HLE (host-side decode + tags).
# gdb-multiarch for the probes in tools/session-dbg/.
```

## 3. Build

```bash
./configure --target-list=arm-softmmu --enable-sdl --enable-plugins   # or the repo's meson setup
ninja -C build qemu-system-arm
# the trace plugin:  ninja -C build contrib/plugins/libpctrace.so
```

## 4. Binary assets (NOT in git — see the Release)

The binaries (Apple firmware + disk images) are large and copyrighted,
so they go in a **GitHub Release** of the repo, not in the git tree
(`/assets/` is in `.gitignore`). Download them from the Release and place them:

| File | Place it in | Purpose |
|---|---|---|
| `osos_decrypted.bin` | `assets/classic6g/` | retailOS kernel (`-kernel`) |
| `rsrc_fat16.img`, `aupd_decrypted.bin`, `bootflash_nor_1MB.bin` | `assets/classic6g/` | 6G resources / boot |
| `Emulti.img` | `~/6g/` (or wherever you prefer) | disk with music (one song; for testing the player) |
| `osos.dis`, `iram.dis` | `~/6g/` | disassembly (VA = off + 0x07FF51B8) used by the probes in `tools/session-dbg/` |

Adjust the `HLE_AUDIO_DISK` / `-drive` paths to wherever you put `Emulti.img`.

## 5. Launch the Now Playing player (HLE)

```bash
export DISPLAY=:0                       # WSLg / X for the window
HLE_AUDIO_DATAPATH=1 HLE_AUDIO_DISK=$HOME/6g/Emulti.img HLE_AUDIO_SUSTAIN=1 \
AUDIOPACE=1 AUDIOLIVE=1 HLE_NP_OVERLAY=1 \
  build/qemu-system-arm -M iPod-Classic \
  -kernel assets/classic6g/osos_decrypted.bin \
  -drive if=ide,format=raw,file=$HOME/6g/Emulti.img -snapshot \
  -audio pa -monitor telnet:127.0.0.1:4455,server,nowait -L build/pc-bios
```

Navigate with the wheel to **Music → a song → SELECT**. The player appears.
Controls (keyboard): `s`=play/pause, `d`/`a`=next/previous, `]`/`[`=seek ±5s,
`w`=hide/`Enter`=show.

## 6. Session tools

`tools/session-dbg/` contains all the investigation scripts from this session
(runners `run_*.sh`, gdb probes `*.gdb` / `*.py`, captures). They reference absolute
WSL paths (`/mnt/c/Desarrollo/qemu-ios`, `~/6g/...`) — adjust them to the path
on the new machine before using them. Remote control of the guest:
`tools/ipod_classic/iremote.py` (wheel via telnet monitor).

## 7. What was NOT uploaded (and why)

- The ~15 GB of disk image variants in `~/6g/` (experiments): only
  `Emulti.img` (the working one) is uploaded. The rest is regenerated or requested separately.
- The rest of `assets/` (rockbox 615M, 5G disks, ipsw, ref…): not needed
  for the 6G player. If you need them, ask for them.

## Current status (summary)

- **Achieved:** usable **HLE** Now Playing player — REAL audio of the song
  from the disk + screen drawn by QEMU (overlay) + play/pause/seek/
  skip controls that drive the audio. See `wiki/19` §11.23.
- **Not achieved (native):** Apple's ORIGINAL Now Playing screen is still
  blocked by the structural MeCCA deadlock (§11.12-11.22). The redirect
  fix keeps the guest alive, but its native render produces nothing.
