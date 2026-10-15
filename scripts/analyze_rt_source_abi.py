"""Extract SDK candidate vtable slots from MSVC x86 wrapper assembly."""

import argparse
import hashlib
import json
from pathlib import Path
import re


def analyze(text):
    slots = {}
    for name in ('GetCallQueue', 'GetRenderContext', 'GetMatrix4', 'GetMatrix3'):
        body = re.search(r'_candidate' + name + r'\s+PROC\b(.*?)_candidate' + name + r'\s+ENDP\b', text, re.S)
        if not body:
            raise ValueError('Missing MSVC wrapper: ' + name)
        calls = re.findall(r'\b(?:call|jmp)\s+DWORD PTR \[[a-z]+\+(\d+)\]', body[1], re.I)
        if len(calls) != 1 or int(calls[0]) % 4:
            raise ValueError('Ambiguous indirect call: ' + name)
        slots[name] = {'vtable_byte_offset': int(calls[0]), 'vtable_slot': int(calls[0]) // 4}
    if slots['GetMatrix4'] == slots['GetMatrix3']:
        raise ValueError('Overloaded matrix methods resolved to same slot')
    return {'schema': 1, 'scope': 'MSVC x86 compiled pinned SDK candidates',
            'game_abi_verified': False, 'game_hook_installed': False, 'methods': slots}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('assembly', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = analyze(args.assembly.read_text(errors='strict'))
    result['assembly_sha256'] = hashlib.sha256(args.assembly.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
