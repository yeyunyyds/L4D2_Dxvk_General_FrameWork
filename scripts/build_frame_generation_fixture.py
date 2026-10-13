"""Build the exact FG runtime with actual Present/backbuffer pixel checks."""
import json
import os
from pathlib import Path
import subprocess
import hashlib
from prepare_research import ROOT
out=ROOT/'.research/fg-fixture';out.mkdir(exist_ok=True)
s=(ROOT/'tools/dxvk_host_capture.cpp').read_text().replace('#include "dxvk_capture_shared.h"','#include "dxvk_capture_shared.h"\n#include "frame_generation.h"')
a=s.index('void draw(');b=s.index('void run(const Options& args)');s=s[:a]+s[b:]
s=s.replace('LARGE_INTEGER frequency{}, start{}, stop{};', 'LARGE_INTEGER frequency{}, start{};').replace('    uint32_t background = D3DCOLOR_XRGB(16 + frame % 100, 32, 192);\n','')
pos=s.index('void run(const Options& args)');s=s[:pos]+'#include "frame_generation_scene.h"\n\n'+s[pos:]
s=s.replace('      hr(device->Reset(&params), "Reset");','      fgFixtureReset();\n      const HRESULT resetResult=device->Reset(&params);\n      ldb_fg::reset(device.get(),frame,resetResult);\n      hr(resetResult,"Reset");')
a=s.index('    draw(device.get(), frame,');b=s.index('\n  }\n  capture.reset();',a)
s=s[:a]+r'''    if(ldb_fg::readEnv(L"LDB_FG_FIXTURE_CAPTURE_CLOCK")==L"1" && (frame==2||frame==7||frame==10)) Sleep(frame==7?21000:2300);
    fgFixtureScene(device.get(),frame,params.BackBufferWidth,params.BackBufferHeight);
    uint32_t calls=0;
    bool special=ldb_fg::readEnv(L"LDB_FG_FIXTURE_CASE")!=L"normal" && !ldb_fg::readEnv(L"LDB_FG_FIXTURE_CASE").empty();
    bool failure=ldb_fg::readEnv(L"LDB_FG_FIXTURE_CASE")==L"failure";
    bool generatedExpected=ldb_fg::config().enabled && frame!=0 && int(frame)!=args.resetFrame && !(special&&(frame==2||frame==3));
    Com<IDirect3DSwapChain9> chain;
    if(frame%2)hr(device->GetSwapChain(0,chain.put()),"FG fixture swapchain");
    HRESULT result=ldb_fg::present(device.get(),frame,[&](){
      bool generated=generatedExpected&&(ldb_fg::config().generatedOnly||calls==0);
      fgFixtureCheck(device.get(),frame,params.BackBufferWidth,params.BackBufferHeight,generated,calls++);
      if(failure && frame==2)return D3DERR_INVALIDCALL;
      return chain.get()?chain->Present(nullptr,nullptr,nullptr,nullptr,0):device->Present(nullptr,nullptr,nullptr,nullptr);
    },special&&frame==2,chain.get());
    if(failure&&frame==2){if(result!=D3DERR_INVALIDCALL)throw std::runtime_error("Original failure HRESULT changed");std::cout<<"{\"event\":\"fixture_failure\",\"hresult_preserved\":true}\n";}else hr(result,"FG fixture Present");
    const uint32_t expectedCalls=generatedExpected&&!ldb_fg::config().generatedOnly?2u:1u;
    if(calls!=expectedCalls)throw std::runtime_error("Expected generated Present did not execute");
'''+s[b:]
s=s.replace('  capture.reset();\n  std::cout << "{\\"event\\":\\"completed\\",','  capture.reset();\n  fgFixtureReset();\n  ldb_fg::destroy(device.get());\n  std::cout << "{\\"event\\":\\"completed\\",')
p=out/'frame_generation_fixture.cpp';p.write_text(s)
compiler=ROOT/'.research/windows-tools/root/usr/bin/x86_64-w64-mingw32-g++-posix';env=os.environ.copy();env['PATH']=str(compiler.parent)+os.pathsep+env.get('PATH','');exe=out/'frame_generation_fixture.exe'
cmd=[str(compiler),'-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-cast-function-type','-static','-municode',*[f'-I{p}' for p in [ROOT/'tools',ROOT/'integration/ldb',ROOT/'tests',ROOT/'.research/bin',ROOT/'.research/sources/vulkan_headers/include']],str(p),'-luser32','-luuid','-o',str(exe)]
subprocess.run(cmd,check=True,env=env);(out/'build.json').write_text(json.dumps({'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'command':cmd,'actual_game':'unrun'},indent=2)+'\n');print(exe)
