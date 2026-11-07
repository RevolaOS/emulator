# RepriseOS UI testing

Use the complete configuration in [AGENTS.md](../AGENTS.md) and
[Running the music player](../README.md#running-the-music-player), including
`PMUCHARGER=adapter` and `DISP_ALPHA=1`. A launcher that boots without these
settings is not the established feature-test setup.

## Prepare the run

- Build the firmware under test and extract its body and appended payload from
  that same image. Read the firmware repository's `payload/native/layout.h` for
  current offsets and the payload load address.
- Use a known test disk with matching resources and theme packs. Supply the same
  absolute image path to `-drive` and `HLE_AUDIO_DISK`.
- Use `-snapshot` and a separate run directory, QMP socket and log. Do not reuse
  another running VM's socket or modify its disk.
- Record the command, environment overrides and firmware revision with the
  screenshots. Leave firmware binaries and disk images out of Git.

## Drive the UI

[`tools/ipodvm`](../tools/ipodvm/README.md) implements the steps below:
`launch` waits for the first stable screen, `key` resumes, sends and waits,
and `select`/`menu` verify the frame changed before a guarded retry. ipodvm
leaves the guest running; pass `--pause` to stop it after a command.

1. Let the VM run until a screenshot shows Language or the main menu. Startup
   can take roughly 60 seconds; a black frame at 20 or 30 seconds is not failure.
   If QMP reports the VM paused, send `cont` before waiting.
2. For each navigation step, send `cont`, then HMP `sendkey <key> 200` through
   QMP. Wait 3 seconds after a wheel key or 5 seconds after a button, then `stop`
   (ipodvm continues sooner once the screen has changed and settled).
3. Capture and inspect a screenshot while paused before choosing the next action.
   A successful QMP reply confirms command delivery, not guest acceptance.
4. If Select/Menu is dropped, verify the current screen and retry. A wheel touch
   can help, but can also change selection; inspect the result before continuing.
5. Quit only the VM created for this test when finished.

| Key | Action |
| --- | --- |
| `ret` | Select |
| `w` | Menu/back |
| `q` / `e` | Wheel up/down |
| `a` / `d` | Previous/next |
| `s` | Play/pause |

Pause between review steps so time spent inspecting screenshots does not leave
an unattended guest running. Resume before sending inputs; never send a long
sequence based on an unverified selection.

## If navigation appears stuck

Compare the environment, disk, payload loader and input timing with the
configuration above before attributing a failure to firmware or declaring
emulation unusable. A launcher without the complete configuration can stall at
the Language screen.

- A paused guest gets no run time. Slow screens (Settings, artwork loads) need
  20–30 s of running guest time (`ipodvm wait` or `ipodvm run N`).
- The idle Charging screen wakes on any key. On USB or battery power
  (`ipodvm launch --power usb|battery`) the firmware enters standby after
  about two minutes idle; `ipodvm status` reports `asleep`, and any key wakes
  it.
- Theme packs built for another catalog are rejected and the Theme menu is
  hidden. Build the disk with packs from the same firmware build
  (`ipodvm disk`).

## Limits

These settings include hardware and audio workarounds. `IRAMSPILL` is a measurement
aid, and host-assisted audio is not proof of hardware playback behavior. Do not
use `HLE_NP_OVERLAY` to validate firmware UI. UI screenshots establish rendering
and navigation only; audio, timing, memory and physical-device acceptance require
separate checks. See [the playback investigation](../wiki/20-classic6g-player.md).
