# ipodvm

Launch, drive, read, navigate and profile emulated iPod Classic VMs, from a
shell or as MCP tools. `launch` boots a RepriseOS CFW image with the complete
feature-test configuration from [AGENTS.md](../../AGENTS.md); the other
commands work with whatever firmware the VM runs. Each command is a one-shot
QMP client; there is no interactive control shell.

```sh
alias ipodvm="python3 $PWD/tools/ipodvm/ipodvm.py"    # from qemu-ipod-classic
ipodvm disk --base /path/library.img --out /tmp/x/disk.img --themes build/x/themes
ipodvm launch --firmware build/x/osos-cfw.bin --disk /tmp/x/disk.img
ipodvm goto "Settings/Theme"
ipodvm screen
```

Requirements: Python 3 (standard library only), the QEMU build in `build/`,
Xcode's `swiftc` for screen reading, and mtools for `disk`. Add `--json` to
any command for machine-readable output.

| File | Contents |
| --- | --- |
| `core.py` | QMP, launch, input, screenshots, test disks, profiler control |
| `screen.py` | screen reading and `goto` navigation |
| `screentext.swift` | Vision text recognition, compiled to `build/ipodvm/screentext` on first use |
| `ipodvm.py` | command line |
| `mcp_server.py` | MCP server (stdio) |

## Test disks

`disk` clones `--base` (an APFS clone, instant) and installs every `.bin`
under `--themes`, which mirrors the device's `/themes` tree
(`classic/default.bin`, `classic/dark.bin`, `<id>/theme.bin`, …). Existing
packs on the clone are removed first unless `--keep-themes`: packs built for
another catalog are rejected by the firmware, which then hides the Theme menu.
The FAT32 volume offset is found from the MBR (iPod disks use 4 KiB sectors).

## Runs

Each VM lives in `/tmp/ipodvm/<name>/` (default name `vm`, select with `-n`).
The short path keeps `qmp.sock` and `gdb.sock` under macOS's 104-byte Unix
socket limit. The directory holds `launch.json` (command, environment,
firmware SHA-256, disk, layout), `qemu.log`, `audio.wav`, `shots/` and
profiler output. The disk is an APFS clone of `--disk`, and QEMU runs with
`-snapshot`, so neither image is written. Launching a name again replaces its
stopped run directory, including screenshots and profiles.

Run `launch` from the firmware repository (or pass `--layout`): the payload
offset and load address come from its `payload/native/layout.h`. `launch`
returns once the screen has been non-black and unchanged across two polls
5 s apart (normally Language or the main menu, 45–75 s after start) and leaves the guest paused (`--run` keeps
it running, `--no-wait` returns immediately).

`launch` opens an SDL window showing the live screen; it also accepts keys
typed into it. Paused guests show their last frame. The title (also shown by
`list` and `status`) names the VM, the repository and branch it was launched
from, and the Claude Code session ID prefix when there is one; override it
with `--title`. `--headless` runs without a window.

## Reading the screen

`screen` returns the title, the visible rows (label, value such as `Off`,
second-line detail such as `99 Songs`, submenu arrow), the selected row and
the preview pane text. `state` is `list`, `other` (Now Playing, About, …),
`charging` (the idle Charging screen; any key wakes it) or `blank`.

Text comes from macOS Vision. The selection bar is found from pixels rather
than a fixed colour, so it does not depend on the theme, accent colour or dark
mode: it is the longest run of rows, at most 70 px tall, whose colour at x = 1
or x = 158 (inside the bar on half- and full-width lists, which covers rows
where a scrolling label crosses x = 1) differs strongly from the list
background. The bar's width, followed pixel by pixel along its top and bottom
rows, separates half-width lists with a preview pane from full-width lists. Vision can misread individual glyphs (the Greek
"Ελληνικά" reads as Latin letters) and a selected label that is too long
scrolls and reads garbled.

## Navigating

`goto Settings/Theme` returns to the main menu with Menu (passing the
Language screen and waking the Charging screen), then for each label scrolls
until that row is selected and opens it. Labels match exactly, then as a
truncated `…` label, then as a prefix, ignoring case. A garbled scrolling
label is confirmed from the rows above and below it. Rows not on screen are
searched downwards, then upwards; the error lists every row seen. `--here`
starts from the current screen. Selecting a toggle row toggles it.

Wheel keys go in batches of at most three, and each batch is verified by
reading the screen again. Opening a row uses the verified Select below,
comparing only the list area so an animated preview pane cannot fake a
change.

## Input

| Command | Behaviour |
| --- | --- |
| `key KEYS…` | resume, send each key (3 s settle after wheel keys, 5 s after buttons), pause, screenshot; reports whether the frame changed |
| `select`, `menu` | press and verify the frame changed; if not, run the guest `--settle` s (default 25) more, then wheel-touch and retry only if the touch left the frame unchanged |
| `wait --until change\|stable` | run the guest until the frame differs from the start, or two polls match |
| `run [N]`, `pause` | resume (for N seconds, then pause) or pause |
| `shot [FILE]` | screenshot (default: next file in `shots/`) |
| `status` | run state, PC/LR, title, firmware, profiler |
| `hmp LINE…` | raw HMP command, e.g. `hmp info registers` |
| `list`, `quit` | list VMs under `/tmp/ipodvm`; stop one |

Keys: `ret`/`select`, `w`/`menu`, `q`/`up`, `e`/`down`, `a`/`prev`,
`d`/`next`, `s`/`play`.

A successful QMP reply only means the key was delivered. Rows with animated
previews (the main menu's Settings row scrolls a marquee) change the frame on
their own; pass `--region X,Y,W,H` to compare only a stable area such as the
title bar (`--region 0,0,150,20`). The wheel touch (`q e`, or `e q` on the
top row in `goto`) can move the selection at a list edge; the press then
stops and reports instead of retrying.

The firmware sleeps after about two minutes of idle guest time and then
ignores input. Commands report this when the PC is in the IRAM sleep loop
with LR = 0; relaunch the VM. A paused guest gets no run time: work that
needs time (artwork loads, Settings) needs `run N` or `wait`.

## MCP server

```sh
claude mcp add ipodvm -- python3 /path/to/qemu-ipod-classic/tools/ipodvm/mcp_server.py
```

Tools: `disk_prepare`, `vm_launch` (`repo` names the firmware repository;
relative paths resolve against it),
`vm_list`, `vm_status`, `vm_quit`, `vm_run`, `vm_pause`, `screen`, `goto`,
`press`, `select`, `menu`, `screenshot`, `wait`, `profile_keys`,
`profile_dump`, `profile_control` and `hmp`. Results are JSON text; pass
`image: true` (or use `screenshot`) to also receive the PNG. Launch and
navigation take tens of seconds.

## Function profiler

```sh
ipodvm launch --firmware … --disk … --elf .build/payload/payload.elf \
    --profile 'cfw_theme_*' --fn 0x081215d8:UIProcessMessage [--trace] [--signpost]
ipodvm profile keys e e            # reset, send keys, dump, report
ipodvm profile reset|start|stop|dump
```

`--profile GLOB…` selects payload functions from `--elf` (all when no glob is
given; interworking veneers are skipped). `--elf` must be the `payload.elf`
built with `--firmware`: launch compares each selected function's entry
bytes with the image and refuses on a mismatch. Copy the ELF next to the
image when you build it, since later builds overwrite `.build/payload`.
`--fn ADDR:NAME` adds stock-firmware functions (runtime addresses, as in
`payload/native/bindings.h`) and works with any firmware.

The [`funcprof`](../../contrib/plugins/funcprof.c) TCG plugin does the
measurement without halting the CPU. Per function it reports calls,
inclusive and self guest instructions, the largest call, host emulation time,
and unclosed entries. `incl` covers all callees. `self` subtracts watched
callees only, so stock code that a hook calls counts as the hook's own cost
until that stock function is also watched with `--fn`. Hooks entered by a
patched branch (stale LR, resume through `ldr pc`) never return to their
caller and appear only as unclosed.

`--trace` writes `<summary>.trace.json` in Chrome trace format for
<https://ui.perfetto.dev>; `--timeline insns` puts instructions on the time
axis (1 µs = 1 instruction) instead of host time.

### Instruments

`--signpost` emits an `os_signpost` interval per call (subsystem
`org.qemu.ipod.funcprof`, category Points of Interest; the message carries the
function name and instruction count).

```sh
ipodvm instruments --time 20 e e   # Time Profiler on the QEMU process, then keys
```

records with `xctrace` and opens the trace: the Points of Interest lane shows
guest calls next to the host CPU profile of QEMU's own code (TCG, device
models, audio HLE). Any other template works via `--template`.

### What the numbers mean

TCG is not cycle-accurate. Instruction counts are instructions retired by
the emulated CPU, not ARM926 cycles: they ignore caches, memory wait states
and per-instruction cost. Guest timers follow host time, so timer-driven work
and interrupts (which count towards whichever call is open) vary slightly
between runs. Host times measure emulation speed. Use instruction counts to
compare two versions of the same code under the same input; measure absolute
time on the device.
