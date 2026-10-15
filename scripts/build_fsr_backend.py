"""Build the GPU-only Host backend as a separate Windows x64 DLL (or native .so)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
from prepare_research import ROOT,require_source

def build(target):
    source=ROOT/'.research/sources/fidelityfx'
    lock=json.loads((ROOT/'research/sources.lock.json').read_text())
    require_source(source,lock['sources']['fidelityfx']['commit'])
    out=ROOT/'.research/fsr-backend'/target;out.mkdir(parents=True,exist_ok=True)
    gpu=ROOT/'.research/tools/root/usr/bin'
    for name,symbol in [('fsr_inputs','fsrInputsSpirv'),('fsr_compose','fsrComposeSpirv')]:
        spv=out/(name+'.spv')
        subprocess.run([str(gpu/'glslangValidator'),'-V','--target-env','vulkan1.1','-Os',str(ROOT/'shaders'/(name+'.comp')),'-o',str(spv)],check=True)
        subprocess.run([str(gpu/'spirv-val'),'--target-env','vulkan1.1',str(spv)],check=True)
        words=struct.unpack(f'<{len(spv.read_bytes())//4}I',spv.read_bytes())
        (out/(name+'_spirv.h')).write_text('#pragma once\n#include <cstdint>\nstatic const uint32_t '+symbol+'[]={'+','.join(hex(w) for w in words)+'};\n')
    native=ROOT/'.research/fsr-vulkan'
    if not (native/'ffx_vk_adapted.cpp').exists():raise RuntimeError('Build native FSR components first')
    sources=[native/'ffx_vk_adapted.cpp',source/'sdk/src/components/frameinterpolation/ffx_frameinterpolation.cpp',
             source/'sdk/src/components/opticalflow/ffx_opticalflow.cpp',source/'sdk/src/shared/ffx_object_management.cpp',
             source/'sdk/src/shared/ffx_breadcrumbs_list.cpp',ROOT/'tools/fsr_sdk_support.cpp',native/'shader_blobs.cpp',ROOT/'tools/fsr_backend.cpp']
    tools=ROOT/'.research/windows-tools/root/usr/bin'
    env=os.environ.copy();env['PATH']=str(tools)+os.pathsep+env.get('PATH','')
    compiler=str(tools/'x86_64-w64-mingw32-g++-posix') if target=='windows' else 'c++'
    include=[out,ROOT/'tools',ROOT/'integration/ldb',source/'sdk/include',source/'sdk/src/shared',source/'sdk/src/backends/shared',ROOT/'.research/sources/vulkan_headers/include']
    if target!='windows':include.insert(0,native/'include')
    common=[compiler,'-std=c++17','-O2','-DFFX_GCC','-D_DEBUG','-DNOMINMAX','-include',str(ROOT/'tools/fsr_sdk_portability.h'),*[f'-I{p}' for p in include]]
    if target!='windows':common+=['-fPIC']
    objects=[]
    for src in sources:
        obj=out/(src.stem+'.o');cmd=common+[str(src),'-c','-o',str(obj)]
        r=subprocess.run(cmd,env=env,capture_output=True,text=True)
        (out/(src.stem+'.log')).write_text(r.stdout+r.stderr)
        if r.returncode:raise RuntimeError(f'Compile failed: {src}; see {out/(src.stem+".log")}')
        objects.append(obj)
    libs=['-Wl,-l:libvulkan.so.1']
    filename='libLDBFsrBackend64.so'
    if target=='windows':
        names=set()
        for src in [*sources,ROOT/'tools/fsr_component_backend.h']:
            names.update(re.findall(r'\b(vk[A-Z]\w+)\s*\(',src.read_text()))
        names.add('vkGetDeviceProcAddr')
        definition=out/'vulkan.def';definition.write_text('LIBRARY vulkan-1.dll\nEXPORTS\n'+'\n'.join(sorted(names))+'\n')
        imp=out/'libvulkan-1.a'
        subprocess.run([str(tools/'x86_64-w64-mingw32-dlltool'),'-d',str(definition),'-l',str(imp)],env=env,check=True)
        libs=['-static',str(imp)];filename='LDBFsrBackend64.dll'
    dll=out/filename
    subprocess.run([compiler,'-shared',*map(str,objects),*libs,'-o',str(dll)],env=env,check=True)
    (out/'build.json').write_text(json.dumps({'sdk_commit':lock['sources']['fidelityfx']['commit'],
       'target':target,'dll_sha256':hashlib.sha256(dll.read_bytes()).hexdigest(),
       'source_hashes':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
       'motion_source':'camera_plus_bounded_patch; not engine motion vectors'},indent=2)+'\n')
    print(dll)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=['windows','native'],default='windows')
    build(p.parse_args().target)
