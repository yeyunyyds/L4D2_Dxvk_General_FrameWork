"""PE/RTTI failure cases and optional live x64-reader to x86-target check."""

import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from rt_game_probe import MappedImage, WindowsProcess, collect, file_identity, matches, pattern_bytes, unique_modules

BASE = 0x12340000


def put32(data, at, *values):
    struct.pack_into('<' + 'I' * len(values), data, at, *values)


def fixture():
    data = bytearray(0x5000)
    data[:2] = b'MZ'; put32(data, 60, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', data, 0x84, 0x14c, 3, 1234, 0, 0, 224, 0x102)
    struct.pack_into('<H', data, 0x98, 0x10b)
    put32(data, 0x98 + 56, len(data))
    for i, (name, rva, flags) in enumerate([
            (b'.text', 0x1000, 0x60000020), (b'.rdata', 0x2000, 0x40000040),
            (b'.data', 0x3000, 0xc0000040)]):
        offset = 0x178 + i * 40
        data[offset:offset+len(name)] = name
        put32(data, offset + 8, 0x1000, rva, 0x1000, rva)
        put32(data, offset + 36, flags)
    data[0x1050:0x1056] = bytes.fromhex('8b 43 04 3b c7 c3')
    # Same bytes in data cannot create an executable hook candidate.
    data[0x2050:0x2056] = data[0x1050:0x1056]
    name = b'.?AVCMatRenderContext@@\0'
    data[0x3108:0x3108 + len(name)] = name
    put32(data, 0x2200, 0, 0, 0, BASE + 0x3100, BASE + 0x2240)  # COL
    put32(data, 0x2240, 0, 0, 1, BASE + 0x2280)  # hierarchy
    put32(data, 0x2280, BASE + 0x2290)
    put32(data, 0x2290, BASE + 0x3100)
    put32(data, 0x2400, BASE + 0x2200)
    for slot in range(147):
        put32(data, 0x2404 + slot * 4, BASE + 0x1050)
    return data


def image(data, missing=()):
    def read(address, size):
        at = address - BASE
        if address in missing:
            raise OSError('Unreadable mapped page')
        if at < 0 or at + size > len(data):
            raise OSError('Out of range')
        return bytes(data[at:at+size])
    return MappedImage(read, BASE, len(data))


class ProbeCases(unittest.TestCase):
    def test_identical_snapshot_aliases_merge_but_conflicting_modules_remain(self):
        first = {'name': 'client.dll', 'path': 'C:\\Game\\client.dll', 'base': BASE, 'size': 4096}
        alias = {**first, 'path': 'c:\\game\\CLIENT.DLL'}
        conflict = {**first, 'base': BASE + 0x10000}
        result = unique_modules([first, alias, conflict])
        self.assertEqual(len(result), 2)
        self.assertEqual(result[0]['snapshot_alias_count'], 2)
        self.assertEqual(result[1]['snapshot_alias_count'], 1)

    def test_patterns_are_bounded_and_validate_tokens(self):
        self.assertEqual(matches(b'aaaa', '61 61'), ([0, 1, 2], False))
        self.assertEqual(matches(b'aaaa', '? 61', 1), ([0], True))
        for value in ['', '? ??', 'zz', '1', 'aa*']:
            with self.subTest(value=value), self.assertRaises(ValueError):
                pattern_bytes(value)

    def test_aslr_and_executable_sections_only(self):
        module = image(fixture())
        found = module.pattern('8B 43 ? 3B C7')
        self.assertEqual(found['result'], 'unique_candidate')
        self.assertEqual(found['match_rvas'], [0x1050])
        window = found['code_windows'][0]
        self.assertEqual(window['window_rva'] + window['target_offset'], 0x1050)
        self.assertIn('8b43043bc7', window['bytes'])
        self.assertFalse(found['hook_install_allowed'])
        self.assertEqual(module.pattern('90 90 90')['result'], 'absent')
        self.assertEqual(module.code_window(0x1000)['target_offset'], 0)
        self.assertEqual(len(module.code_window(0x1fff)['bytes']) // 2, 33)
        self.assertIsNone(module.code_window(0x2050))

    def test_ambiguous_pattern_cannot_select_reference_rva(self):
        data = fixture(); data[0x1080:0x1085] = data[0x1050:0x1055]
        found = image(data).pattern('8B 43 ? 3B C7')
        self.assertEqual(found['result'], 'ambiguous')
        self.assertEqual(found['match_rvas'], [0x1050, 0x1080])

    def test_unreadable_code_is_incomplete_not_absent(self):
        module = image(fixture(), [BASE + 0x1000])
        self.assertEqual(module.pattern('8B 43 ? 3B C7')['result'], 'incomplete')
        self.assertEqual(len(module.read_errors), 1)

    def test_rtti_candidate_offsets_and_slots_are_not_semantics(self):
        result = image(fixture()).rtti('CMatRenderContext', [29, 30, 146])
        self.assertEqual(result['result'], 'candidates')
        self.assertIsNone(result['selected_game_slot'])
        self.assertFalse(result['interface_binding_verified'])
        self.assertEqual(result['candidates'][0]['vtable_rva'], 0x2404)
        for slot in result['candidates'][0]['candidate_slots'].values():
            self.assertEqual(slot['target_rva'], 0x1050)
            self.assertFalse(slot['semantics_verified'])

    def test_rtti_rejects_bad_locator_hierarchy_or_first_entry(self):
        for offset, value in [(0x2200, 1), (0x2210, BASE + 0x5000),
                              (0x2248, 0), (0x224c, BASE + 0x4fff),
                              (0x2404, BASE + 0x2050)]:
            data = fixture(); put32(data, offset, value)
            with self.subTest(offset=offset):
                self.assertEqual(image(data).rtti('CMatRenderContext', [146])['result'], 'unavailable')

    def test_missing_rtti_is_unavailable_and_outside_slots_are_reported(self):
        module = image(fixture())
        self.assertEqual(module.rtti('CMatQueuedRenderContext', [146])['result'], 'unavailable')
        slot = module.rtti('CMatRenderContext', [1000])['candidates'][0]['candidate_slots']['1000']
        self.assertIsNone(slot['target_rva'])
        self.assertFalse(slot['in_module_executable'])

    def test_invalid_pe_and_section_bounds(self):
        for offset, value, width in [(0, 0, 2), (60, 0xfffffff0, 4),
                                     (0x84, 0x8664, 2), (0x98, 0x20b, 2),
                                     (0xd0, 0x6000, 4), (0x184, 0x5000, 4),
                                     (0x1ac, 0x1000, 4)]:
            data = fixture()
            struct.pack_into('<H' if width == 2 else '<I', data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                image(data)

    def test_registry_candidate_without_rtti_or_factory_execution(self):
        for indirect in (False, True):
            with self.subTest(indirect=indirect):
                data = fixture()
                data[0x3108:0x3120] = b'\0' * 24  # RTTI was stripped.
                name = b'VMaterialSystem079\0'
                data[0x2800:0x2800 + len(name)] = name
                put32(data, 0x3200, BASE + 0x1100, BASE + 0x2800, 0)
                put32(data, 0x3300, BASE + 0x2404)
                put32(data, 0x3400, BASE + 0x3300)
                data[0x1100] = 0xa1 if indirect else 0xb8
                put32(data, 0x1101, BASE + (0x3400 if indirect else 0x3300))
                data[0x1105] = 0xc3
                module = image(data)
                self.assertEqual(module.rtti('CMatRenderContext', [146])['result'], 'unavailable')
                candidate = module.interface_registry()['candidates'][0]
                self.assertEqual(candidate['entry_rva'], 0x3200)
                self.assertEqual(candidate['object_rva'], 0x3300)
                self.assertEqual(candidate['vtable_rva'], 0x2404)
                self.assertFalse(candidate['interface_binding_verified'])
                self.assertFalse(candidate['candidate_slots']['95']['semantics_verified'])
                # An unrecognized getter is retained for inspection, not followed.
                data[0x1100] = 0xcc
                candidate = image(data).interface_registry()['candidates'][0]
                self.assertEqual(candidate['getter_shape'], 'unresolved')
                self.assertIsNone(candidate['object_rva'])

    @unittest.skipIf(os.name == 'nt', 'Linux-only unavailable-path check')
    def test_non_windows_failure_keeps_unrun_boundaries(self):
        result = collect(1234, {'hook_references': []})
        self.assertEqual(result['execution'], 'incomplete')
        self.assertFalse(result['hook_install_allowed'])
        self.assertEqual(result['game_queue_execution'], 'unrun')
        self.assertIn('requires Windows', result['error'])

    @unittest.skipUnless(os.name == 'nt' and os.environ.get('RT_GAME_PROBE_FIXTURE'),
                         'Native Windows x86 fixture supplied by build script')
    def test_live_x64_reader_x86_process_rtti_and_module_snapshot(self):
        self.assertEqual(ctypes.sizeof(ctypes.c_void_p), 8)
        executable = Path(os.environ['RT_GAME_PROBE_FIXTURE']).resolve()
        process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, text=True)
        try:
            expected = json.loads(process.stdout.readline())
            self.assertEqual(expected['pid'], process.pid)
            with WindowsProcess(process.pid) as target:
                start = target.identity()
                self.assertTrue(start['alive'])
                modules = target.modules()
                output = os.environ.get('RT_GAME_PROBE_EVIDENCE')
                if output:
                    Path(output).write_text(json.dumps({'schema': 1, 'result': 'incomplete',
                        'scope': 'external_x64_reader_x86_fixture_not_game_binding',
                        'expected': expected, 'snapshot_modules': modules}, indent=2) + '\n')
                own = [m for m in modules if m['name'] == executable.name.lower()]
                self.assertEqual(len(own), 1, own)
                own = own[0]
                self.assertEqual(own['base'], expected['base'])
                module = MappedImage(target.read, own['base'], own['size'])
                self.assertEqual(module.read_errors, [])
                rtti = module.rtti('ProbeContext', [0, 1, 2])
                candidates = [c for c in rtti['candidates'] if c['vtable_rva'] == expected['vtable_rva']]
                self.assertEqual(len(candidates), 1)
                self.assertEqual(candidates[0]['candidate_slots']['0']['target_rva'], expected['first_rva'])
                self.assertEqual(target.identity()['creation_filetime'], start['creation_filetime'])
                self.assertEqual(file_identity(own['path']), file_identity(executable))
                registry = module.interface_registry()
                entry = [c for c in registry['candidates'] if c['entry_rva'] == expected['registry_rva']]
                self.assertEqual(len(entry), 1)
                self.assertEqual(entry[0]['vtable_rva'], expected['vtable_rva'])
            report = collect(process.pid, {'hook_references': []})
            self.assertEqual(report['execution'], 'incomplete')  # Not a game process.
            self.assertEqual(len(report['modules']), 4)
            self.assertTrue(all(m['result'] == 'unavailable' for m in report['modules']))
            if output:
                Path(output).write_text(json.dumps({'schema': 1, 'result': 'pass',
                    'scope': 'external_x64_reader_x86_fixture_not_game_binding',
                    'expected': expected, 'module': module.evidence([], {'ProbeContext': [0, 1, 2]}),
                    'missing_game_modules': report, 'game_queue_execution': 'unrun'}, indent=2) + '\n')
        finally:
            try:
                process.communicate('q', timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                process.kill(); process.communicate()
        self.assertEqual(process.returncode, 0)


if __name__ == '__main__':
    unittest.main()
