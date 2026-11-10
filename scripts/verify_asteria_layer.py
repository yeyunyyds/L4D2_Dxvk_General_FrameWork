"""Verify and describe the compiled GF/Asteria x64 Vulkan layer."""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(dll: Path, source: Path, output: Path) -> None:
    data = dll.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError("Layer is not a PE image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", data, pe + 4)[0] != 0x8664:
        raise ValueError("Layer is not an x86_64 PE image")
    required = (b"vkNegotiateLoaderLayerInterfaceVersion", b"vkGetInstanceProcAddr",
                b"vkGetDeviceProcAddr", b"AsteriaSubmitFrameInputsV2",
                "ASTERIA_FG_ESTIMATED_INTERNAL".encode("utf-16le"))
    if any(symbol not in data for symbol in required):
        raise ValueError("Layer is missing a required loader/runtime symbol")
    source_receipt = json.loads((source / "gf-x64-layer-source.json").read_text())
    manifest = {
        "file_format_version": "1.2.0",
        "layer": {
            "name": "VK_LAYER_GF_Asteria_FrameGeneration",
            "type": "GLOBAL",
            "library_path": ".\\AsteriaFrameGenerationLayer64.dll",
            "api_version": "1.3.280",
            "implementation_version": "1",
            "description": "GF x64 FSR3 frame-generation layer based on Asteria",
            "functions": {
                "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion",
                "vkGetInstanceProcAddr": "vkGetInstanceProcAddr",
                "vkGetDeviceProcAddr": "vkGetDeviceProcAddr",
            },
        },
    }
    (output / "VK_LAYER_GF_Asteria_FrameGeneration.json").write_text(json.dumps(manifest, indent=2) + "\n")
    receipt = {
        **source_receipt,
        "dll": dll.name,
        "dll_sha256": digest(dll),
        "pe_machine": "AMD64",
        "queue_policy": "surface-selected distinct game/compute/present/acquire queues",
        "input_policy": "native provider V2 ticket/lease with separate HUD-less color and UI overlay; estimated inputs are diagnostic fallback only",
    }
    (output / "gf-asteria-layer-build.json").write_text(json.dumps(receipt, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        verify(args.dll.resolve(), args.source.resolve(), args.output.resolve())
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, f"{error}\n")
