# Local iPod test runs

Read this file even when launching QEMU from a firmware worktree or a sibling
workspace directory. Follow [the RepriseOS UI test procedure](docs/repriseos-testing.md)
before concluding that input or navigation is broken. Older sibling launchers may
omit the required configuration below.

Start RepriseOS (firmware 2.0.5) test VMs with the complete playback configuration
in [README.md](README.md#playback-configurations), unless the test specifically
needs a bare hardware baseline:

```sh
SHA1_ZERO=1 SM1_IRQ=1 SM1_SYNC=1 SM1_DECODE=1 \
HLE_AUDIO_DATAPATH=1 AUDIOPACE=1 DISP_ALPHA=1 PMUCHARGER=adapter \
HLE_AUDIO_DISK="$reprise_disk" AUDIOWAV="$reprise_run/audio.wav" \
python3 "$reprise_launcher" --headless
```

Add `AUDIOLIVE=1 AUDIOGAIN=20` and `-audio sdl` to hear it on the host.

[`tools/ipodvm`](tools/ipodvm/README.md) applies this configuration and the
input procedure; prefer it over per-run launcher and `remote.py` copies.
`ipodvm disk` builds a test disk with matching theme packs, `ipodvm launch`
boots it in a live window, `ipodvm screen` reads the screen as structured
text, and `ipodvm goto Settings/Theme` navigates by row names with verified
input. `key`, `select`, `menu`, `wait`, `shot` and `quit` cover the rest, and
`mcp_server.py` exposes the same operations as MCP tools. It also provides the
non-halting function and heap profiler.

When launching by hand, set the three `reprise_*` paths for the run first.
`HLE_AUDIO_DISK` must be the same absolute disk-image path passed to QEMU's
`-drive`. Preserve the launcher's RepriseOS payload loader, use `-snapshot`,
and keep a separate QMP socket and log for each VM. Do not replace `SHA1_ZERO`
with `SHA1_OFF`: the latter disables the SHA device and is only for deliberate
compatibility experiments.

Capture audio through `AUDIOWAV`; a running VM or a decoded-track log does not
prove audible output. Verify non-silent samples and inspect the real Now Playing
screen. Do not use `HLE_NP_OVERLAY` for firmware UI verification. Check a screenshot
after each important input because Select/Menu presses can be dropped.

The playback settings are emulator workarounds, not hardware acceptance. Report
elapsed-time, pause/resume, track-change, seek and bookmark limitations honestly.
See [the playback investigation](wiki/20-classic6g-player.md) before changing
the configuration. Keep VM files out of git and never install firmware on hardware
as part of an emulator test. This fork is public: never commit Apple firmware,
decrypted firmware, disk images or device identity backups (NOR, SysInfo), and
keep QMP and the GDB stub on local Unix sockets.
