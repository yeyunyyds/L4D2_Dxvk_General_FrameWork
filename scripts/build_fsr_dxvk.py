"""Build an isolated pinned DXVK x64 backend with opt-in FSR device features."""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
from prepare_research import ROOT, require_source

out=ROOT/'.research/fsr-dxvk'
lock=json.loads((ROOT/'research/sources.lock.json').read_text())
base=ROOT/'.research/sources/dxvk'
require_source(base,lock['sources']['dxvk']['commit'])
source=out/'source'
out.mkdir(parents=True,exist_ok=True)
if not source.exists():
    source.mkdir()
    data=subprocess.check_output(['git','archive','HEAD'],cwd=base)
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:archive.extractall(source,filter='data')
    for subpath,url in [('include/spirv','https://github.com/KhronosGroup/SPIRV-Headers.git'),
                        ('subprojects/libdisplay-info','https://github.com/doitsujin/libdisplay-info.git')]:
        sha=subprocess.check_output(['git','ls-tree','HEAD',subpath],cwd=base,text=True).split()[2]
        dest=source/subpath
        subprocess.run(['git','init','-q',str(dest)],check=True)
        subprocess.run(['git','-C',str(dest),'fetch','--depth=1',url,sha],check=True)
        subprocess.run(['git','-C',str(dest),'checkout','--detach','FETCH_HEAD'],check=True)
    import shutil
    shutil.copytree(ROOT/'.research/sources/vulkan_headers/include',source/'include/vulkan/include')
    p=source/'src/d3d9/d3d9_device.cpp'
    text=p.read_text()
    anchor='    // Geometry shaders are used for some meta ops'
    assert text.count(anchor)==1
    text=text.replace(anchor,'''    // Explicit opt-in profile; thread and queue topology remain unchanged.
    if (env::getEnvVar("LDB_FG_BACKEND") == "fsr") {
      enabled.core.features.shaderStorageImageExtendedFormats = supported.core.features.shaderStorageImageExtendedFormats;
      enabled.core.features.shaderStorageImageReadWithoutFormat = supported.core.features.shaderStorageImageReadWithoutFormat;
      enabled.core.features.shaderStorageImageWriteWithoutFormat = supported.core.features.shaderStorageImageWriteWithoutFormat;
    }
'''+anchor)
    p.write_text(text)
    p=source/'src/d3d9/d3d9_main.cpp'
    p.write_text(p.read_text()+'\nextern "C" DLLEXPORT unsigned int __stdcall LdbFsrProfileVersion() { return 1; }\n')
    p=source/'src/d3d9/d3d9.def';p.write_text(p.read_text()+'\n  LdbFsrProfileVersion\n')
compiler=ROOT/'.research/windows-tools/root/usr/bin'
p=source/'src/d3d9/d3d9_device.cpp'
text=p.read_text()
if 'enabled.vk12.vulkanMemoryModelDeviceScope =' not in text:
    anchor='      enabled.core.features.shaderStorageImageWriteWithoutFormat = supported.core.features.shaderStorageImageWriteWithoutFormat;'
    assert text.count(anchor)==1
    p.write_text(text.replace(anchor,anchor+'\n      enabled.vk12.vulkanMemoryModelDeviceScope = supported.vk12.vulkanMemoryModelDeviceScope;'))
# Debian's extracted toolchain omits update-alternatives links. windres locates
# its target preprocessor beside itself; never let it fall back to Linux gcc.
alias=compiler/'x86_64-w64-mingw32-gcc'
if not alias.exists():alias.symlink_to('x86_64-w64-mingw32-gcc-posix')
p=source/'src/d3d9/d3d9_main.cpp'
original=subprocess.check_output(['git','show','HEAD:src/d3d9/d3d9_main.cpp'],cwd=base,text=True)
p.write_text(original+'\n#include "d3d9_device.h"\nextern "C" DLLEXPORT unsigned int __stdcall LdbFsrDeviceFeatures(IDirect3DDevice9* device) {\n'
 '  if (!device) return 0;\n  const auto& all = static_cast<dxvk::D3D9DeviceEx*>(device)->GetDXVKDevice()->features();\n  const auto& f=all.core.features;\n'
 '  return (f.shaderStorageImageExtendedFormats ? 1u : 0u) | (f.shaderStorageImageReadWithoutFormat ? 2u : 0u) | (f.shaderStorageImageWriteWithoutFormat ? 4u : 0u) | ((!all.vk12.vulkanMemoryModel || all.vk12.vulkanMemoryModelDeviceScope) ? 8u : 0u);\n}\n')
p=source/'src/d3d9/d3d9.def'
p.write_text(subprocess.check_output(['git','show','HEAD:src/d3d9/d3d9.def'],cwd=base,text=True)+'\n  LdbFsrDeviceFeatures\n')
env=os.environ.copy()
env['PATH']=str(compiler)+os.pathsep+str(ROOT/'.research/tools/root/usr/bin')+os.pathsep+str(ROOT/'.research/python-tools/bin')+os.pathsep+env.get('PATH','')
env['PYTHONPATH']=str(ROOT/'.research/python-tools')
cross=out/'cross.txt'
cross.write_text("[binaries]\n"+'\n'.join(f"{k} = '{compiler/v}'" for k,v in [
 ('c','x86_64-w64-mingw32-gcc-posix'),('cpp','x86_64-w64-mingw32-g++-posix'),
 ('ar','x86_64-w64-mingw32-ar'),('strip','x86_64-w64-mingw32-strip'),('windres','x86_64-w64-mingw32-windres')])+
 "\n[properties]\nneeds_exe_wrapper = true\n[host_machine]\nsystem = 'windows'\ncpu_family = 'x86_64'\ncpu = 'x86_64'\nendian = 'little'\n")
build=out/'build'
if not (build/'build.ninja').exists():
    subprocess.run(['python3','-m','mesonbuild.mesonmain','setup',str(build),str(source),'--cross-file',str(cross),
      '--buildtype=release','-Denable_dxgi=false','-Denable_d3d8=false','-Denable_d3d10=false','-Denable_d3d11=false'],env=env,check=True)
subprocess.run(['python3','-m','mesonbuild.mesonmain','compile','-C',str(build),'-j','2'],env=env,check=True)
dll=build/'src/d3d9/d3d9.dll'
(out/'build.json').write_text(json.dumps({'dxvk_commit':lock['sources']['dxvk']['commit'],
 'opt_in_environment':'LDB_FG_BACKEND=fsr','dll_sha256':hashlib.sha256(dll.read_bytes()).hexdigest(),
 'device_patch_sha256':hashlib.sha256((source/'src/d3d9/d3d9_device.cpp').read_bytes()).hexdigest()},indent=2)+'\n')
print(dll)
