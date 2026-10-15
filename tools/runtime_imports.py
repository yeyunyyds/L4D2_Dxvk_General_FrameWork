"""Inspect ordinary/delayed PE imports from the already-built RT product."""

import argparse
import json
import os
from pathlib import Path
import struct


def imports(path):
    data = Path(path).read_bytes()
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('Not a PE image')
    pe = struct.unpack_from('<I', data, 60)[0]
    if data[pe:pe+4] != b'PE\0\0':
        raise ValueError('Missing PE header')
    machine, count = struct.unpack_from('<HH', data, pe+4)
    optional_size = struct.unpack_from('<H', data, pe+20)[0]
    optional = pe+24
    magic = struct.unpack_from('<H', data, optional)[0]
    if magic not in (0x10b, 0x20b) or not 0 < count <= 96:
        raise ValueError('Unsupported PE layout')
    directories = optional + (112 if magic == 0x20b else 96)
    image_base = struct.unpack_from('<Q' if magic == 0x20b else '<I', data,
                                    optional + (24 if magic == 0x20b else 28))[0]
    sections = []
    for i in range(count):
        at = optional + optional_size + 40*i
        virtual_size, rva, raw_size, raw = struct.unpack_from('<4I', data, at+8)
        sections.append((rva, max(virtual_size, raw_size), raw_size, raw))

    def offset(rva, size=1):
        for base, length, raw_size, raw in sections:
            delta = rva - base
            if 0 <= delta and delta+size <= min(length, raw_size) and raw+delta+size <= len(data):
                return raw+delta
        raise ValueError('Import data outside file-backed sections')

    result = {'machine': hex(machine), 'ordinary': [], 'delayed': []}
    for index, kind, stride in ((1, 'ordinary', 20), (13, 'delayed', 32)):
        if directories + 8*(index+1) > optional + optional_size:
            continue
        rva, size = struct.unpack_from('<II', data, directories+8*index)
        if not rva:
            continue
        for i in range(min(size // stride + 1, 1024)):
            at = offset(rva+i*stride, stride)
            values = struct.unpack_from('<' + 'I'*(stride//4), data, at)
            if not any(values):
                break
            name_rva = values[3] if kind == 'ordinary' else values[1]
            if kind == 'delayed' and not values[0] & 1:
                name_rva -= image_base
            start = offset(name_rva)
            end = data.find(b'\0', start, min(len(data), start+512))
            if end < 0:
                raise ValueError('Unterminated import name')
            result[kind].append(data[start:end].decode('ascii').lower())
        else:
            raise ValueError('Import descriptor budget exceeded')
    return result


def analyze(runtime):
    files = {p.name.casefold(): p for p in runtime.iterdir() if p.suffix.casefold() in ('.dll', '.exe')}
    system = Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32'
    result = {'schema': 1, 'scope': 'binary_import_dependencies_not_gpu_execution',
              'b580_rt': 'unrun', 'game_rt': 'unrun', 'files': {}, 'missing_ordinary': []}
    for name, path in sorted(files.items()):
        entry = imports(path)
        entry['resolved_ordinary'] = {}
        for dependency in entry['ordinary']:
            location = ('bundle' if dependency in files else 'windows_api_set' if
                        dependency.startswith(('api-ms-win-', 'ext-ms-win-')) else
                        'system32' if (system / dependency).is_file() else 'missing')
            entry['resolved_ordinary'][dependency] = location
            if location == 'missing':
                result['missing_ordinary'].append({'importer': name, 'dependency': dependency})
        result['files'][name] = entry
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runtime', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = analyze(args.runtime)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['missing_ordinary'], indent=2))
