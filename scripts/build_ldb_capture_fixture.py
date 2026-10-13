"""Compile the exact Host observer with a real DXVK diagnostic scene, not IPC/game."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

from prepare_research import ROOT

out = ROOT / '.research/ldb-fixture'
out.mkdir(parents=True, exist_ok=True)
s = (ROOT / 'tools/dxvk_host_capture.cpp').read_text()
def replace(a, b):
    global s
    if s.count(a) != 1:
        raise RuntimeError(f'Unexpected diagnostic fixture anchor: {a}')
    s = s.replace(a, b)
replace('#include "dxvk_capture_shared.h"', '#include "dxvk_capture_shared.h"\n#include "host_capture.h"')
replace('  hr(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, background, 1.0f, 0), "Clear");',
        '  ldb_capture::boundary(device, "Clear", frame, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER);\n  hr(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, background, 1.0f, 0), "Clear");')
anchor = '      hr(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");'
replace('    draw(device.get(), frame, params.BackBufferWidth, params.BackBufferHeight, background);',
        '    draw(device.get(), frame, params.BackBufferWidth, params.BackBufferHeight, background);\n    ldb_capture::draw(device.get(), "DrawPrimitiveUP", frame, S_OK, 1);\n    draw(device.get(), frame, params.BackBufferWidth, params.BackBufferHeight, background);')
replace(anchor, '''      ldb_capture::draw(device.get(), "DrawPrimitiveUP", frame, S_OK, 1);
      HRESULT presented = S_OK;
      if (frame % 2) {
        Com<IDirect3DSwapChain9> chain;
        hr(device->GetSwapChain(0, chain.put()), "fixture GetSwapChain");
        ldb_capture::swapBefore(chain.get(), frame);
        presented = chain->Present(nullptr, nullptr, nullptr, nullptr, 0);
        ldb_capture::swapAfter(chain.get(), frame, presented);
      } else {
        ldb_capture::beforePresent(device.get(), frame);
        presented = device->Present(nullptr, nullptr, nullptr, nullptr);
        ldb_capture::afterPresent(device.get(), frame, presented);
      }
      hr(presented, "Present");''')
replace('      hr(device->Reset(&params), "Reset");',
        '      ldb_capture::reset(device.get(), frame, D3DERR_INVALIDCALL); // failed Reset must not advance generation\n      const HRESULT resetResult = device->Reset(&params);\n      ldb_capture::reset(device.get(), frame, resetResult);\n      hr(resetResult, "Reset");')
replace('  capture.reset();\n  std::cout << "{\\"event\\":\\"completed\\",',
        '  capture.reset();\n  ldb_capture::destroy(device.get());\n  std::cout << "{\\"event\\":\\"completed\\",')
source = out / 'host_observer_fixture.cpp'
source.write_text('// Generated test fixture; executes the exact Host observer, not LDB IPC.\n' + s)
compiler = ROOT / '.research/windows-tools/root/usr/bin/x86_64-w64-mingw32-g++-posix'
env = os.environ.copy()
env['PATH'] = str(compiler.parent) + os.pathsep + env.get('PATH', '')
exe = out / 'host_observer_fixture.exe'
command = [str(compiler), '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-cast-function-type',
           '-static', '-municode', *[f'-I{p}' for p in [ROOT/'tools', ROOT/'integration/ldb', ROOT/'.research/bin', ROOT/'.research/sources/vulkan_headers/include']],
           str(source), '-luser32', '-luuid', '-o', str(exe)]
subprocess.run(command, check=True, env=env)
(out / 'build.json').write_text(json.dumps({'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
    'observer_sha256': hashlib.sha256((ROOT/'integration/ldb/host_capture.h').read_bytes()).hexdigest(),
    'command': command, 'l4d2_game_capture': 'unrun', 'full_host_build': 'unrun'}, indent=2)+'\n')
print(exe)
