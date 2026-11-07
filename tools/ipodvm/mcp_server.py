#!/usr/bin/env python3
"""MCP server exposing ipodvm as tools (stdio, JSON-RPC 2.0, standard library only).

Register with Claude Code, for example:
    claude mcp add ipodvm -- python3 /path/to/qemu-ipod-classic/tools/ipodvm/mcp_server.py

Every tool is a one-shot operation on a VM under /tmp/ipodvm, the same as the
CLI. Progress goes to stderr; stdout carries only protocol messages.
"""
import base64
import json
from pathlib import Path
import sys
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parent))

import core  # noqa: E402
import screen  # noqa: E402

SERVER = {'name': 'ipodvm', 'version': '1.0.0'}
PROTOCOL_VERSIONS = ('2025-06-18', '2025-03-26', '2024-11-05')
NAME = {'type': 'string', 'description': 'VM name (default "vm")'}
KEYS = {'type': 'array', 'items': {'type': 'string'},
        'description': 'ret/select, w/menu, q/up, e/down, a/prev, d/next, s/play'}
REGION = {'type': 'array', 'items': {'type': 'integer'}, 'minItems': 4, 'maxItems': 4,
          'description': 'compare only [x, y, w, h], e.g. the title bar [0, 0, 150, 20]'}
IMAGE = {'type': 'boolean', 'description': 'also return the screenshot image'}
PAUSE = {'type': 'boolean', 'description': 'pause the guest afterwards (default: leave it '
         'running)'}


def tool(name, description, properties=None, required=()):
    return {'name': name, 'description': description,
            'inputSchema': {'type': 'object', 'properties': properties or {},
                            'required': list(required)}}


TOOLS = [
    tool('vm_launch',
         'Boot a RepriseOS CFW image with the complete feature-test configuration and '
         'return once the first screen is stable (VM left running). Takes 45-75 s. Opens '
         'a live window unless headless. The firmware enters standby after about two '
         'minutes idle only on battery or USB power; any key wakes it. vm_power changes '
         'the power while running.',
         {'firmware': {'type': 'string', 'description': 'path to osos-cfw.bin'},
          'disk': {'type': 'string', 'description': 'test disk image (cloned, never modified)'},
          'repo': {'type': 'string', 'description': 'firmware repository (for '
                   'payload/native/layout.h and the window title); default: server cwd'},
          'name': NAME, 'headless': {'type': 'boolean'},
          'title': {'type': 'string', 'description': 'window title override'},
          'layout': {'type': 'string', 'description': 'payload/native/layout.h override'},
          'pause': {'type': 'boolean', 'description': 'pause after boot'},
          'power': {'type': 'string', 'enum': ['adapter', 'usb', 'battery'],
                    'description': 'external power the PMU reports (default adapter: '
                    'idles on the Charging screen and never enters standby)'},
          'persist': {'type': 'boolean', 'description': 'write guest disk changes to the '
                      'run\'s disk clone (no -snapshot); the result reports its path'},
          'elf': {'type': 'string', 'description': 'payload.elf matching firmware, for profiling'},
          'profile': {'type': 'array', 'items': {'type': 'string'},
                      'description': 'payload function globs to profile ([] = all); needs elf'},
          'fn': {'type': 'array', 'items': {'type': 'string'},
                 'description': 'stock functions to profile, "0xADDR:NAME"'},
          'trace': {'type': 'boolean'},
          'timeline': {'type': 'string', 'enum': ['host', 'insns']},
          'signpost': {'type': 'boolean'},
          'heap': {'type': 'array', 'items': {'type': 'string'},
                   'description': 'track heap allocations through hooks "0xADDR:KIND[:NAME]" '
                   '([] = the stock 2.0.5 allocator)'},
          'symbols': {'type': 'string', 'description': 'Ghidra functions.jsonl naming stock '
                      'allocation sites'}},
         ['firmware', 'disk']),
    tool('disk_prepare', 'Clone a test disk and install theme packs from a directory that '
         'mirrors /themes (classic/default.bin, <id>/theme.bin, ...). Removes existing '
         'packs unless keep_themes. Use the new disk with vm_launch.',
         {'base': {'type': 'string'}, 'out': {'type': 'string'},
          'themes': {'type': 'string'}, 'keep_themes': {'type': 'boolean'}},
         ['base', 'out']),
    tool('vm_list', 'List VMs under /tmp/ipodvm with their window titles (owner session).'),
    tool('vm_status', 'Run state, PC/LR, whether the firmware is in standby (asleep), '
         'power and persist settings.', {'name': NAME}),
    tool('vm_power', 'Plug in or unplug external power on a running VM, as a PMU plug '
         'event: the firmware reacts at once and plugging in wakes it from standby. adapter '
         'idles on the Charging screen; usb or battery leave it and allow standby.',
         {'name': NAME, 'power': {'type': 'string', 'enum': ['adapter', 'usb', 'battery']}},
         ['power']),
    tool('vm_quit', 'Stop a VM.', {'name': NAME}),
    tool('vm_run', 'Resume the guest, for `seconds` then pause if given (slow screens and '
         'disk loads need real guest time; a paused guest gets none).',
         {'name': NAME, 'seconds': {'type': 'number'}}),
    tool('vm_pause', 'Pause the guest.', {'name': NAME}),
    tool('screen', 'Read the screen as JSON: title, rows (label, value, detail, submenu), '
         'selected row, preview pane text, state (list/other/charging/blank); asleep '
         'when the firmware is in standby (the last frame stays on screen).',
         {'name': NAME, 'image': IMAGE}),
    tool('goto', 'Navigate by row labels, e.g. "Settings/Theme": returns to the main menu '
         '(passing Language and the Charging screen), scrolls to each row, selects it. '
         'Returns the steps and the final screen. Selecting a toggle row toggles it.',
         {'name': NAME, 'path': {'type': 'string'},
          'from_root': {'type': 'boolean', 'description': 'default true; false starts here'},
          'image': IMAGE},
         ['path']),
    tool('press', 'Resume and send keys with settle waits. Reports whether the frame '
         'changed and woke=true if the guest was in standby (the waking key is also '
         'delivered). Prefer select/menu for buttons and goto for navigation.',
         {'name': NAME, 'keys': KEYS, 'image': IMAGE, 'pause': PAUSE}, ['keys']),
    tool('select', 'Press Select and verify the screen changed, retrying safely if dropped.',
         {'name': NAME, 'region': REGION, 'image': IMAGE, 'pause': PAUSE}),
    tool('menu', 'Press Menu and verify the screen changed, retrying safely if dropped.',
         {'name': NAME, 'region': REGION, 'image': IMAGE, 'pause': PAUSE}),
    tool('screenshot', 'Return the current screen as an image.', {'name': NAME}),
    tool('wait', 'Run the guest until the screen changes or two polls match.',
         {'name': NAME, 'until': {'type': 'string', 'enum': ['change', 'stable']},
          'timeout': {'type': 'number'}, 'image': IMAGE, 'pause': PAUSE}),
    tool('profile_keys', 'Reset the profiler, send keys, dump: per-function calls and guest '
         'instruction counts and, with heap hooks, the heap allocated, held and leaked by '
         'that input (VM must be launched with profiling).',
         {'name': NAME, 'keys': KEYS, 'settle': {'type': 'number'},
          'top': {'type': 'integer', 'description': 'functions and heap sites to return (default 25)'}},
         ['keys']),
    tool('profile_dump', 'Dump the profiler totals since the last reset (heap: current live '
         'totals plus window statistics).',
         {'name': NAME, 'top': {'type': 'integer'}}),
    tool('profile_control', 'Queue a profiler reset, start or stop.',
         {'name': NAME, 'action': {'type': 'string', 'enum': ['reset', 'start', 'stop']}},
         ['action']),
    tool('hmp', 'Run a raw QEMU HMP command, e.g. "info registers".',
         {'name': NAME, 'command': {'type': 'string'}}, ['command']),
]


def trim_profile(data, top):
    data = dict(data)
    if 'heap' in data:
        data['heap'] = core.heap_views(data, top or 25)
    data['functions'] = data['functions'][:top or 25]
    return data


def call(name, a):
    vm = a.get('name', 'vm')
    if name == 'vm_launch':
        repo = Path(a.get('repo') or Path.cwd()).expanduser()

        def path(key):      # relative paths are relative to the firmware repository
            return str(repo / a[key]) if a.get(key) else None
        return core.launch(vm, path('firmware'), path('disk'), layout=path('layout'),
                           title=a.get('title'), headless=a.get('headless', False),
                           pause=a.get('pause', False), elf=path('elf'),
                           profile=a.get('profile'), fns=a.get('fn'),
                           trace=a.get('trace', False), timeline=a.get('timeline'),
                           signpost=a.get('signpost', False), heap=a.get('heap'),
                           symbols=path('symbols'), cwd=str(repo),
                           power=a.get('power', 'adapter'), persist=a.get('persist', False))
    if name == 'disk_prepare':
        return core.prepare_disk(a['base'], a['out'], a.get('themes'),
                                 not a.get('keep_themes', False))
    if name == 'vm_list':
        return core.list_vms()
    if name == 'vm_status':
        return core.status(vm)
    if name == 'vm_power':
        return core.set_power(vm, a['power'])
    if name == 'vm_quit':
        return core.quit_vm(vm)
    if name == 'vm_run':
        return core.resume(vm, a.get('seconds'))
    if name == 'vm_pause':
        return core.pause(vm)
    if name == 'screen':
        return screen.screen(vm)
    if name == 'goto':
        return screen.goto(vm, a['path'], a.get('from_root', True))
    if name == 'press':
        return core.key(vm, a['keys'], a.get('pause', False))
    if name in ('select', 'menu'):
        region = tuple(a['region']) if a.get('region') else None
        return core.button(vm, name, region, pause=a.get('pause', False))
    if name == 'screenshot':
        return core.screenshot(vm)
    if name == 'wait':
        return core.wait(vm, a.get('until', 'stable'), a.get('timeout', 60),
                         pause=a.get('pause', False))
    if name == 'profile_keys':
        return trim_profile(core.profile_keys(vm, a['keys'], a.get('settle', 0)), a.get('top'))
    if name == 'profile_dump':
        return trim_profile(core.profile_dump(vm), a.get('top'))
    if name == 'profile_control':
        return core.profile_control(vm, a['action'])
    if name == 'hmp':
        return core.hmp(vm, a['command'])
    raise core.Failure(f'unknown tool {name}')


def tool_result(name, arguments):
    try:
        data = call(name, arguments)
    except core.Failure as error:
        return {'content': [{'type': 'text', 'text': str(error)}], 'isError': True}
    except Exception:
        return {'content': [{'type': 'text', 'text': traceback.format_exc()}], 'isError': True}
    content = [{'type': 'text', 'text': data if isinstance(data, str) else json.dumps(data, indent=1)}]
    shot = data.get('shot') if isinstance(data, dict) else None
    if shot and (name == 'screenshot' or arguments.get('image')):
        content.append({'type': 'image', 'mimeType': 'image/png',
                        'data': base64.b64encode(Path(shot).read_bytes()).decode()})
    return {'content': content}


def handle(message):
    method, ident = message.get('method'), message.get('id')
    params = message.get('params') or {}
    if method == 'initialize':
        requested = params.get('protocolVersion')
        version = requested if requested in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[0]
        result = {'protocolVersion': version,
                  'capabilities': {'tools': {}}, 'serverInfo': SERVER}
    elif method == 'tools/list':
        result = {'tools': TOOLS}
    elif method == 'tools/call':
        result = tool_result(params['name'], params.get('arguments') or {})
    elif method == 'ping':
        result = {}
    elif ident is None:
        return None                      # notification
    else:
        return {'jsonrpc': '2.0', 'id': ident,
                'error': {'code': -32601, 'message': f'unknown method {method}'}}
    return {'jsonrpc': '2.0', 'id': ident, 'result': result}


def main():
    for line in sys.stdin:
        if not line.strip():
            continue
        try:
            message = json.loads(line)
        except ValueError as error:
            reply = {'jsonrpc': '2.0', 'id': None,
                     'error': {'code': -32700, 'message': str(error)}}
        else:
            try:
                reply = handle(message)
            except (KeyError, TypeError) as error:
                reply = {'jsonrpc': '2.0', 'id': message.get('id'),
                         'error': {'code': -32602, 'message': f'invalid params: {error}'}}
            except Exception as error:
                reply = {'jsonrpc': '2.0', 'id': message.get('id'),
                         'error': {'code': -32603, 'message': str(error)}}
        if reply is not None:
            sys.stdout.write(json.dumps(reply) + '\n')
            sys.stdout.flush()


if __name__ == '__main__':
    main()
