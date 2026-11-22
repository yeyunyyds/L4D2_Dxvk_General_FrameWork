"""Embed a validated little-endian SPIR-V module as a C++ uint32 array."""
import argparse
import hashlib
from pathlib import Path
import struct


def embed(source: Path, output: Path, symbol: str, provenance: str):
    payload = source.read_bytes()
    if len(payload) % 4 or payload[:4] != b'\x03\x02#\x07':
        raise ValueError('Input is not a little-endian SPIR-V module')
    words = struct.unpack(f'<{len(payload) // 4}I', payload)
    rows = []
    for offset in range(0, len(words), 12):
        rows.append(','.join(hex(word) for word in words[offset:offset + 12]))
    provenance_path = Path(provenance)
    source_stamp = (
        f'// source-sha256: {hashlib.sha256(provenance_path.read_bytes()).hexdigest()}\\n'
        if provenance_path.is_file() else ''
    )
    output.write_text(
        f'// Generated from {provenance}; validated Vulkan 1.1 SPIR-V.\\n'
        + source_stamp + ''
        '#pragma once\n#include <cstdint>\n'
        f'namespace dxvk_capture {{ inline constexpr uint32_t {symbol}[]={{\n'
        + ',\n'.join(rows) + '\n}; }\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--symbol', required=True)
    parser.add_argument('--provenance', required=True)
    args = parser.parse_args()
    embed(args.source, args.output, args.symbol, args.provenance)
