#!/usr/bin/env python3
"""Launch, drive, read, navigate and profile emulated iPod Classic VMs.

`launch` boots a RepriseOS CFW image (body plus appended payload); the other
commands work with whatever firmware the VM runs. Each VM lives in
/tmp/ipodvm/<name>/. Every command is a one-shot QMP client; there is no
interactive control shell. Add --json for machine-readable output.
See README.md next to this file.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import core  # noqa: E402
import screen  # noqa: E402


def show_screen(data):
    title = data.get('title') or '(no title)'
    print(f'{title}  [{data["state"]}]')
    for row in data.get('rows', []):
        mark = '>' if row.get('selected') else ' '
        text = row['label']
        if row.get('value'):
            text += f' = {row["value"]}'
        if row.get('detail'):
            text += f'  ({row["detail"]})'
        print(f' {mark} {text}')
    if data.get('text'):
        print('  ' + ' | '.join(data['text']))
    if data.get('preview'):
        print(f'  preview: {" | ".join(data["preview"])}')
    if data.get('asleep'):
        print('  asleep: the firmware is in standby; any key wakes it')
    print(f'  shot: {data["shot"]}')


def show_heap(data, top):
    views = core.heap_views(data, top)
    t = views['totals']
    print(f'heap: {t["live_bytes"]:,} B live in {t["live_count"]:,} blocks; since reset '
          f'{t["allocs"]:,} allocs {t["bytes"]:,} B, {t["frees"]:,} frees '
          f'{t["freed_bytes"]:,} B, peak {t["peak_bytes"]:,} B live')
    problems = [f'{t[k]:,} {k.replace("_", " ")}' for k in
                ('failed', 'untracked_frees', 'replaced', 'dropped') if t[k]]
    if problems:
        print(f'  warning: {", ".join(problems)}')
    for h in views['heaps']:
        print(f'  heap {h["heap"]}: {h["live_bytes"]:>12,} B live {h["live_count"]:>8,} blocks'
              f'  peak {h["peak_bytes"]:>12,}  +{h["bytes"]:,} -{h["freed_bytes"]:,}')

    def site_label(s):
        return f'{s["addr"]} {s.get("name") or ""}'[:48]
    tables = [('allocating since reset', 'allocating', ('allocs', 'bytes', 'leak_bytes')),
              ('holding live bytes', 'holding', ('live_count', 'live_bytes')),
              ('leaked since reset', 'leaking', ('leak_count', 'leak_bytes'))]
    for title, key, columns in tables:
        if not views[key]:
            continue
        print(f'{"site (" + title + ")":48} ' + ' '.join(f'{c:>12}' for c in columns))
        for s in views[key]:
            print(f'{site_label(s):48} ' + ' '.join(f'{s[c]:>12,}' for c in columns))
    if views['functions']:
        columns = ('alloc_calls', 'alloc_bytes', 'alloc_incl', 'freed_bytes', 'freed_incl')
        print(f'{"function (heap)":36} ' + ' '.join(f'{c:>12}' for c in columns))
        for f in views['functions']:
            print(f'{f["name"][:36]:36} ' + ' '.join(f'{f[c]:>12,}' for c in columns))
    print('bytes are requested sizes; allocator headers and rounding are not included')


def show_profile(data, top, heap_only=False):
    if heap_only:
        if 'heap' not in data:
            raise core.Failure('this VM was launched without heap hooks (--heap)')
        show_heap(data, top)
        print(f'summary {data["summary"]}')
        return
    total = data['insns'] or 1
    print(f'{data["insns"]:,} guest instructions recorded over '
          f'{data["host_ns"] / 1e9:.1f} s host time')
    print(f'{"function":36} {"calls":>8} {"incl insns":>14} {"%":>6} {"self insns":>14} '
          f'{"avg":>11} {"max":>11} {"host ms":>9}')
    for f in data['functions'][:top]:
        avg = f['incl'] // f['calls'] if f['calls'] else 0
        note = f'  ({f["unclosed"]} unclosed)' if f['unclosed'] else ''
        print(f'{f["name"][:36]:36} {f["calls"]:>8,} {f["incl"]:>14,} '
              f'{100 * f["incl"] / total:>5.1f}% {f["self"]:>14,} {avg:>11,} '
              f'{f["max"]:>11,} {f["host_ns"] / 1e6:>9.1f}{note}')
    print('incl counts watched and unwatched callees; self excludes watched callees only, '
          'so stock code a hook calls is part of its self')
    if 'heap' in data:
        show_heap(data, top)
    if data.get('events_dropped'):
        print(f'warning: {data["events_dropped"]:,} trace events dropped (tracecap)')
    print(f'summary {data["summary"]}')
    if data.get('trace'):
        print(f'trace   {data["trace"]}  (open in https://ui.perfetto.dev)')
    if data.get('shot'):
        print(f'shot    {data["shot"]}')


def show_state(state):
    regs = state['registers']
    names = [f'r{i}' for i in range(13)] + ['sp', 'lr', 'pc', 'cpsr']
    for row in range(0, len(names), 4):
        print('  ' + '  '.join(f'{n:>4}={regs[n]}' for n in names[row:row + 4]))
    for base, lines in state.get('memory', {}).items():
        print(f'  memory at {base}:')
        for line in lines:
            print(f'    {line}')


def show_debug(data):
    if 'pcs' in data:
        print('stepped: ' + ' '.join(data['pcs']))
        show_state(data)
        return
    for number, hit in enumerate(data['hits'], 1):
        watch = f' (access at {hit["watch"]})' if hit.get('watch') else ''
        print(f'hit {number}: {hit["point"] or "unknown point"}{watch}')
        show_state(hit)
    if not data['hit']:
        print(f'no hit ({", ".join(data["points"])}); paused at PC={data["pc"]} LR={data["lr"]}')
    print(f'shot    {data["shot"]}')


def show(command, data, args):
    if args.json:
        print(json.dumps(data, indent=2))
    elif command in ('screen',):
        show_screen(data)
    elif command == 'goto':
        for step in data['steps']:
            print(f'  {step}')
        show_screen(data)
    elif command == 'profile' and 'functions' in data:
        show_profile(data, args.top, args.action == 'heap')
    elif command == 'list':
        for vm in data:
            state = f'running pid {vm["pid"]}' if vm['running'] else 'stopped'
            print(f'{vm["name"]:16} {state:18} {vm["title"]}')
    elif command == 'prune':
        for run in data['removed']:
            print(f'{run["name"]:16} {run["private"] / 2**20:8.1f} MiB own '
                  f'({run["allocated"] / 2**20:.1f} MiB with shared clone blocks)')
        for name in data['running_kept']:
            print(f'{name:16} running, kept')
        print(f'{"would free" if data["dry_run"] else "freed"} '
              f'{data["private_bytes"] / 2**20:.1f} MiB')
    elif command == 'hmp':
        print(data, end='')
    elif command == 'peek':
        print('\n'.join(data['words']) or data['hex'])
    elif command in ('break', 'step'):
        show_debug(data)
    elif command == 'status':
        print(f'{data["name"]}: {data["state"]}{" (asleep)" if data["asleep"] else ""}  '
              f'pid {data["pid"]}  PC={data["pc"]} LR={data["lr"]}')
        print(f'  title    {data["title"]}')
        print(f'  firmware {data["firmware"]["path"]} ({data["firmware"]["sha256"][:12]})')
        if data.get('profile'):
            print(f'  profiling {data["profile"]["functions"]} functions, '
                  f'{data["profile"].get("heap_hooks", 0)} heap hooks')
        print(f'  power    {data["power"]}'
              + ('  persist (guest writes reach the run\'s disk.img)' if data['persist'] else ''))
        print(f'  speed    {data["speed"]}x')
        print(f'  hold     {"on (locked)" if data.get("hold") else "off"}')
        print(f'  battery  {data["battery"]} raw')
    else:
        for key, value in data.items():
            if value is not None:
                print(f'{key}: {value}')


def main():
    parser = argparse.ArgumentParser(prog='ipodvm', description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--json', action='store_true', help='print results as JSON')
    sub = parser.add_subparsers(dest='command', required=True)

    def named(name, help_text):
        p = sub.add_parser(name, help=help_text, description=help_text)
        p.add_argument('-n', '--name', default='vm', help='VM name (default: vm)')
        p.add_argument('--json', action='store_true', default=argparse.SUPPRESS,
                       help='print results as JSON')
        return p

    p = named('launch', 'boot a RepriseOS CFW image with the complete feature-test configuration')
    p.add_argument('--firmware', required=True, help='osos-cfw.bin under test')
    p.add_argument('--disk', required=True, help='test disk image (cloned, never modified)')
    p.add_argument('--layout', help='payload/native/layout.h (default: found from cwd)')
    p.add_argument('--title', help='window title (default: name · repo (branch) · session)')
    p.add_argument('--headless', action='store_true',
                   help='no window (default: SDL window that also accepts keys)')
    p.add_argument('--pause', action='store_true', help='pause after boot')
    p.add_argument('--power', choices=sorted(core.POWER), default='adapter',
                   help='external power the PMU reports (default adapter: idles on the '
                        'Charging screen and never enters standby)')
    p.add_argument('--persist', action='store_true',
                   help='write guest disk changes to the run\'s disk clone (no -snapshot)')
    p.add_argument('--record-audio', action='store_true',
                   help='record the guest\'s audio to the run\'s audio.wav (16-bit stereo PCM)')
    p.add_argument('--battery', type=int, metavar='RAW',
                   help=f'raw battery reading, 0-{core.MAX_BATTERY} '
                        f'(default {core.DEFAULT_BATTERY})')
    p.add_argument('--no-wait', action='store_true', help='return without waiting for boot')
    p.add_argument('--boot-timeout', type=int, default=180)
    p.add_argument('--elf', help='payload.elf matching --firmware, for --profile')
    p.add_argument('--profile', nargs='*', metavar='GLOB',
                   help='profile payload functions matching GLOBs (none = all); needs --elf')
    p.add_argument('--fn', action='append', metavar='0xADDR:NAME',
                   help='also profile a stock-firmware function (repeatable)')
    p.add_argument('--trace', action='store_true', help='record per-call events for Perfetto')
    p.add_argument('--timeline', choices=['host', 'insns'], help='trace time axis (default host)')
    p.add_argument('--signpost', action='store_true', help='emit os_signposts for Instruments')
    p.add_argument('--heap', nargs='*', metavar='0xADDR:KIND[:NAME]',
                   help='track heap allocations through these hooks (none = the stock '
                        '2.0.5 allocator, heap-hooks-2.0.5.txt)')
    p.add_argument('--symbols', metavar='FUNCTIONS_JSONL',
                   help='Ghidra functions.jsonl for naming stock-firmware allocation sites')
    p.set_defaults(func=lambda a: core.launch(
        a.name, a.firmware, a.disk, a.layout, a.title, a.headless, a.pause, not a.no_wait,
        a.boot_timeout, a.elf, a.profile, a.fn, a.trace, a.timeline, a.signpost, a.heap,
        a.symbols, power=a.power, persist=a.persist, battery=a.battery,
        record_audio=a.record_audio))

    p = sub.add_parser('disk', help='clone a test disk and install theme packs')
    p.add_argument('--base', required=True, help='disk image to clone (never modified)')
    p.add_argument('--out', required=True, help='new disk image')
    p.add_argument('--themes', help='directory mirroring /themes (e.g. classic/default.bin)')
    p.add_argument('--keep-themes', action='store_true',
                   help='keep packs already on the base disk')
    p.add_argument('--copy', action='append', default=[], metavar='SRC:/DEST',
                   help='copy a file onto the disk, e.g. build/x.rvx:/Revola/x.rvx')
    p.add_argument('--json', action='store_true', default=argparse.SUPPRESS)
    p.set_defaults(func=lambda a: core.prepare_disk(
        a.base, a.out, a.themes, not a.keep_themes,
        [tuple(c.split(':', 1)) for c in a.copy]))

    p = named('screen', 'read the screen: title, rows, selected row, values')
    p.set_defaults(func=lambda a: screen.screen(a.name))

    p = named('goto', 'navigate by row names, e.g. "Settings/Theme" (from the main menu)')
    p.add_argument('path', help='row labels separated by /')
    p.add_argument('--here', action='store_true', help='start from the current screen')
    p.set_defaults(func=lambda a: screen.goto(a.name, a.path, not a.here))

    p = named('key', 'resume, send keys (each waits for the screen to settle), screenshot')
    p.add_argument('keys', nargs='+', help='ret w q e a d s, or select menu up down prev next play')
    p.add_argument('--pause', action='store_true', help='pause afterwards')
    p.set_defaults(func=lambda a: core.key(a.name, a.keys, a.pause))

    for which in ('select', 'menu'):
        p = named(which, f'press {which.title()} and verify the screen changed, retrying safely')
        p.add_argument('--attempts', type=int, default=3)
        p.add_argument('--settle', type=float, default=25.0,
                       help='extra guest run time before calling a press dropped')
        p.add_argument('--region', metavar='X,Y,W,H',
                       help='compare only this area (e.g. the title bar on animated screens)')
        p.add_argument('--pause', action='store_true', help='pause afterwards')
        p.set_defaults(func=lambda a: core.button(
            a.name, a.command, core.parse_region(a.region), a.attempts, a.settle, a.pause))

    p = named('shot', 'save a screenshot (default: next file in the run\'s shots/)')
    p.add_argument('output', nargs='?')
    p.set_defaults(func=lambda a: core.screenshot(a.name, a.output))

    p = named('wait', 'run the guest until the screen changes or settles')
    p.add_argument('--until', choices=['change', 'stable', 'asleep'], default='stable')
    p.add_argument('--timeout', type=float, default=60)
    p.add_argument('--interval', type=float, default=2)
    p.add_argument('--region', metavar='X,Y,W,H')
    p.add_argument('--pause', action='store_true', help='pause afterwards')
    p.add_argument('--speed', type=int,
                   help=f'guest speed during the wait, restored afterwards '
                        f'(default {core.ASLEEP_SPEED} for --until asleep, else unchanged)')
    p.set_defaults(func=lambda a: core.wait(
        a.name, a.until, a.timeout, a.interval, core.parse_region(a.region), a.pause,
        a.speed))

    p = named('run', 'resume the guest (for N seconds, then pause, if given)')
    p.add_argument('seconds', nargs='?', type=float)
    p.add_argument('--speed', type=int,
                   help='guest speed for the N seconds, restored afterwards')
    p.set_defaults(func=lambda a: core.resume(a.name, a.seconds, a.speed))
    named('pause', 'pause the guest').set_defaults(func=lambda a: core.pause(a.name))
    named('status', 'run state, PC/LR, standby (asleep), power, speed, hold, '
          'battery').set_defaults(
        func=lambda a: core.status(a.name))
    p = named('power', 'plug in or unplug external power on the running VM')
    p.add_argument('power', choices=sorted(core.POWER))
    p.set_defaults(func=lambda a: core.set_power(a.name, a.power))
    p = named('hold', 'move the Hold switch on the running VM (on = locked)')
    p.add_argument('position', choices=['on', 'off'])
    p.set_defaults(func=lambda a: core.set_hold(a.name, a.position == 'on'))
    p = named('battery', 'set the raw battery reading on the running VM')
    p.add_argument('raw', type=int, help=f'0-{core.MAX_BATTERY}')
    p.set_defaults(func=lambda a: core.set_battery(a.name, a.raw))
    p = named('speed', 'run guest time N times faster on the running VM (1 = real time)')
    p.add_argument('factor', type=int)
    p.set_defaults(func=lambda a: core.set_speed(a.name, a.factor))
    named('quit', 'stop the VM').set_defaults(func=lambda a: core.quit_vm(a.name))
    p = named('hmp', 'run a raw HMP command (e.g. "info registers")')
    p.add_argument('line', nargs='+')
    p.set_defaults(func=lambda a: core.hmp(a.name, ' '.join(a.line)))

    p = named('peek', 'read guest memory over the GDB stub')
    p.add_argument('address')
    p.add_argument('length', nargs='?', type=lambda v: int(v, 0), default=64)
    p.set_defaults(func=lambda a: core.peek(a.name, a.address, a.length))
    p = named('poke', 'write guest memory (live patch); prints the bytes it replaced')
    p.add_argument('address')
    p.add_argument('values', nargs='+', help='hex bytes (e.g. 1eff2fe1), or words with --word')
    p.add_argument('--word', action='store_true',
                   help='values are 32-bit little-endian words (e.g. 0xe12fff1e)')
    p.set_defaults(func=lambda a: core.poke(a.name, a.address,
                                            core.poke_bytes(a.values, a.word)))
    p = named('break', 'run until a breakpoint or watchpoint hits and report the state')
    p.add_argument('points', nargs='+', metavar='POINT',
                   help='ADDR (odd = Thumb), hw:ADDR, or w:/r:/a:ADDR[:LEN] watchpoints')
    p.add_argument('--keys', nargs='+', default=[], help='keys to send once the guest runs')
    p.add_argument('--timeout', type=float, default=60)
    p.add_argument('--hits', type=int, default=1, help='hits to collect before stopping')
    p.add_argument('--dump', action='append', metavar='ADDR|REG:LEN',
                   help='memory to read at each hit, e.g. r0:64 (repeatable)')
    p.add_argument('--stack', type=lambda v: int(v, 0), default=64,
                   help='bytes from SP to read at each hit (default 64)')
    p.set_defaults(func=lambda a: core.debug_break(
        a.name, a.points, a.keys, a.timeout, a.dump, a.stack, a.hits))
    p = named('step', 'single-step the paused guest')
    p.add_argument('count', nargs='?', type=int, default=1)
    p.set_defaults(func=lambda a: core.step(a.name, a.count))
    p = named('reg', 'set a register of the paused guest')
    p.add_argument('register', help='r0-r15, sp, lr, pc or cpsr')
    p.add_argument('value')
    p.set_defaults(func=lambda a: core.set_register(a.name, a.register, a.value))
    p = sub.add_parser('list', help='list VMs under /tmp/ipodvm')
    p.add_argument('--json', action='store_true', default=argparse.SUPPRESS)
    p.set_defaults(func=lambda a: core.list_vms())
    p = sub.add_parser('prune', help='delete stopped run directories')
    p.add_argument('names', nargs='*', help='stopped runs to delete (default: by age)')
    p.add_argument('--days', type=float, default=7,
                   help='delete stopped runs launched more than this many days ago')
    p.add_argument('--dry-run', action='store_true', help='only report what would go')
    p.add_argument('--json', action='store_true', default=argparse.SUPPRESS)
    p.set_defaults(func=lambda a: core.prune(a.days, a.names, a.dry_run))

    p = named('profile', 'control the function and heap profiler')
    p.add_argument('action', choices=['dump', 'heap', 'reset', 'start', 'stop', 'keys'],
                   help='heap: dump and show only the heap section')
    p.add_argument('keys', nargs='*', help='for `keys`: inputs to profile')
    p.add_argument('--settle', type=float, default=0,
                   help='for `keys`: extra run time after the last key before the dump')
    p.add_argument('--top', type=int, default=25)
    p.add_argument('--out', help='summary JSON path (default: run dir)')

    def run_profile(a):
        if a.action == 'keys':
            if not a.keys:
                raise core.Failure('profile keys needs at least one key')
            return core.profile_keys(a.name, a.keys, a.settle, a.out)
        if a.action in ('dump', 'heap'):
            return core.profile_dump(a.name, a.out)
        return core.profile_control(a.name, a.action)
    p.set_defaults(func=run_profile)

    p = named('instruments', 'record the QEMU process with xctrace and open it in Instruments')
    p.add_argument('--template', default='Time Profiler')
    p.add_argument('--time', type=int, help='record N seconds (default: until Ctrl-C)')
    p.add_argument('--no-open', action='store_true')
    p.add_argument('keys', nargs='*', help='inputs to send once recording has started')
    p.set_defaults(func=lambda a: core.instruments(
        a.name, a.template, a.time, a.keys, not a.no_open))

    args = parser.parse_args()
    try:
        show(args.command, args.func(args), args)
    except core.Failure as error:
        print(f'ipodvm: {error}', file=sys.stderr)
        raise SystemExit(1)


if __name__ == '__main__':
    main()
