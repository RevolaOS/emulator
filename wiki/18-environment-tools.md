# Environment and tools: what to install and download to reproduce all of this

Everything that had to be installed or downloaded for the entire project —
from decrypting the firmware to playing audio with Rockbox. Grouped by what
each thing is for. Actual versions used (Ubuntu 24.04 on WSL2).

To build and run just the QEMU machine, section 2 is enough. The rest was
added as each front came up: RE, disks, music, audio, physical device.

---

## 1. Base: Windows + WSL2

- **Windows 11**, with **WSL2** and the **Ubuntu-24.04** distribution.
- **WSLg** (comes with modern WSL2): provides a **graphical window** (to see
  the iPod's screen) and **audio** (PulseAudio → Windows speakers, which is how
  the music played in the emulator is heard).
- The QEMU binary is a Linux ELF: **it only runs inside WSL**.
- The tree lives in `C:\Desarrollo\qemu-ios`, seen from WSL as
  `/mnt/c/Desarrollo/qemu-ios`. **Warning:** building on `/mnt/c` is slow
  (thousands of small files over the Windows bridge); every QEMU rebuild takes
  several minutes because of this.

### On Windows (only for the physical device over USB)

- **usbipd-win** — passes a USB device from Windows to WSL. Needed to talk to
  the physical iPod (emCORE) from Linux.
  ```powershell
  winget install --id dorssel.usbipd-win
  # once, as administrator:  usbipd bind --busid <n-n>
  # each connection:         usbipd attach --wsl --busid <n-n>
  ```

---

## 2. Building and running QEMU

```bash
echo "admin" | sudo -S apt-get install -y \
    ninja-build libglib2.0-dev libpixman-1-dev libsdl2-dev libssl-dev \
    gcc git python3
```

| Package | What for |
|---|---|
| `ninja-build`, `libglib2.0-dev` | QEMU's build system (meson+ninja) |
| `libpixman-1-dev` | framebuffer rendering that QEMU needs |
| `libsdl2-dev` | the graphical window (SDL display) |
| `libssl-dev` | crypto that QEMU links |
| `gcc`, `git`, `python3` | compiler, version control, meson |

See [05-build-notes.md](05-build-notes.md) for the details of building and the
environment's traps (CRLF, git symlinks, `--disable-werror`).

---

## 3. Reverse engineering and debugging

```bash
echo "admin" | sudo -S apt-get install -y gcc-arm-none-eabi gdb-multiarch
```

| Tool | Version | What for |
|---|---|---|
| `gcc-arm-none-eabi` (`arm-none-eabi-gcc`, `-objdump`) | 13.2.1 | **disassembling** Apple's firmware (`objdump -D -b binary -m arm`), and **building Rockbox** (Rockbox's official toolchain was not needed) |
| `gdb-multiarch` | 15.1 | debugging with Rockbox symbols (backtraces with function names). See `tools/ipod_classic/gdbpoke.py` |

`python3` (already installed) was used through our own scripts for firmware
analysis: `tools/ipod_classic/findlit.py`, `osana.py`, `fwtool.py`, etc.

---

## 4. Disk images (FAT) and music library

```bash
echo "admin" | sudo -S apt-get install -y \
    dosfstools mtools libgpod-common libgpod4t64 libgpod-dev
```

| Package | Version | What for |
|---|---|---|
| `dosfstools` (`mkfs.vfat`) | 4.2 | formatting the FAT32/FAT16 partitions of the iPod's disk |
| `mtools` (`mcopy`, `mmd`) | 4.0.43 | copying files to the FAT without mounting it |
| `libgpod-common`, `libgpod4t64`, `libgpod-dev` | 0.8.3 | **writing the `iTunesDB`** with the signature the iPod accepts. `libgpod-dev` brings `itdb.h`, against which `tools/ipod_classic/addtrack.c` is compiled. **There is no CLI and no Python bindings** on this Ubuntu: the library has to be called from C |

---

## 5. Audio: playing and verifying

```bash
echo "admin" | sudo -S apt-get install -y ffmpeg python3-numpy zip
```

| Package | Version | What for |
|---|---|---|
| `ffmpeg` | 6.1.1 | decoding the original MP3 to reference PCM, to **compare** with what we capture from the emulator (cross-correlation, spectrograms) |
| `python3-numpy` | 1.26.4 | the measurement: RMS, Pearson correlation, spectrum — the proof that the WAV **is the song** and not noise |
| `zip` | — | packaging the Rockbox build (`rockbox.zip` with the codecs) |

The live audio backend (speakers) uses **WSLg's PulseAudio**, already present;
QEMU is wired with `-audiodev pa`.

---

## 6. The physical device over USB (emCORE), first phase

```bash
echo "admin" | sudo -S apt-get install -y python3-usb libusb-1.0-0
```

| Package | Version | What for |
|---|---|---|
| `python3-usb` (pyusb) | 1.2.1 | talking to the iPod's **emCORE debugger** (VID `FFFF`:PID `E000`) so that **the device itself decrypts the firmware** with its GID key. See [10-classic6g-emcore.md](10-classic6g-emcore.md) and `tools/ipod_classic/emcore_s5l8702.py` |
| `libusb-1.0-0` | 1.0.27 | the USB layer under pyusb |

> The official emCORE clients did not work (`ff/ff/ff` interface and a retired
> pyusb API); a client of our own was written. Details in [10](10-classic6g-emcore.md).

---

## 7. Downloaded code and firmware (not `apt`)

| What | From where | What for | In the tree |
|---|---|---|---|
| **QEMU-iOS base** | `github.com/devos50/qemu-ios` (branch `ipod_touch_2g`) | the emulator's starting point | the repo itself |
| **Rockbox** (complete tree) | rockbox.org / git | executable specification of the S5L8702 hardware, **and the free firmware that plays audio** (decodes on the ARM). The `ipod6g` target is built | `assets/rockbox/` |
| **freemyipod/qemu** (branch `s5l8702-dev`) | `github.com/freemyipod/qemu` | earlier model of the S5L8702, reference | `assets/ref/` |
| **giek2000/ipod-classic-firmware-research** | `github.com/giek2000/...` | Classic 6G research, decryption guide | `assets/ref/giek/` |
| **The firmware** `iPod_24.1.1.2.ipsw` | provided by the user | Apple's `osos` to decrypt and boot | `assets/` (gitignored) |
| Documentation | freemyipod.org/wiki | memory map, protocol, bootrom | cited in the wiki |

> `assets/` is **outside git**: Apple's firmware and the device dumps are not
> ours to redistribute.

---

## 8. Everything at once (the `apt`s above, together)

```bash
echo "admin" | sudo -S apt-get update && echo "admin" | sudo -S apt-get install -y \
    ninja-build libglib2.0-dev libpixman-1-dev libsdl2-dev libssl-dev \
    gcc git python3 \
    gcc-arm-none-eabi gdb-multiarch \
    dosfstools mtools libgpod-common libgpod4t64 libgpod-dev \
    ffmpeg python3-numpy zip \
    python3-usb libusb-1.0-0
```

And on Windows, if the physical iPod is going to be used:
`winget install --id dorssel.usbipd-win`.

With that and the repos from section 7, the environment reproduces the entire
project: decrypt, boot, draw, navigate, mount the disk, list music, build and
boot Rockbox, and play audio.
