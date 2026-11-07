"""VM lifecycle, input, screenshots and profiling for iPod Classic RepriseOS VMs.

Functions return plain data and report progress through `log` (stderr), so
the CLI and the MCP server can share them. Each VM lives in
/tmp/ipodvm/<name>/, short enough for macOS's 104-byte Unix socket limit.
"""
import bisect
import contextlib
import ctypes
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import time

TOOL = Path(__file__).resolve().parent
QEMU_ROOT = TOOL.parent.parent
QEMU = QEMU_ROOT / 'build/qemu-system-arm'
PLUGIN_DIR = QEMU_ROOT / 'build/contrib/plugins'
PLUGIN = PLUGIN_DIR / 'libfuncprof.so'
RUNS = Path('/tmp/ipodvm')

# IMG1 header stripped for QEMU's raw -kernel load at 0x08000000.
IMG1_HEADER = 0x800

# Complete feature-test configuration (AGENTS.md, README "Running the music player").
ENVIRONMENT = dict(
    IRAMSPILL='0x10000', SHA1_ZERO='1', SM1_IRQ='1', SM1_SLOTS='1',
    SM1_NSLOTS='2', SM1_DECODE='1', SM1_FEED_OFF='1', DISP_ALPHA='1',
    PMUCHARGER='adapter', HLE_AUDIO_DATAPATH='1', HLE_AUDIO_SUSTAIN='1',
)

WHEEL_KEYS = {'q', 'e'}
# After a key, input waits until the frame has changed and then held for
# SETTLE_HOLDS polls POLL s apart, but at least KEY_FLOOR s (past the 200 ms
# hold) and at most WHEEL_WAIT or BUTTON_WAIT s.
WHEEL_WAIT, BUTTON_WAIT = 3.0, 5.0
POLL, SETTLE_HOLDS, KEY_FLOOR = 0.25, 2, 0.4
KEY_NAMES = {'select': 'ret', 'menu': 'w', 'up': 'q', 'down': 'e',
             'prev': 'a', 'next': 'd', 'play': 's'}
VALID_KEYS = set(KEY_NAMES) | set(KEY_NAMES.values())

# Firmware 2.0.5 standby: after about two minutes idle on battery or USB power
# 0x22002dcc gates the clocks and halts in WFI at 0x22002e70 (QEMU reports the
# PC after it). Any key wakes it.
STANDBY_WFI = range(0x22002e6c, 0x22002e78)

# --power: what the PMU reports as external power (PMUCHARGER at start-up, the
# machine's "power" property while running). The firmware only enters standby
# without the wall adapter; on the adapter it idles on the Charging screen.
POWER = {'adapter': 'adapter', 'usb': 'usb', 'battery': None}

# speed: the machine's "speed" property runs the timer block, and with it the
# firmware's kernel tick and microsecond counter, up to 64x faster.
MAX_SPEED = 64


class Failure(Exception):
    pass


def log(message):
    print(message, file=sys.stderr, flush=True)


# ---- QMP ------------------------------------------------------------------

class QMP:
    def __init__(self, run):
        self.run = run
        # One scratch screendump per client, deleted after every read. QEMU can
        # still write one after its client timed out and exited; the next
        # client removes those.
        self.scratch = run / f'.frame-{os.getpid()}.ppm'
        for stale in run.glob('.frame-*.ppm'):
            pid = stale.stem.removeprefix('.frame-')
            if not (pid.isdigit() and pid_alive(int(pid))):
                stale.unlink(missing_ok=True)
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(30)
        try:
            self.socket.connect(str(run / 'qmp.sock'))
            self.stream = self.socket.makefile('rwb', buffering=0)
            if 'QMP' not in json.loads(self.stream.readline()):
                raise Failure('unexpected QMP greeting')
            self.execute('qmp_capabilities')
        except OSError as error:
            self.socket.close()
            raise Failure(f'{run.name}: cannot reach QMP ({error}); is the VM running?')
        except Failure:
            self.socket.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.socket.close()
        # A timed-out screendump can still be written after the call failed.
        self.scratch.unlink(missing_ok=True)

    def execute(self, name, arguments=None):
        message = {'execute': name}
        if arguments is not None:
            message['arguments'] = arguments
        self.stream.write((json.dumps(message) + '\n').encode())
        while True:
            try:
                line = self.stream.readline()
            except TimeoutError:
                raise Failure(f'{self.run.name}: QMP {name} timed out; '
                              'is QEMU stopped or hung?')
            if not line:
                raise Failure('QMP disconnected')
            reply = json.loads(line)
            if 'error' in reply:
                raise Failure(f"QMP {name}: {reply['error'].get('desc', reply['error'])}")
            if 'return' in reply:
                return reply['return']

    def hmp(self, line):
        return self.execute('human-monitor-command', {'command-line': line})

    def running(self):
        return self.execute('query-status')['running']

    def cont(self):
        self.execute('cont')

    def stop(self):
        self.execute('stop')

    def run_for(self, seconds):
        """Give the guest real run time, then pause it again."""
        self.cont()
        try:
            time.sleep(seconds)
        finally:
            self.stop()

    def frame(self):
        """Return (width, height, rgb bytes) of the current display."""
        try:
            self.execute('screendump', {'filename': str(self.scratch)})
            return read_ppm(self.scratch.read_bytes())
        finally:
            self.scratch.unlink(missing_ok=True)

    def png(self, path):
        self.execute('screendump', {'filename': str(path), 'format': 'png'})
        return path

    def shot(self, label):
        return self.png(next_shot(self.run, label))

    def registers(self):
        text = self.hmp('info registers')
        regs = {f'r{int(n)}': int(v, 16)
                for n, v in re.findall(r'\bR(\d\d)=([0-9a-fA-F]{8})', text)}
        regs.update(sp=regs.get('r13', 0), lr=regs.get('r14', 0), pc=regs.get('r15', 0))
        return regs


def read_ppm(data):
    fields, offset = [], 0
    while len(fields) < 4:
        while data[offset:offset + 1].isspace():
            offset += 1
        if data[offset:offset + 1] == b'#':
            offset = data.index(b'\n', offset) + 1
            continue
        end = offset
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[offset:end])
        offset = end
    if fields[0] != b'P6' or fields[3] != b'255':
        raise Failure('unsupported screendump format')
    width, height = int(fields[1]), int(fields[2])
    return width, height, data[offset + 1:offset + 1 + width * height * 3]


def digest(frame, region=None):
    width, height, rgb = frame
    if region is None:
        return hashlib.sha1(rgb).hexdigest()
    x, y, w, h = region
    rows = (rgb[((y + row) * width + x) * 3:((y + row) * width + x + w) * 3]
            for row in range(h) if y + row < height)
    return hashlib.sha1(b''.join(rows)).hexdigest()


def is_blank(frame):
    rgb = frame[2]
    return rgb.count(0) == len(rgb)


def parse_region(text):
    return tuple(int(v) for v in text.split(',')) if text else None


# ---- run directories -------------------------------------------------------

def run_dir(name):
    run = RUNS / name
    if not (run / 'launch.json').exists():
        raise Failure(f'no VM named {name!r}; see `ipodvm list`')
    return run


def pid_alive(pid):
    # A long-lived parent (the MCP server) must reap its exited QEMU children,
    # or they linger as zombies that still answer kill(pid, 0).
    try:
        if os.waitpid(pid, os.WNOHANG)[0] == pid:
            return False
    except ChildProcessError:
        pass
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def vm_pid(run):
    try:
        pid = int((run / 'pid').read_text())
    except (FileNotFoundError, ValueError):
        return None
    return pid if pid_alive(pid) else None


def next_shot(run, label):
    shots = run / 'shots'
    shots.mkdir(exist_ok=True)
    index = len(list(shots.glob('*.png')))
    safe = re.sub(r'[^A-Za-z0-9_-]+', '-', label).strip('-') or 'shot'
    return shots / f'{index:03d}-{safe}.png'


def parse_layout(path):
    text = Path(path).read_text()
    values = {}
    for key in ('CFW_PAYLOAD_BASE', 'CFW_PAYLOAD_FILE_OFFSET'):
        match = re.search(rf'#define\s+{key}\s+(0x[0-9a-fA-F]+)', text)
        if not match:
            raise Failure(f'{path}: {key} not found')
        values[key] = int(match.group(1), 16)
    return values['CFW_PAYLOAD_BASE'], values['CFW_PAYLOAD_FILE_OFFSET']


def find_layout(start):
    for directory in [start, *start.parents]:
        candidate = directory / 'payload/native/layout.h'
        if candidate.is_file():
            return candidate
    return None


def owner_label(name, cwd):
    """Window title naming the VM, where it was launched from and by which session."""
    try:
        top = subprocess.run(['git', 'rev-parse', '--show-toplevel'], cwd=cwd, text=True,
                             capture_output=True, check=True).stdout.strip()
        branch = subprocess.run(['git', 'branch', '--show-current'], cwd=cwd, text=True,
                                capture_output=True, check=True).stdout.strip()
        place = Path(top).name + (f' ({branch})' if branch else '')
    except (subprocess.CalledProcessError, FileNotFoundError):
        place = Path(cwd).name
    parts = [name, place]
    session = os.environ.get('CLAUDE_CODE_SESSION_ID')
    if session:
        parts.append(f'claude {session[:8]}')
    return ' · '.join(parts)


# ---- ELF symbols -----------------------------------------------------------

def elf_sections(data):
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x2e)
    return [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize)
            for i in range(shnum)]


def elf_functions(data):
    """Return [(addr, size, name)] for STT_FUNC symbols of a 32-bit LE ELF."""
    sections = elf_sections(data)
    functions = []
    for section in sections:
        if section[1] != 2:          # SHT_SYMTAB
            continue
        strtab = sections[section[6]]
        for offset in range(section[4], section[4] + section[5], 16):
            name, value, size, info, _, _ = struct.unpack_from('<IIIBBH', data, offset)
            if info & 0xf != 2 or value == 0:   # STT_FUNC
                continue
            start = strtab[4] + name
            label = data[start:data.index(b'\0', start)].decode()
            functions.append((value & ~1, size, label))
    return sorted(set(functions))


def elf_bytes(data, addr, length):
    for s in elf_sections(data):
        kind, sh_addr, sh_offset, sh_size = s[1], s[3], s[4], s[5]
        if kind == 1 and sh_addr <= addr and addr + length <= sh_addr + sh_size:
            return data[sh_offset + addr - sh_addr:sh_offset + addr - sh_addr + length]
    return None


def profile_symbols(elf, patterns, fns, image, base, file_offset):
    """Select watched functions and check the ELF describes this image's code."""
    entries = []
    if patterns is not None:
        data = Path(elf).read_bytes()
        if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
            raise Failure(f'{elf}: not a 32-bit little-endian ELF')
        # Linker interworking veneers only tail-jump to the real function.
        # Only code inside the loaded payload can be checked against the image.
        payload_end = base + len(image) - file_offset
        functions = [f for f in elf_functions(data)
                     if not re.fullmatch(r'__.+_from_(arm|thumb)', f[2])
                     and base <= f[0] < payload_end]
        patterns = patterns or ['*']    # --profile without globs: every function
        chosen = [f for f in functions if any(fnmatch.fnmatch(f[2], p) for p in patterns)]
        if not chosen:
            raise Failure(f'no functions in {elf} match {patterns}')
        payload = image[file_offset:]
        mismatched = []
        for addr, size, name in chosen:
            length = max(4, min(size, 16))
            if elf_bytes(data, addr, length) != payload[addr - base:addr - base + length]:
                mismatched.append(name)
        if mismatched:
            raise Failure(f'{elf} does not match the firmware payload '
                          f'({len(mismatched)} functions differ, e.g. '
                          f'{", ".join(mismatched[:5])}); rebuild or pass the matching ELF')
        entries += [(addr, name) for addr, _, name in chosen]
    for spec in fns or []:
        addr, _, name = spec.partition(':')
        entries.append((int(addr, 0), name or addr))
    unique = {}
    for addr, name in entries:      # aliases share an entry; keep the first name
        unique.setdefault(addr, name)
    return sorted(unique.items())


_REG = r'/r(?:[0-9]|1[0-2])'
HEAP_KIND = re.compile(rf'(?:alloc|free)(?:{_REG}){{0,2}}|realloc(?:(?:{_REG}){{2,3}})?')


HEAP_PRESET = TOOL / 'heap-hooks-2.0.5.txt'


def heap_hooks(specs):
    """Parse heap hooks "0xADDR:KIND[:NAME]"; [] means the stock 2.0.5 preset."""
    hooks = []
    if specs == []:
        for line in HEAP_PRESET.read_text().splitlines():
            line = line.split('#', 1)[0].strip()
            if line:
                addr, kind, *name = line.split(None, 2)
                hooks.append((addr, kind, name[0] if name else kind))
    else:
        for spec in specs or []:
            addr, _, rest = spec.partition(':')
            kind, _, name = rest.partition(':')
            hooks.append((addr, kind, name or kind))
    parsed = []
    for addr, kind, name in hooks:
        try:
            value = int(addr, 0)
        except ValueError:
            value = None
        if value is None or not HEAP_KIND.fullmatch(kind):
            raise Failure(f'bad heap hook {addr} {kind}; expected 0xADDR alloc[/SIZE[/HEAP]], '
                          'free[/PTR[/HEAP]] or realloc[/PTR/SIZE[/HEAP]] with registers r0-r12')
        parsed.append((value, kind, name))
    duplicates = sorted({a for a, _, _ in parsed if [p[0] for p in parsed].count(a) > 1})
    if duplicates:
        raise Failure(f'duplicate heap hooks: {", ".join(f"{a:#010x}" for a in duplicates)}')
    return parsed


def ensure_plugin():
    source = QEMU_ROOT / 'contrib/plugins/funcprof.c'
    if not PLUGIN.exists() or PLUGIN.stat().st_mtime < source.stat().st_mtime:
        subprocess.run(['make', '-C', str(PLUGIN_DIR), PLUGIN.name], check=True,
                       stdout=subprocess.DEVNULL)


# ---- test disks ------------------------------------------------------------

def fat_offset(disk):
    """Byte offset of the disk's FAT32 volume.

    iPod Classic disks use 4 KiB sectors, so MBR LBAs are tried as both 512-
    and 4096-byte units and confirmed by the FAT32 boot-sector signature.
    """
    with open(disk, 'rb') as f:
        mbr = f.read(512)
        if mbr[510:512] != b'\x55\xaa':
            raise Failure(f'{disk}: no MBR')
        for i in range(4):
            lba, = struct.unpack_from('<I', mbr, 0x1be + 16 * i + 8)
            for sector in (4096, 512):
                if lba:
                    f.seek(lba * sector + 0x52)
                    if f.read(8) == b'FAT32   ':
                        return lba * sector
    raise Failure(f'{disk}: no FAT32 volume found')


def prepare_disk(base, out, themes=None, clean=True):
    """Clone `base` to `out` and install theme packs from `themes`.

    `themes` mirrors the device's /themes tree (classic/default.bin,
    classic/dark.bin, <id>/theme.bin, ...); only .bin files are copied, as the
    installer does. With `clean`, existing /themes entries are removed first so
    stale packs from older catalogs cannot be discovered.
    """
    base, out = Path(base).expanduser().resolve(), Path(out).expanduser().resolve()
    if base == out:
        raise Failure('output must differ from the base disk')
    for tool in ('mcopy', 'mmd', 'mdeltree', 'mdir'):
        if not shutil.which(tool):
            raise Failure(f'{tool} not found; install mtools (brew install mtools)')
    out.unlink(missing_ok=True)
    subprocess.run(['cp', '-c', str(base), str(out)], check=True)
    volume = f'{out}@@{fat_offset(out)}'
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')   # iPod media byte 0xf2

    # mmd asks how to resolve an existing folder on the controlling terminal,
    # even with stdin redirected, so every mmd call passes `-D s` (skip).
    def mtools(*command, check=True):
        return subprocess.run(command, env=env, stdin=subprocess.DEVNULL, capture_output=True,
                              text=True, check=check)

    installed = []
    if themes:
        themes = Path(themes).expanduser().resolve()
        packs = sorted(p for p in themes.rglob('*.bin') if p.is_file())
        if not packs:
            raise Failure(f'no .bin packs under {themes}')
        mtools('mmd', '-D', 's', '-i', volume, '::/themes', check=False)
        if clean:
            listing = mtools('mdir', '-b', '-i', volume, '::/themes', check=False).stdout
            for entry in filter(None, (line.strip() for line in listing.splitlines())):
                tool = 'mdeltree' if entry.endswith('/') else 'mdel'
                mtools(tool, '-i', volume, entry.rstrip('/'))
            left = mtools('mdir', '-b', '-i', volume, '::/themes', check=False).stdout.strip()
            if left:
                raise Failure(f'could not clear /themes on {out}: {left}')
        for pack in packs:
            relative = pack.relative_to(themes)
            folder = '::/themes'
            for part in relative.parts[:-1]:
                folder += f'/{part}'
                mtools('mmd', '-D', 's', '-i', volume, folder, check=False)
            mtools('mcopy', '-o', '-i', volume, str(pack), f'{folder}/{relative.name}')
            installed.append(f'/themes/{relative.as_posix()}')
    return {'disk': str(out), 'fat_offset': fat_offset(out), 'installed': installed}


# ---- lifecycle -------------------------------------------------------------

def launch(name, firmware, disk, layout=None, title=None, headless=False, pause=False,
           wait=True, boot_timeout=180, elf=None, profile=None, fns=None, trace=False,
           timeline=None, signpost=False, heap=None, symbols=None, cwd=None,
           power='adapter', persist=False):
    """Start a VM; with `wait`, return once the first screen is stable."""
    cwd = Path(cwd or Path.cwd())
    firmware = Path(firmware).expanduser().resolve()
    disk = Path(disk).expanduser().resolve()
    layout = Path(layout) if layout else find_layout(cwd)
    if layout is None and elf:
        layout = find_layout(Path(elf).resolve().parent)
    if layout is None:
        raise Failure('payload/native/layout.h not found; run from the firmware '
                      'repository or pass a layout path')
    if profile is not None and not elf:
        raise Failure('profiling payload functions needs the matching payload.elf')
    if power not in POWER:
        raise Failure(f'unknown power {power!r}; use {sorted(POWER)}')
    base, file_offset = parse_layout(layout)
    if not QEMU.exists():
        raise Failure(f'{QEMU} missing; build QEMU first (see README.md)')

    RUNS.mkdir(exist_ok=True)
    run = RUNS / name
    if run.exists():
        if vm_pid(run):
            raise Failure(f'{name} is already running; quit it first')
        shutil.rmtree(run)
    run.mkdir(parents=True)

    image = firmware.read_bytes()
    (run / 'body.bin').write_bytes(image[IMG1_HEADER:])
    (run / 'payload.bin').write_bytes(image[file_offset:])
    # APFS clone: instant, and --disk is never written. Without `persist`,
    # -snapshot keeps guest writes off the clone too.
    subprocess.run(['cp', '-c', str(disk), str(run / 'disk.img')], check=True)

    environment = dict(ENVIRONMENT, HLE_AUDIO_DISK=str(run / 'disk.img'),
                       AUDIOWAV=str(run / 'audio.wav'))
    environment.pop('PMUCHARGER', None)
    if POWER[power]:
        environment['PMUCHARGER'] = POWER[power]
    title = title or owner_label(name, cwd)
    # QEMU option values escape a comma by doubling it.
    command = [str(QEMU), '-name', title.replace(',', ',,'), '-M', 'iPod-Classic',
               '-kernel', str(run / 'body.bin'),
               '-device', f'loader,file={run / "payload.bin"},addr={base:#010x},force-raw=on',
               '-drive', f'if=ide,format=raw,file={run / "disk.img"}',
               *([] if persist else ['-snapshot']),
               '-serial', 'null', '-monitor', 'none',
               '-qmp', f'unix:{run / "qmp.sock"},server=on,wait=off',
               '-gdb', f'unix:{run / "gdb.sock"},server=on,wait=off',
               '-no-reboot', '-no-shutdown',
               '-display', 'none' if headless else 'sdl']

    profiling = None
    hooks = heap_hooks(heap) if heap is not None else []
    if symbols and not hooks:
        raise Failure('--symbols names heap allocation sites; it needs --heap')
    if symbols:
        symbols = Path(symbols).expanduser().resolve()
        if not symbols.is_file():
            raise Failure(f'{symbols} not found')
    if profile is not None or fns or hooks:
        entries = profile_symbols(elf, profile, fns, image, base, file_offset)
        ensure_plugin()
        os.mkfifo(run / 'profile.ctl')
        plugin = [str(PLUGIN), f'ctl={run / "profile.ctl"}', f'out={run / "profile-exit.json"}']
        if entries:
            watched = run / 'profile.syms'
            watched.write_text(''.join(f'{addr:#010x} {label}\n' for addr, label in entries))
            plugin.append(f'syms={watched}')
        if hooks:
            hook_file = run / 'profile.heap'
            hook_file.write_text(''.join(f'{addr:#010x} {kind} {label}\n'
                                         for addr, kind, label in hooks))
            plugin.append(f'heaps={hook_file}')
        if trace:
            plugin.append('trace=on')
        if timeline:
            plugin.append(f'timeline={timeline}')
        if signpost:
            plugin.append('signpost=on')
        command += ['-plugin', ','.join(plugin)]
        profiling = {'functions': len(entries), 'heap_hooks': len(hooks),
                     'elf': str(Path(elf).resolve()) if elf else None,
                     'symbols': str(symbols) if symbols else None}

    record = {'title': title, 'command': command, 'environment': environment,
              'firmware': {'path': str(firmware), 'sha256': hashlib.sha256(image).hexdigest()},
              'disk': str(disk), 'layout': str(layout.resolve()), 'profile': profiling,
              'power': power, 'persist': persist}
    (run / 'launch.json').write_text(json.dumps(record, indent=2) + '\n')

    with (run / 'qemu.log').open('w') as output:
        process = subprocess.Popen(command, env=dict(os.environ, **environment), stdout=output,
                                   stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                   start_new_session=True, cwd=run)
    (run / 'pid').write_text(str(process.pid))
    log(f'{name}: QEMU pid {process.pid}, run dir {run}')

    deadline = time.time() + 15
    while not (run / 'qmp.sock').exists():
        if process.poll() is not None:
            raise Failure(f'QEMU exited ({process.returncode}); see {run / "qemu.log"}:\n'
                          + (run / 'qemu.log').read_text()[-2000:])
        if time.time() > deadline:
            raise Failure('QMP socket did not appear')
        time.sleep(0.2)
    time.sleep(0.3)
    result = {'name': name, 'pid': process.pid, 'run': str(run), 'title': title,
              'profile': profiling, 'power': power,
              'disk': str(run / 'disk.img') if persist else None}
    if not wait:
        result['state'] = 'booting'
        return result

    log(f'{name}: waiting for the first stable screen (boot takes ~45-75 s)...')
    with QMP(run) as qmp:
        started, previous = time.time(), None
        while True:
            time.sleep(5)
            frame = qmp.frame()
            current = None if is_blank(frame) else digest(frame)
            if current and current == previous:
                break
            previous = current
            if time.time() - started > boot_timeout:
                raise Failure(f'no stable screen after {boot_timeout} s; '
                              f'take a screenshot and check {run / "qemu.log"}')
        if pause:
            qmp.stop()
        result.update(state='paused' if pause else 'running',
                      boot_seconds=round(time.time() - started), shot=str(qmp.shot('boot')))
    return result


def status(name):
    run = run_dir(name)
    record = json.loads((run / 'launch.json').read_text())
    with QMP(run) as qmp:
        state = qmp.execute('query-status')['status']
        regs = qmp.registers()
    return {'name': name, 'state': state, 'asleep': asleep(regs), 'pid': vm_pid(run),
            'pc': f'{regs["pc"]:#010x}', 'lr': f'{regs["lr"]:#010x}',
            'title': record.get('title', ''), 'firmware': record['firmware'],
            'profile': record.get('profile'), 'power': record.get('power', 'adapter'),
            'persist': record.get('persist', False), 'speed': record.get('speed', 1),
            'run': str(run)}


def set_power(name, power):
    """Plug in or unplug external power on a running VM."""
    if power not in POWER:
        raise Failure(f'unknown power {power!r}; use {sorted(POWER)}')
    run = run_dir(name)
    with QMP(run) as qmp:
        previous = qmp.execute('qom-get', {'path': '/machine', 'property': 'power'})
        qmp.execute('qom-set', {'path': '/machine', 'property': 'power', 'value': power})
        current = qmp.execute('qom-get', {'path': '/machine', 'property': 'power'})
    record = json.loads((run / 'launch.json').read_text())
    record['power'] = current
    (run / 'launch.json').write_text(json.dumps(record, indent=2) + '\n')
    return {'name': name, 'power': current, 'previous': previous}


def apply_speed(qmp, factor):
    """Set the machine's timer speed; return (previous, current)."""
    if not 1 <= factor <= MAX_SPEED:
        raise Failure(f'speed must be 1 to {MAX_SPEED}, not {factor}')
    previous = qmp.execute('qom-get', {'path': '/machine', 'property': 'speed'})
    qmp.execute('qom-set', {'path': '/machine', 'property': 'speed', 'value': factor})
    current = qmp.execute('qom-get', {'path': '/machine', 'property': 'speed'})
    record = json.loads((qmp.run / 'launch.json').read_text())
    record['speed'] = current
    (qmp.run / 'launch.json').write_text(json.dumps(record, indent=2) + '\n')
    return previous, current


@contextlib.contextmanager
def speed_for(qmp, factor):
    """Run the block at `factor` (None: unchanged), then restore the previous speed."""
    if factor is None:
        yield
        return
    previous, _ = apply_speed(qmp, factor)
    try:
        yield
    finally:
        apply_speed(qmp, previous)


def set_speed(name, factor):
    """Run guest time `factor` times faster on a running VM (1 = real time)."""
    with QMP(run_dir(name)) as qmp:
        previous, current = apply_speed(qmp, factor)
    return {'name': name, 'speed': current, 'previous': previous}


def list_vms():
    if not RUNS.exists():
        return []
    vms = []
    for run in sorted(RUNS.iterdir()):
        if (run / 'launch.json').exists():
            record = json.loads((run / 'launch.json').read_text())
            pid = vm_pid(run)
            vms.append({'name': run.name, 'running': pid is not None, 'pid': pid,
                        'title': record.get('title', ''), 'run': str(run)})
    return vms


def quit_vm(name):
    run = run_dir(name)
    pid = vm_pid(run)
    if pid is None:
        return {'name': name, 'state': 'not running'}
    with QMP(run) as qmp:
        try:
            qmp.execute('quit')
        except Failure:
            pass
    for _ in range(50):
        if not pid_alive(pid):
            return {'name': name, 'state': 'stopped'}
        time.sleep(0.1)
    raise Failure(f'{name}: QEMU pid {pid} still running 5 s after quit')


def private_size(path):
    """Bytes only this file holds on APFS (clones share the rest), or None."""
    class AttrList(ctypes.Structure):
        _fields_ = [('bitmapcount', ctypes.c_ushort), ('reserved', ctypes.c_uint16),
                    ('commonattr', ctypes.c_uint32), ('volattr', ctypes.c_uint32),
                    ('dirattr', ctypes.c_uint32), ('fileattr', ctypes.c_uint32),
                    ('forkattr', ctypes.c_uint32)]
    returned, private = 0x80000000, 0x8   # ATTR_CMN_RETURNED_ATTRS, ATTR_CMNEXT_PRIVATESIZE
    request = AttrList(5, 0, returned, 0, 0, 0, private)
    buffer = ctypes.create_string_buffer(64)
    libc = ctypes.CDLL(None)
    # FSOPT_NOFOLLOW | FSOPT_ATTR_CMN_EXTENDED
    if libc.getattrlist(str(path).encode(), ctypes.byref(request), buffer, len(buffer), 0x21):
        return None
    attributes = struct.unpack_from('<5I', buffer, 4)
    return struct.unpack_from('<q', buffer, 24)[0] if attributes[4] & private else None


def run_usage(run):
    """(allocated, private) bytes of a run directory."""
    allocated = private = 0
    for path in run.rglob('*'):
        if path.is_file() and not path.is_symlink():
            blocks = path.lstat().st_blocks * 512
            allocated += blocks
            size = private_size(path) if sys.platform == 'darwin' else None
            private += blocks if size is None else size
    return allocated, private


def prune(days=7.0, names=None, dry_run=False):
    """Delete stopped runs launched more than `days` ago, or the named stopped runs."""
    if names:
        runs = [run_dir(name) for name in names]
    else:
        cutoff = time.time() - days * 86400
        runs = [r for r in sorted(RUNS.iterdir()) if (r / 'launch.json').exists()
                and (r / 'launch.json').stat().st_mtime < cutoff] if RUNS.exists() else []
    removed, kept, freed = [], [], 0
    for run in runs:
        if vm_pid(run):
            kept.append(run.name)
            continue
        allocated, private = run_usage(run)
        removed.append({'name': run.name, 'allocated': allocated, 'private': private})
        freed += private
        if not dry_run:
            shutil.rmtree(run)
    return {'removed': removed, 'running_kept': kept, 'private_bytes': freed,
            'dry_run': dry_run}


def resume(name, seconds=None, speed=None):
    """Resume; with `seconds`, run that many host seconds (at `speed`, then
    restored) and pause again."""
    with QMP(run_dir(name)) as qmp:
        if seconds:
            with speed_for(qmp, speed):
                qmp.run_for(seconds)
            return {'name': name, 'state': 'paused', 'ran_seconds': seconds}
        qmp.cont()
    return {'name': name, 'state': 'running'}


def pause(name):
    with QMP(run_dir(name)) as qmp:
        qmp.stop()
    return {'name': name, 'state': 'paused'}


def hmp(name, line):
    with QMP(run_dir(name)) as qmp:
        return qmp.hmp(line)


def screenshot(name, output=None):
    run = run_dir(name)
    with QMP(run) as qmp:
        path = Path(output).resolve() if output else next_shot(run, 'shot')
        return {'shot': str(qmp.png(path))}


# ---- input -----------------------------------------------------------------

def asleep(regs):
    return regs.get('pc', 0) in STANDBY_WFI


def settle_frame(qmp, before, limit, region=None, floor=0.0):
    """Poll the running guest until the frame (or region) differs from `before`
    and the next SETTLE_HOLDS polls match it, but no sooner than `floor` and no
    later than `limit` seconds. Return the last digest."""
    started = time.monotonic()
    changed, holds = None, 0
    while True:
        time.sleep(POLL)
        current = digest(qmp.frame(), region)
        if current != before:
            holds = holds + 1 if current == changed else 0
            changed = current
        elapsed = time.monotonic() - started
        if (holds >= SETTLE_HOLDS and elapsed >= floor) or elapsed >= limit:
            return current


def press(qmp, keys, pause=False, region=None):
    """Resume, send each key and wait for the screen to settle; pause afterwards
    if asked. A key that changes nothing waits its full limit."""
    keys = [KEY_NAMES.get(k, k) for k in keys]
    unknown = [k for k in keys if k not in VALID_KEYS]
    if unknown:
        raise Failure(f'unknown keys {unknown}; use {sorted(VALID_KEYS)}')
    qmp.cont()
    for key in keys:
        before = digest(qmp.frame(), region)
        qmp.hmp(f'sendkey {key} 200')
        settle_frame(qmp, before, WHEEL_WAIT if key in WHEEL_KEYS else BUTTON_WAIT, region,
                     KEY_FLOOR)
    if pause:
        qmp.stop()


def key(name, keys, pause=False):
    run = run_dir(name)
    with QMP(run) as qmp:
        before = digest(qmp.frame())
        woke = asleep(qmp.registers())
        press(qmp, keys, pause)
        changed = digest(qmp.frame()) != before
        result = {'shot': str(qmp.shot('-'.join(keys))), 'changed': changed}
        if woke:
            result['woke'] = True
    return result


def press_verified(qmp, which, region=None, attempts=3, settle=25.0, touch=('q', 'e')):
    """Press Select/Menu until the frame (or region) changes, retrying safely.

    An unchanged frame gets up to `settle` more seconds of guest time first
    (slow screens), returning as soon as the change has settled. A retry
    follows a wheel touch only if the touch left the frame unchanged;
    otherwise the first press may have landed late, or the touch moved the
    selection, and retrying could double-apply.
    """
    key_name = KEY_NAMES.get(which, which)
    before = digest(qmp.frame(), region)
    for attempt in range(1, attempts + 1):
        press(qmp, [key_name], region=region)
        if digest(qmp.frame(), region) != before:
            return attempt
        if settle_frame(qmp, before, settle, region) != before:
            return attempt
        if attempt == attempts:
            raise Failure(f'{which}: screen unchanged after {attempt} attempts; '
                          f'{qmp.shot(which + "-dropped")}')
        press(qmp, list(touch), region=region)
        if digest(qmp.frame(), region) != before:
            raise Failure(f'{which}: the wheel touch changed the screen, so the first press '
                          f'may have landed late or the list was at an edge; not retrying. '
                          f'Check {qmp.shot(which + "-touch-changed")}')
        log(f'{which}: attempt {attempt} looked dropped; retrying')
    return attempts


def button(name, which, region=None, attempts=3, settle=25.0, pause=False):
    run = run_dir(name)
    with QMP(run) as qmp:
        attempt = press_verified(qmp, which, region, attempts, settle)
        if pause:
            qmp.stop()
        return {'shot': str(qmp.shot(which)), 'attempts': attempt}


ASLEEP_SPEED = 16


def wait(name, until='stable', timeout=60.0, interval=2.0, region=None, pause=False,
         speed=None):
    """Run the guest until the frame differs from the start, two polls match, or
    the firmware is in standby (`until='asleep'`). `speed` applies for the wait
    and is restored afterwards; `until='asleep'` defaults to ASLEEP_SPEED."""
    if speed is None and until == 'asleep':
        speed = ASLEEP_SPEED
    run = run_dir(name)
    with QMP(run) as qmp, speed_for(qmp, speed):
        start = previous = digest(qmp.frame(), region)
        started = time.time()
        qmp.cont()
        try:
            while time.time() - started < timeout:
                if until == 'asleep' and asleep(qmp.registers()):
                    break
                time.sleep(interval)
                current = digest(qmp.frame(), region)
                if until == 'change' and current != start:
                    break
                if until == 'stable' and current == previous:
                    break
                previous = current
            else:
                raise Failure(f'screen did not {until} within {timeout} s')
        finally:
            if pause:
                qmp.stop()
        return {'shot': str(qmp.shot(f'wait-{until}')),
                'seconds': round(time.time() - started, 1)}


# ---- profiling --------------------------------------------------------------

def profile_send(run, *lines):
    ctl = run / 'profile.ctl'
    if not ctl.exists():
        raise Failure(f'{run.name} was launched without profiling')
    try:
        fd = os.open(ctl, os.O_WRONLY | os.O_NONBLOCK)
    except OSError as error:
        raise Failure(f'profiler control not listening ({error})')
    with os.fdopen(fd, 'w') as stream:
        stream.write(''.join(line + '\n' for line in lines))


_symbol_tables = {}


def symbol_table(profiling):
    """Sorted [(start, end, name, entry)] code ranges from the payload ELF and a Ghidra
    functions.jsonl (whose functions may span several ranges). Unreadable sources and
    malformed lines are skipped."""
    sources = [Path(p) for p in (profiling.get('elf'), profiling.get('symbols')) if p]
    key = tuple((str(p), p.stat().st_mtime) for p in sources if p.is_file())
    if key in _symbol_tables:
        return _symbol_tables[key]
    ranges = []
    elf = profiling.get('elf')
    if elf and Path(elf).is_file():
        try:
            ranges += [(addr, addr + max(size, 1), name, addr)
                       for addr, size, name in elf_functions(Path(elf).read_bytes())
                       if not re.fullmatch(r'__.+_from_(arm|thumb)', name)]
        except (OSError, struct.error, ValueError, IndexError):
            log(f'cannot read symbols from {elf}')
    symbols = profiling.get('symbols')
    if symbols and Path(symbols).is_file():
        with open(symbols) as stream:
            for line in stream:
                try:
                    entry = json.loads(line)
                    props = entry['properties']['additionalProperties']
                    name, start = props['name'], int(props['location'], 16)
                    for location in entry['locations']:
                        address = location['physicalLocation']['address']
                        begin = address['absoluteAddress']
                        ranges.append((begin, begin + address.get('length', 1), name, start))
                except (ValueError, KeyError, TypeError):
                    continue
    _symbol_tables[key] = table = sorted(ranges)
    return table


def symbolize(ret, table):
    """name+0xoff (from the function entry) for a return address, looked up by the
    call before it; None if no range contains it."""
    addr = ret - 1
    i = bisect.bisect_right(table, (addr, float('inf'), '', 0)) - 1
    while i >= 0 and table[i][0] > addr - 0x10000:     # ranges may overlap
        start, end, name, entry = table[i]
        if start <= addr < end:
            return f'{name}+{ret - entry:#x}'
        i -= 1
    return None


def name_heap_sites(run, data):
    try:
        profiling = json.loads((run / 'launch.json').read_text()).get('profile') or {}
    except (OSError, ValueError):
        return
    table = symbol_table(profiling)
    for site in data['heap']['sites']:
        site['name'] = symbolize(int(site['addr'], 16), table)


def heap_views(data, top=25):
    """Summary of a dump's heap section: totals plus the top sites by allocation,
    by live bytes held and by leaked bytes, and the top watched functions."""
    heap = data['heap']
    sites = heap['sites']

    def ranked(key, fields):
        rows = sorted((s for s in sites if s[key]), key=lambda s: s[key], reverse=True)
        return [{k: s[k] for k in ('addr', 'name', *fields) if k in s} for s in rows[:top]]
    functions = sorted((f for f in data['functions'] if f.get('alloc_incl') or
                        f.get('freed_incl')),
                       key=lambda f: f['alloc_incl'], reverse=True)[:top]
    return {
        'totals': heap['totals'],
        'heaps': sorted(heap['heaps'], key=lambda h: h['live_bytes'], reverse=True),
        'allocating': ranked('bytes', ('allocs', 'bytes', 'frees', 'freed_bytes',
                                       'leak_count', 'leak_bytes')),
        'holding': ranked('live_bytes', ('live_count', 'live_bytes')),
        'leaking': ranked('leak_bytes', ('leak_count', 'leak_bytes', 'leaks')),
        'functions': [{k: f[k] for k in ('name', 'alloc_calls', 'alloc_bytes', 'alloc_incl',
                                         'free_calls', 'freed_bytes', 'freed_incl')}
                      for f in functions],
    }


def profile_dump_with(run, qmp, output):
    output = Path(output).resolve()
    output.unlink(missing_ok=True)
    was_running = qmp.running()
    profile_send(run, f'dump {output}')
    if not was_running:
        qmp.cont()
    try:
        deadline = time.time() + 30
        while not output.exists():
            if time.time() > deadline:
                raise Failure('profiler did not write the dump within 30 s')
            time.sleep(0.05)
    finally:
        if not was_running:
            qmp.stop()
    data = json.loads(output.read_text())
    if 'heap' in data:
        name_heap_sites(run, data)
    data['summary'] = str(output)
    trace = Path(f'{output}.trace.json')
    if trace.exists():
        data['trace'] = str(trace)
    return data


def default_profile_path(run):
    return run / f'profile-{time.strftime("%H%M%S")}.json'


def profile_control(name, action):
    if action not in ('reset', 'start', 'stop'):
        raise Failure(f'unknown profiler action {action!r}')
    profile_send(run_dir(name), action)
    return {'name': name, 'queued': action}


def profile_dump(name, output=None):
    run = run_dir(name)
    with QMP(run) as qmp:
        return profile_dump_with(run, qmp, output or default_profile_path(run))


def profile_keys(name, keys, settle=0.0, output=None):
    """Reset the profiler, send keys, let the guest settle, dump."""
    run = run_dir(name)
    with QMP(run) as qmp:
        profile_send(run, 'reset', 'start')
        press(qmp, keys)
        if settle:
            time.sleep(settle)
        data = profile_dump_with(run, qmp, output or default_profile_path(run))
        data['shot'] = str(qmp.shot('profile-' + '-'.join(keys)))
    return data


def instruments(name, template='Time Profiler', seconds=None, keys=(), open_trace=True):
    run = run_dir(name)
    pid = vm_pid(run)
    if pid is None:
        raise Failure(f'{name} is not running')
    trace = run / f'instruments-{time.strftime("%H%M%S")}.trace'
    command = ['xcrun', 'xctrace', 'record', '--template', template,
               '--attach', str(pid), '--output', str(trace)]
    if seconds:
        command += ['--time-limit', f'{seconds}s']
    recorder = subprocess.Popen(command, stdout=sys.stderr)
    try:
        with QMP(run) as qmp:
            if keys:
                time.sleep(3)            # let xctrace attach
                press(qmp, keys)
            else:
                qmp.cont()
            try:
                recorder.wait()
            except KeyboardInterrupt:
                recorder.send_signal(2)
                recorder.wait()
    finally:
        if recorder.poll() is None:
            recorder.terminate()
    if not trace.exists():
        raise Failure('xctrace did not write a trace')
    if open_trace:
        subprocess.run(['open', str(trace)])
    return {'trace': str(trace)}
