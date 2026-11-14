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

Requirements: Python 3 (standard library only), the QEMU build in `build/`
and mtools for `disk`. Add `--json` to any command for machine-readable
output.

| File | Contents |
| --- | --- |
| `core.py` | QMP, launch, input, screenshots, test disks, profiler control |
| `screen.py` | screen reading and `goto` navigation |
| `ipodvm.py` | command line |
| `mcp_server.py` | MCP server (stdio) |
| `heap-hooks-2.0.5.txt` | stock 2.0.5 allocator hooks for `launch --heap` |

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
firmware SHA-256, disk, layout), `qemu.log`, `shots/`, profiler output and,
with `--record-audio`, `audio.wav`. The disk is an APFS clone of `--disk`, so
`--disk` is never written, and QEMU runs with `-snapshot`, so the clone is not
written either.
`--persist` drops `-snapshot`: guest writes then reach the run's `disk.img`,
which can be inspected with mtools after `quit`. Launching a name again
replaces its stopped run directory, including screenshots, profiles and the
disk clone. Screen reads go through one scratch screendump per client
(`.frame-<pid>.ppm`), deleted after each read; only `shots/` keeps images.

`prune` deletes stopped run directories launched more than `--days` ago
(default 7), or the stopped runs named on the command line; `--dry-run` only
reports. Running VMs are kept. It reports the space each run holds on its own:
`disk.img` is an APFS clone that shares its blocks with `--disk`, so `du`
overstates what deleting a run frees (about 10 MiB per run, mostly the
firmware copies). Blocks an APFS snapshot still references, such as a Time
Machine local snapshot, count as shared.

Run `launch` from the firmware repository (or pass `--layout`): the payload
offset and load address come from its `payload/native/layout.h`. `launch`
returns once the screen has been non-black and unchanged across two polls
5 s apart (normally Language or the main menu, 45–75 s after start) and leaves
the guest running (`--pause` pauses it, `--no-wait` returns immediately).

`--power` sets the external power the PMU reports: `adapter` (default),
`usb` or `battery`. On the adapter the firmware idles on the Charging screen;
on USB or battery it enters standby after about two minutes idle.
`power adapter|usb|battery` plugs in or unplugs power on a running VM (QMP
`qom-set /machine power`). The PMU raises its plug interrupt, so the firmware
reacts at once: unplugging the adapter leaves the Charging screen, and
plugging in wakes it from standby. `status` reports the current value.

`hold on|off` moves the Hold switch on a running VM (QMP
`qom-set /machine hold true|false`). VMs start unlocked. The PMU raises its
hold interrupt, so the firmware re-reads the switch at once. While locked the
firmware parks the click wheel lines, so wheel and button input is dropped;
a key still wakes the VM from standby.

`battery RAW` sets the raw battery reading, 0–1023, that the PMU's A/D
converter returns on a running VM (QMP `qom-set /machine battery`);
`launch --battery RAW` sets it at start-up (default 844). Apple's scaling of
the reading is not known, so it is given raw. On battery power the firmware
follows a change within a minute or two: at 400 the status-bar battery is red; at 0 it
shows "Low Battery — Connect to Power" and powers off, leaving that screen
in the frame. Standby or a wake at 0 powers off without the screen, so lower
the reading while the VM is awake. The Charging screen on the adapter does
not follow the reading.

`speed N` fast-forwards guest time on a running VM (QMP
`qom-set /machine speed N`, 1–64, 1 = real time). It runs the timer block N
times faster, and the firmware takes its kernel tick (timer B) and its
microsecond counter (timer E) from that block, so timeouts such as the
two-minute standby pass N times sooner. `wait --until asleep` runs at speed
16 by default and restores the previous speed afterwards, also on a timeout
or error, reaching standby in about 10 s; `--speed` overrides the factor and
`run N --speed F` does the same for a fixed run. Device timers (wheel, LCD,
I²C, SM1, I²S, ATA, audio) and the PMU's RTC keep real time, so set `speed 1`
before checking input, navigation or audio. The value survives a guest reset; `status`
reports it.

`launch` opens an SDL window showing the live screen; it also accepts keys
typed into it, and plays the guest's audio through the host's output
(`AUDIOLIVE=1`, `AUDIOGAIN=20`). Headless VMs are silent. `--record-audio`
writes the guest's output to the run's `audio.wav` (16-bit stereo PCM) in
either mode. Paused guests show their last frame. The title (also shown by
`list` and `status`) names the VM, the repository and branch it was launched
from, and the Claude Code session ID prefix when there is one; override it
with `--title`. Cmd+S in the window saves the screen as
`~/Desktop/ipodvm-NAME-YYYYMMDD-HHMMSS.png` and flashes the file name in the
title; this is separate from `shot` and the run's `shots/`. `--headless` runs
without a window.

## Reading the screen

`screen` returns the title, the visible rows (label, value such as `Off`,
second-line detail such as `99 Songs`), the selected row and the preview pane
text. `state` is `list`, `other` (Now Playing, About, …; `text` holds its
strings), `charging` (the idle Charging screen; any key wakes it), `blank` or
`panic` (the RepriseOS payload's OS PANIC screen).

Text is what the firmware draws, in any language and complete even where the
screen truncates or scrolls it. A read stops the guest at the top of its UI
loop, calls `View_InvalidateRect` on the root view and collects every string
of the redraw that follows with breakpoints on `TextView_DrawString`, the
marquee draw and `GraphicsContext_DrawUtf8` (RevolaOS canvas pages); no
breakpoint stays behind. It takes 0.1 to 0.3 s, during which live audio can
gap. A paused guest runs a frame or two and stays stopped.
The addresses are those of firmware 2.0.5; other firmware gives no text.
Neither does a UI loop that does not run (boot, standby, a busy UI task; the
read gives up after a second), and during a screen transition only the title
is drawn. `goto` reads after the screen has settled.

The selected row is the one whose text view has the selected-row class, or
the one under the selection bar when no view has it. Row geometry comes from
the selection bar, which is found from pixels rather
than a fixed colour, so it does not depend on the theme, accent colour or dark
mode: it is the longest run of rows, at most 70 px tall, whose colour at x = 1
or x = 158 (inside the bar on half- and full-width lists, which covers rows
where a scrolling label crosses x = 1) differs strongly from the list
background, trimmed to its first and last row marked at x = 1 (a scrollbar
thumb next to the bar also marks x = 158). The bar's width, followed pixel by
pixel along its top and bottom rows, separates half-width lists with a
preview pane from full-width lists. On lists with artwork the bar is looked
for at the column where the text starts.

## Navigating

`goto Settings/Theme` returns to the main menu with Menu (passing the
Language screen and waking the Charging screen), then for each label scrolls
until that row is selected and opens it. Labels match exactly, then as a
prefix, ignoring case. Rows not on screen are searched downwards, then
upwards; the error lists every row seen. `--here` starts from the current
screen. Selecting a toggle row toggles it.

Wheel keys go in batches of at most three, and each batch is verified by
reading the screen again. Opening a row uses the verified Select below.
Change detection during navigation compares only the list area, without
the preview pane (which animates) and the scrollbar strip at the list's right
edge (shown for about two seconds after scrolling).

## Input

| Command | Behaviour |
| --- | --- |
| `key KEYS…` | resume, send each key and wait for the screen to settle, screenshot; reports whether the frame changed and `woke` if the guest was in standby |
| `select`, `menu` | sample the screen for 1.5 s to find areas that animate on their own, press, and verify the frame changed outside them; if not, run the guest up to `--settle` s (default 25) more, then wheel-touch and retry only if the touch left the frame unchanged |
| `wait --until change\|stable\|asleep` | run the guest until the frame differs from the start, two polls match, or the firmware is in standby (at `--speed`, default 16 for `asleep`) |
| `run [N] [--speed F]`, `pause` | resume (for N seconds, then pause) or pause |
| `shot [FILE]` | screenshot (default: next file in `shots/`) |
| `status` | run state, standby, PC/LR, title, firmware, profiler, power, speed, hold |
| `power adapter\|usb\|battery` | change the external power the PMU reports while running |
| `hold on\|off` | move the Hold switch while running (`on` = locked) |
| `speed N` | run guest time N times faster (1–64) while running |
| `hmp LINE…` | raw HMP command, e.g. `hmp info registers` |
| `list`, `quit` | list VMs under `/tmp/ipodvm`; stop one |
| `prune [NAME…] [--days N] [--dry-run]` | delete stopped run directories |

Keys: `ret`/`select`, `w`/`menu`, `q`/`up`, `e`/`down`, `a`/`prev`,
`d`/`next`, `s`/`play`.

Each key is held 200 ms (`sendkey KEY 200`). The screen then counts as
settled once it has changed and stayed the same for two polls 0.25 s apart,
at least 0.4 s after the key. A key that changes nothing, or a screen that
keeps animating, waits the full limit: 3 s after a wheel key, 5 s after a
button. The screenshot can lag the guest, so an unchanged frame does not prove
a dropped key.

A successful QMP reply only means the key was delivered. Marquee labels,
scrolling captions and animated previews change the frame on their own, so
`select` and `menu` first record which bands of the screen change without
input and leave them out of the comparison (the result's `animated` lists them
as `[x, y, w, h]`). A change of at most 60 rows below the title bar is sampled
again before it counts, in case a marquee was paused during the first sample.
Text changing inside an animated band, such as a value next to a scrolling
label, is not seen. When more than half of the compared area animates, the
press is sent once, not retried, and the result carries a `warning`.
`--region X,Y,W,H` narrows the comparison to a stable area such as the title
bar (`--region 0,0,150,20`). The wheel touch (`q e`, or `e q` on the
top row in `goto`) can move the selection at a list edge; the press then
stops and reports instead of retrying.

`key`, `select`, `menu` and `wait` leave the guest running; `--pause` pauses
it afterwards. A paused guest gets no run time: work that needs time (artwork
loads, Settings) needs `run N` or `wait`.

In standby the firmware has gated its clocks and halted the CPU; the screen
keeps its last frame and `screen` and `status` report `asleep`. Any key wakes
it, and that key is also delivered to the user interface.

When the RepriseOS payload crashes, it draws its OS PANIC screen and halts.
Every poll checks for that screen (only the panic background colour and white,
with white in the "OS PANIC" title box). `key`, `select`, `menu`, `wait` and
`goto` then stop sending input and fail with the panic reason and LR or PC
read from the screen, plus a screenshot.

## Debugging

These commands talk to QEMU's GDB stub on the run's `gdb.sock` directly, so no
GDB is needed. Each one pauses the guest and leaves it paused.

| Command | Behaviour |
| --- | --- |
| `peek ADDR [LEN]` | read memory (default 64 bytes) as little-endian words |
| `poke ADDR HEX…`, `poke ADDR --word VAL…` | write memory, e.g. `poke 0x081215d8 --word 0xe12fff1e` (`bx lr`); prints the bytes it replaced, which undo the patch when written back |
| `break POINT… [--keys KEYS…] [--timeout S] [--hits N] [--dump WHERE:LEN] [--stack LEN]` | run until a point hits; report registers, `--stack` bytes from SP (default 64) and each `--dump` (`0xADDR:LEN` or `r0:LEN`) per hit, then screenshot |
| `step [N]` | single-step N instructions; prints each PC and the final registers |
| `reg NAME VALUE` | set `r0`–`r15`, `sp`, `lr`, `pc` or `cpsr` |

Points: `ADDR` is a breakpoint (an odd address is Thumb), `hw:ADDR` a hardware
breakpoint, and `w:ADDR[:LEN]`, `r:ADDR[:LEN]`, `a:ADDR[:LEN]` write, read
and access watchpoints (default 4 bytes). `--keys` are sent 1 s of guest time
apart once the guest runs, and sending stops at the first hit. A watchpoint
stops before the access: PC is the accessing instruction, memory still holds
the old value (`step` performs the access), and `watch` gives the accessed
address.

Points last only for one `break`: QEMU drops every breakpoint when a client
detaches or sends GDB's initial `?` query, so the tool sends neither. It
removes its points before returning and leaves the guest paused, also after a
timeout (`hit: false` with PC and LR). After a hit, `status` reports the run
state `debug`; `run`, `key` and the other commands resume from it as from a
pause.

Memory writes invalidate translated code, so a `poke` into code takes effect
on the next call. The firmware copies its code into place during boot, so
patch code after boot, or `break` on an early boot address and `poke` at the
hit.

## MCP server

```sh
claude mcp add ipodvm -- python3 /path/to/qemu-ipod-classic/tools/ipodvm/mcp_server.py
```

Tools: `disk_prepare`, `vm_launch` (`repo` names the firmware repository;
relative paths resolve against it),
`vm_list`, `vm_status`, `vm_power`, `vm_hold`, `vm_speed`, `vm_quit`, `vm_run`, `vm_pause`, `vm_prune`, `screen`, `goto`,
`press`, `select`, `menu`, `screenshot`, `wait`, `profile_keys`,
`profile_dump`, `profile_control`, `hmp`, and the debugging tools `mem_read`,
`mem_write`, `debug_break`, `debug_step` and `reg_write`. Results are JSON text; pass
`image: true` (or use `screenshot`) to also receive the PNG. Launch takes
tens of seconds; navigation takes a few seconds per step.

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

### Heap profiling

```sh
ipodvm launch --firmware … --disk … --heap \
    [--symbols <firmware repo>/ghidra/analysis/2.0.5/osos/functions.jsonl] \
    [--elf … --profile 'cfw_*']
ipodvm profile keys ret            # what this input allocated, and what it left live
ipodvm profile heap                # heap section only
```

`--heap` with no hooks tracks the stock 2.0.5 allocator through
[`heap-hooks-2.0.5.txt`](heap-hooks-2.0.5.txt). That file lists the shims in
front of the core allocator and the core free, with their argument registers.
`--heap 0xADDR:KIND[:NAME]…` replaces the list with other hooks. The formats
are described in the plugin header. The plugin tracks every block from the
first guest instruction, so the live totals cover the whole native heap, not
just the recording window.

| Field | Meaning |
|---|---|
| `live_bytes`, `live_count` | Blocks allocated now, overall and per heap instance (the allocator's first argument) |
| `peak_bytes` | Highest live total while recording since the last reset |
| `allocs`, `bytes`, `frees`, `freed_bytes` | Activity while recording since the last reset |
| `failed`, `dropped`, `pending` | Allocations that returned null; allocations whose return was lost because more than 32 were waiting; allocations waiting at dump time |
| Sites | Return addresses of allocation calls. They are named from `--elf` (payload) and `--symbols` (a Ghidra `functions.jsonl` export, for stock code). |
| `holding` | Sites ranked by the live bytes they allocated at any time |
| `leaking` | Sites ranked by blocks allocated while recording since the last reset that are still live, with the largest examples (pointer, size, allocation sequence number) |
| `alloc_calls`, `alloc_bytes`, `free_calls`, `freed_bytes` (per function) | Allocations and tracked frees while that watched function was the innermost open frame |
| `alloc_incl`, `freed_incl` (per function) | The same bytes while it or a watched callee was |

Sizes are the requested sizes. Allocator headers and rounding are not
counted. `untracked_frees` (a free of a pointer that was never seen allocated)
and `replaced` (an allocation that returned a pointer still live) are both 0
when the hooks cover every allocator path. A nonzero value means an allocator
entry point is missing from the hook list. To cross-check a 2.0.5 heap, read
its live-block counter at heap + 0x390: `ipodvm hmp 'xp /1wd 0x…'` should match
that heap's `live_count`.

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
