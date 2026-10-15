"""Read-only loaded-module evidence for future L4D2 RT binding.

This collector never injects code, calls a game interface, installs a hook or
changes a CVar. A unique byte pattern or RTTI vtable is only a candidate.
"""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
MODULES = {'CLIENT_MOD': 'client.dll', 'ENGINE_MOD': 'engine.dll',
           'RENDERER_MOD': 'shaderapidx9.dll'}
CLASSES = {'CMaterialSystem': [95, 96], 'CMatRenderContext': [29, 30, 146],
           'CMatQueuedRenderContext': [29, 30, 146]}
EXECUTE = 0x20000000
READ = 0x40000000
MAX_IMAGE = 256 * 1024 * 1024
MAX_SECTION = 64 * 1024 * 1024


def u32(data, offset=0):
    return struct.unpack_from('<I', data, offset)[0]


def pattern_bytes(pattern):
    tokens = pattern.split()
    if not tokens or len(tokens) > 512:
        raise ValueError('Invalid pattern length')
    values = []
    for token in tokens:
        if token in ('?', '??'):
            values.append(None)
        elif len(token) == 2 and all(c in '0123456789abcdefABCDEF' for c in token):
            values.append(int(token, 16))
        else:
            raise ValueError('Invalid pattern token')
    if all(x is None for x in values):
        raise ValueError('Pattern needs a fixed byte')
    return values


def matches(data, pattern, limit=4096):
    """Return bounded matches and whether the match count is truncated."""
    values = pattern_bytes(pattern)
    anchor = next(i for i, x in enumerate(values) if x is not None)
    fixed = bytes([values[anchor]])
    result, position = [], anchor
    while True:
        found = data.find(fixed, position)
        if found < 0:
            return result, False
        position = found + 1
        start = found - anchor
        if start + len(values) <= len(data) and all(
                x is None or data[start + i] == x for i, x in enumerate(values)):
            if len(result) == limit:
                return result, True
            result.append(start)


class MappedImage:
    """A bounded snapshot using memory RVAs, never file offsets or reference RVAs."""

    def __init__(self, read, base, snapshot_size):
        self.base = base
        if not 0 < snapshot_size <= MAX_IMAGE or not 0 < base < 2**32:
            raise ValueError('Expected bounded x86 module')
        dos = read(base, 64)
        if dos[:2] != b'MZ':
            raise ValueError('Missing DOS header')
        pe = u32(dos, 60)
        if pe < 64 or pe > min(snapshot_size - 24, 1024 * 1024):
            raise ValueError('Invalid PE header offset')
        coff = read(base + pe, 24)
        if coff[:4] != b'PE\0\0' or struct.unpack_from('<H', coff, 4)[0] != 0x14c:
            raise ValueError('Expected PE32 i386 module')
        count = struct.unpack_from('<H', coff, 6)[0]
        optional_size = struct.unpack_from('<H', coff, 20)[0]
        if not 1 <= count <= 96 or not 96 <= optional_size <= 4096:
            raise ValueError('Invalid PE section table')
        table_size = optional_size + count * 40
        if pe + 24 + table_size > snapshot_size:
            raise ValueError('Section table outside module')
        header = read(base + pe + 24, table_size)
        if struct.unpack_from('<H', header)[0] != 0x10b:
            raise ValueError('Expected PE32 optional header')
        self.size = u32(header, 56)
        if self.size != snapshot_size or base + self.size > 2**32:
            raise ValueError('Inconsistent module size')
        self.timestamp = u32(coff, 8)
        self.sections, self.read_errors = [], []
        budget = 0
        for i in range(count):
            entry = header[optional_size + i * 40:optional_size + (i + 1) * 40]
            name = entry[:8].split(b'\0')[0].decode('ascii', errors='replace')
            size, rva, raw_size = struct.unpack_from('<III', entry, 8)
            size = size or raw_size
            flags = u32(entry, 36)
            if not size or rva + size > self.size or rva < pe + 24 + table_size:
                raise ValueError('Invalid mapped section bounds')
            if any(rva < s['rva'] + s['size'] and s['rva'] < rva + size for s in self.sections):
                raise ValueError('Overlapping mapped sections')
            section = {'name': name, 'rva': rva, 'size': size, 'flags': flags, 'data': None}
            self.sections.append(section)
            # Executable code and the ordinary MSVC RTTI data sections suffice.
            if flags & READ and (flags & EXECUTE or name in ('.rdata', '.data')):
                budget += size
                if size > MAX_SECTION or budget > MAX_IMAGE:
                    raise ValueError('Module snapshot budget exceeded')
                try:
                    section['data'] = read(base + rva, size)
                    if len(section['data']) != size:
                        raise OSError('Partial mapped section read')
                except OSError as error:
                    section['data'] = None
                    self.read_errors.append({'section': name, 'error': str(error)})

    def data_at(self, rva, size):
        for section in self.sections:
            offset = rva - section['rva']
            if section['data'] is not None and 0 <= offset <= section['size'] - size:
                return section['data'][offset:offset + size]
        return None

    def executable(self, rva):
        return any(s['flags'] & EXECUTE and s['data'] is not None and
                   s['rva'] <= rva < s['rva'] + s['size'] for s in self.sections)

    def pattern(self, pattern):
        found, truncated = [], False
        unavailable = False
        for section in self.sections:
            if not section['flags'] & EXECUTE:
                continue
            if section['data'] is None:
                unavailable = True
                continue
            offsets, extra = matches(section['data'], pattern)
            found.extend(section['rva'] + n for n in offsets)
            truncated |= extra
        return {'result': 'incomplete' if unavailable else
                'ambiguous' if len(found) > 1 or truncated else
                'unique_candidate' if found else 'absent',
                'match_count': len(found), 'count_truncated': truncated,
                'match_rvas': found[:32], 'hook_install_allowed': False}

    def occurrences(self, value):
        for section in self.sections:
            if section['data'] is None or section['flags'] & EXECUTE:
                continue
            position = 0
            while True:
                position = section['data'].find(value, position)
                if position < 0:
                    break
                yield section['rva'] + position
                position += 1

    def rtti(self, name, slots):
        result, truncated = [], False
        # MSVC x86 TypeDescriptor: two pointers followed by decorated name.
        for name_rva in self.occurrences(b'.?AV' + name.encode('ascii') + b'@@\0'):
            td = name_rva - 8
            if td % 4 or self.data_at(td, 8) is None:
                continue
            for type_pointer in self.occurrences(struct.pack('<I', self.base + td)):
                col = type_pointer - 12
                raw = self.data_at(col, 20)
                if col % 4 or raw is None:
                    continue
                signature, offset, cd_offset, _, hierarchy = struct.unpack('<5I', raw)
                chd = self.data_at(hierarchy - self.base, 16)
                if signature or offset > 1024 * 1024 or cd_offset > 1024 * 1024 or chd is None:
                    continue
                chd_signature, _, bases, base_array = struct.unpack('<4I', chd)
                if chd_signature or not 1 <= bases <= 4096 or self.data_at(base_array - self.base, bases * 4) is None:
                    continue
                for locator in self.occurrences(struct.pack('<I', self.base + col)):
                    table = locator + 4
                    first = self.data_at(table, 4)
                    if locator % 4 or first is None or not self.executable(u32(first) - self.base):
                        continue
                    if len(result) == 32:
                        return {'result': 'candidates', 'candidates': result, 'truncated': True,
                                'selected_game_slot': None, 'interface_binding_verified': False}
                    methods = {}
                    for slot in slots:
                        pointer = self.data_at(table + slot * 4, 4)
                        target = u32(pointer) - self.base if pointer else -1
                        valid = self.executable(target)
                        code = self.data_at(target, 32) if valid else None
                        methods[str(slot)] = {'target_rva': target if valid else None,
                                              'in_module_executable': valid,
                                              'first_32_bytes': code.hex() if code else None,
                                              'semantics_verified': False}
                    result.append({'vtable_rva': table, 'locator_rva': col,
                                   'type_descriptor_rva': td, 'object_offset': offset,
                                   'candidate_slots': methods})
        return {'result': 'candidates' if result else 'unavailable',
                'candidates': result, 'truncated': truncated,
                'selected_game_slot': None, 'interface_binding_verified': False}

    def interface_registry(self):
        """Collect SDK InterfaceReg layout candidates without calling factories.

        Source's x86 entry is {factory, name, next}. Even matching this layout
        and a simple getter body does not validate a live SDK/game ABI.
        """
        result = []
        for section in self.sections:
            if section['data'] is None or section['flags'] & EXECUTE:
                continue
            for match in re.finditer(rb'VMaterialSystem[0-9]{3}\0', section['data']):
                name_rva = section['rva'] + match.start()
                for pointer in self.occurrences(struct.pack('<I', self.base + name_rva)):
                    entry = pointer - 4
                    raw = self.data_at(entry, 12)
                    if entry % 4 or raw is None:
                        continue
                    factory, _, next_entry = struct.unpack('<3I', raw)
                    if not self.executable(factory - self.base):
                        continue
                    if next_entry and self.data_at(next_entry - self.base, 12) is None:
                        continue
                    code = self.data_at(factory - self.base, 32)
                    object_address = None
                    getter_shape = 'unresolved'
                    # Only two complete, side-effect-free x86 return shapes.
                    # Read their referenced value; never execute even these.
                    if code and code[5:6] == b'\xc3':
                        if code[0] == 0xb8:  # mov eax, imm32; ret
                            getter_shape = 'mov_eax_immediate_ret'
                            object_address = u32(code, 1)
                        elif code[0] == 0xa1:  # mov eax, [imm32]; ret
                            getter_shape = 'mov_eax_absolute_pointer_ret'
                            value = self.data_at(u32(code, 1) - self.base, 4)
                            object_address = u32(value) if value else None
                    table = self.data_at(object_address - self.base, 4) if object_address else None
                    table_rva = u32(table) - self.base if table else -1
                    first = self.data_at(table_rva, 4)
                    valid = first is not None and self.executable(u32(first) - self.base)
                    methods = {}
                    if valid:
                        for slot in (95, 96):
                            value = self.data_at(table_rva + slot * 4, 4)
                            target = u32(value) - self.base if value else -1
                            function = self.data_at(target, 32) if self.executable(target) else None
                            methods[str(slot)] = {'target_rva': target if function else None,
                                                  'first_32_bytes': function.hex() if function else None,
                                                  'semantics_verified': False}
                    result.append({'name': match.group()[:-1].decode('ascii'),
                                   'entry_rva': entry, 'factory_rva': factory - self.base,
                                   'factory_first_32_bytes': code.hex() if code else None,
                                   'getter_shape': getter_shape,
                                   'object_rva': object_address - self.base if valid else None,
                                   'vtable_rva': table_rva if valid else None,
                                   'candidate_slots': methods,
                                   'interface_binding_verified': False})
                    if len(result) == 32:
                        return {'result': 'candidates', 'candidates': result, 'truncated': True}
        return {'result': 'candidates' if result else 'unavailable',
                'candidates': result, 'truncated': False}

    def evidence(self, hooks, classes):
        return {'machine': 'i386', 'mapped_size': self.size, 'timestamp': self.timestamp,
                'base_address': self.base, 'read_errors': self.read_errors,
                'sections': [{k: v for k, v in s.items() if k != 'data'} |
                             {'mapped_sha256': hashlib.sha256(s['data']).hexdigest()
                              if s['data'] is not None else None} for s in self.sections],
                'hook_candidates': [{'symbol': h['symbol'], **self.pattern(h['pattern'])} for h in hooks],
                'rtti': {name: self.rtti(name, slots) for name, slots in classes.items()},
                'material_system_registry': self.interface_registry() if classes else {'result': 'not_requested'}}


class WindowsProcess:
    """Only QUERY_INFORMATION/VM_READ; no remote thread or write access."""

    def __init__(self, pid):
        if os.name != 'nt':
            raise OSError('Loaded-module collection requires Windows')
        from ctypes import wintypes as w
        self.w = w
        self.api = ctypes.WinDLL('kernel32', use_last_error=True)
        signatures = {
            'OpenProcess': (w.HANDLE, [w.DWORD, w.BOOL, w.DWORD]),
            'CloseHandle': (w.BOOL, [w.HANDLE]),
            'ReadProcessMemory': (w.BOOL, [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                         ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]),
            'CreateToolhelp32Snapshot': (w.HANDLE, [w.DWORD, w.DWORD]),
            'GetProcessTimes': (w.BOOL, [w.HANDLE] + [ctypes.POINTER(w.FILETIME)] * 4),
            'GetExitCodeProcess': (w.BOOL, [w.HANDLE, ctypes.POINTER(w.DWORD)]),
        }
        for name, (restype, argtypes) in signatures.items():
            function = getattr(self.api, name)
            function.restype, function.argtypes = restype, argtypes
        self.pid = pid
        self.handle = self.api.OpenProcess(0x0400 | 0x0010, False, pid)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.api.CloseHandle(self.handle)

    def identity(self):
        times = [self.w.FILETIME() for _ in range(4)]
        exit_code = self.w.DWORD()
        if not self.api.GetProcessTimes(self.handle, *[ctypes.byref(x) for x in times]):
            raise ctypes.WinError(ctypes.get_last_error())
        if not self.api.GetExitCodeProcess(self.handle, ctypes.byref(exit_code)):
            raise ctypes.WinError(ctypes.get_last_error())
        return {'pid': self.pid, 'creation_filetime': times[0].dwLowDateTime |
                times[0].dwHighDateTime << 32, 'alive': exit_code.value == 259}

    def read(self, address, size):
        if not 0 < size <= MAX_SECTION:
            raise ValueError('Invalid memory read size')
        data = ctypes.create_string_buffer(size)
        copied = ctypes.c_size_t()
        if not self.api.ReadProcessMemory(self.handle, address, data, size, ctypes.byref(copied)) or copied.value != size:
            raise OSError(f'ReadProcessMemory at {address:#x}, size {size}: error {ctypes.get_last_error()}')
        return data.raw

    def modules(self):
        w = self.w

        class Entry(ctypes.Structure):
            _fields_ = [('dwSize', w.DWORD), ('th32ModuleID', w.DWORD),
                        ('th32ProcessID', w.DWORD), ('GlblcntUsage', w.DWORD),
                        ('ProccntUsage', w.DWORD), ('modBaseAddr', ctypes.c_void_p),
                        ('modBaseSize', w.DWORD), ('hModule', w.HMODULE),
                        ('szModule', w.WCHAR * 256), ('szExePath', w.WCHAR * 260)]

        for name in ('Module32FirstW', 'Module32NextW'):
            function = getattr(self.api, name)
            function.restype, function.argtypes = w.BOOL, [w.HANDLE, ctypes.POINTER(Entry)]
        snapshot = None
        # ERROR_BAD_LENGTH is documented for racing loader snapshots.
        for _ in range(4):
            snapshot = self.api.CreateToolhelp32Snapshot(0x08 | 0x10, self.pid)
            if snapshot != ctypes.c_void_p(-1).value:
                break
            if ctypes.get_last_error() != 24:
                raise ctypes.WinError(ctypes.get_last_error())
        if snapshot == ctypes.c_void_p(-1).value:
            raise OSError('Module snapshot changed during all four attempts')
        try:
            entry = Entry(); entry.dwSize = ctypes.sizeof(entry)
            ok = self.api.Module32FirstW(snapshot, ctypes.byref(entry))
            result = []
            while ok:
                result.append({'name': entry.szModule.lower(), 'path': entry.szExePath,
                               'base': entry.modBaseAddr, 'size': entry.modBaseSize})
                ok = self.api.Module32NextW(snapshot, ctypes.byref(entry))
            if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES
                raise ctypes.WinError(ctypes.get_last_error())
            return result
        finally:
            self.api.CloseHandle(snapshot)


def file_identity(path):
    path = Path(path)
    before = path.stat()
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise OSError('Module file changed while hashing')
    return {'size': after.st_size, 'sha256': digest.hexdigest()}


def collect(pid, audit):
    report = {'schema': 1, 'scope': 'read_only_game_binding_candidates', 'execution': 'incomplete',
              'reference_mod_commit': json.loads((ROOT / 'integration/rt/baseline.json').read_text())['compat_commit'],
              'reference_audit_sha256': hashlib.sha256(json.dumps(audit, sort_keys=True).encode()).hexdigest(),
              'collector_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'sdk_candidate_manifest_sha256': hashlib.sha256(
                  (ROOT / 'research/rt-source-queue-sdk.json').read_bytes()).hexdigest(),
              'hook_install_allowed': False, 'game_queue_execution': 'unrun',
              'b580_rt': 'unrun', 'snapshot_atomic': False, 'modules': []}
    try:
        with WindowsProcess(pid) as process:
            report['process'] = process.identity()
            if not report['process']['alive']:
                raise OSError('Target process has exited')
            before = process.modules()
            required = set(MODULES.values()) | {'materialsystem.dll'}
            stable = True
            for name in sorted(required):
                candidates = [m for m in before if m['name'] == name]
                item = {'name': name, 'result': 'unavailable'}
                report['modules'].append(item)
                if len(candidates) != 1:
                    item['error'] = 'Module missing or ambiguous'; stable = False; continue
                module = candidates[0]
                item.update(path=module['path'], base_address=module['base'])
                try:
                    item['file'] = file_identity(module['path'])
                    hooks = [h for h in audit['hook_references'] if MODULES[h['module']] == name]
                    classes = CLASSES if name == 'materialsystem.dll' else {}
                    image = MappedImage(process.read, module['base'], module['size'])
                    item.update(image.evidence(hooks, classes))
                    item['result'] = 'incomplete' if image.read_errors else 'snapshot_collected'
                    stable &= not image.read_errors
                except (OSError, ValueError, struct.error) as error:
                    item['error'] = str(error); stable = False
            after = process.modules()
            for name in required:
                identity = lambda entries: [(x['base'], x['size'], x['path']) for x in entries if x['name'] == name]
                if identity(before) != identity(after):
                    stable = False
            report['module_list_stable'] = all(
                [(x['base'], x['size'], x['path']) for x in before if x['name'] == name] ==
                [(x['base'], x['size'], x['path']) for x in after if x['name'] == name]
                for name in required)
            report['process_after'] = process.identity()
            if not report['process_after']['alive']:
                stable = False
            report['execution'] = 'snapshot_collected' if stable else 'incomplete'
    except (OSError, ValueError) as error:
        report['error'] = str(error)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New output directory inside the suite session')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    audit = json.loads((ROOT / 'research/rt-source-audit.json').read_text())
    result = collect(args.pid, audit)
    (args.output / 'game-binding.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'execution': result['execution'], 'hook_install_allowed': False}))
    raise SystemExit(0 if result['execution'] == 'snapshot_collected' else 1)
