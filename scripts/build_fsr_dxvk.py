"""Build an isolated pinned DXVK x64 backend with opt-in FSR device features."""
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
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

# Danyria's replacement-swapchain path reserves four queues in one graphics
# family. Keep that fast path, but expose the actual topology so the Host can
# reject or downgrade unsupported Arc devices instead of requesting invalid
# queue indices. These edits are opt-in and leave normal DXVK unchanged.
p=source/'src/dxvk/dxvk_device.h'
text=p.read_text()
if 'frameGenerationCount' not in text:
    anchor='''  struct DxvkDeviceQueueSet {
    DxvkDeviceQueue graphics;
    DxvkDeviceQueue transfer;
    DxvkDeviceQueue sparse;
  };'''
    replacement='''  struct DxvkDeviceQueueSet {
    DxvkDeviceQueue graphics;
    DxvkDeviceQueue transfer;
    DxvkDeviceQueue sparse;
    DxvkDeviceQueue frameGeneration[3];
    uint32_t frameGenerationCount = 0;
  };'''
    if text.count(anchor)!=1:raise RuntimeError('Unexpected DXVK queue set')
    p.write_text(text.replace(anchor,replacement))

p=source/'src/dxvk/dxvk_adapter.cpp'
text=p.read_text()
if 'LDB_FG_PRESENTER' not in text:
    anchor='''    // Create the requested queues
    float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;'''
    replacement='''    // Create the requested queues. The opt-in FG profile reserves up to
    // three additional queues from the graphics family for the FFX presenter.
    std::vector<float> queuePriorities(4, 1.0f);
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    const bool enableFgPresenter = env::getEnvVar("LDB_FG_PRESENTER") == "1";
    uint32_t fgGraphicsQueueCount = 1;'''
    if text.count(anchor)!=1:raise RuntimeError('Unexpected DXVK queue preamble')
    text=text.replace(anchor,replacement)
    anchor='''      graphicsQueue.queueFamilyIndex  = family;
      graphicsQueue.queueCount        = 1;
      graphicsQueue.pQueuePriorities  = &queuePriority;
      queueInfos.push_back(graphicsQueue);'''
    replacement='''      graphicsQueue.queueFamilyIndex  = family;
      graphicsQueue.queueCount        = enableFgPresenter && family == queueFamilies.graphics
        ? std::min(4u, m_queueFamilies[family].queueCount)
        : 1u;
      graphicsQueue.pQueuePriorities  = queuePriorities.data();
      if (family == queueFamilies.graphics)
        fgGraphicsQueueCount = graphicsQueue.queueCount;
      queueInfos.push_back(graphicsQueue);'''
    if text.count(anchor)!=1:raise RuntimeError('Unexpected DXVK queue request')
    text=text.replace(anchor,replacement)
    anchor='''    queues.graphics = getDeviceQueue(vkd, queueFamilies.graphics, 0);
    queues.transfer = getDeviceQueue(vkd, queueFamilies.transfer, 0);
    queues.sparse = getDeviceQueue(vkd, queueFamilies.sparse, 0);'''
    replacement='''    queues.graphics = getDeviceQueue(vkd, queueFamilies.graphics, 0);
    queues.transfer = getDeviceQueue(vkd, queueFamilies.transfer, 0);
    queues.sparse = getDeviceQueue(vkd, queueFamilies.sparse, 0);
    queues.frameGenerationCount = fgGraphicsQueueCount > 0 ? fgGraphicsQueueCount - 1 : 0;
    for (uint32_t i = 0; i < queues.frameGenerationCount; i++)
      queues.frameGeneration[i] = getDeviceQueue(vkd, queueFamilies.graphics, i + 1);'''
    if text.count(anchor)!=1:raise RuntimeError('Unexpected DXVK queue retrieval')
    p.write_text(text.replace(anchor,replacement))

shutil.copy2(ROOT/'tools/fsr_presenter_api.h',source/'src/d3d9/ldb_fg_presenter_api.h')
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
p.write_text(original+'\n#include "d3d9_device.h"\n#include "ldb_fg_presenter_api.h"\n#include <algorithm>\n#include <vector>\nextern "C" DLLEXPORT unsigned int __stdcall LdbFsrDeviceFeatures(IDirect3DDevice9* device) {\n'
 '  if (!device) return 0;\n  const auto& all = static_cast<dxvk::D3D9DeviceEx*>(device)->GetDXVKDevice()->features();\n  const auto& f=all.core.features;\n'
 '  return (f.shaderStorageImageExtendedFormats ? 1u : 0u) | (f.shaderStorageImageReadWithoutFormat ? 2u : 0u) | (f.shaderStorageImageWriteWithoutFormat ? 4u : 0u) | ((!all.vk12.vulkanMemoryModel || all.vk12.vulkanMemoryModelDeviceScope) ? 8u : 0u);\n}\n'
 '''extern "C" DLLEXPORT unsigned int __stdcall LdbFgQueryDeviceInfoV1(void* opaque, LdbFgDeviceInfoV1* out) {
  if (!opaque || !out || out->version != LDB_FG_DEVICE_API_VERSION || out->size != sizeof(*out)) return 0;
  auto device = static_cast<dxvk::D3D9DeviceEx*>(reinterpret_cast<IDirect3DDevice9*>(opaque))->GetDXVKDevice();
  const auto& queues = device->queues();
  LdbFgDeviceInfoV1 result;
  result.flags = LDB_FG_DEVICE_API_READY;
  result.graphicsFamily = queues.graphics.queueFamily;
  result.requestedGraphicsQueues = queues.frameGenerationCount + 1;
  result.reservedQueueCount = queues.frameGenerationCount;
  result.featureFlags = LdbFsrDeviceFeatures(reinterpret_cast<IDirect3DDevice9*>(opaque));
  result.instance = reinterpret_cast<uintptr_t>(device->instance()->handle());
  result.physicalDevice = reinterpret_cast<uintptr_t>(device->adapter()->handle());
  result.device = reinterpret_cast<uintptr_t>(device->handle());
  result.gameQueue = reinterpret_cast<uintptr_t>(queues.graphics.queueHandle);
  if (queues.frameGenerationCount >= 3) {
    result.asyncComputeQueue = reinterpret_cast<uintptr_t>(queues.frameGeneration[0].queueHandle);
    result.presentQueue = reinterpret_cast<uintptr_t>(queues.frameGeneration[1].queueHandle);
    result.imageAcquireQueue = reinterpret_cast<uintptr_t>(queues.frameGeneration[2].queueHandle);
    result.flags |= LDB_FG_DEVICE_API_ASYNC_COMPUTE_QUEUE | LDB_FG_DEVICE_API_PRESENTER_QUEUES;
  } else if (queues.frameGenerationCount >= 2) {
    result.presentQueue = reinterpret_cast<uintptr_t>(queues.frameGeneration[0].queueHandle);
    result.imageAcquireQueue = reinterpret_cast<uintptr_t>(queues.frameGeneration[1].queueHandle);
    result.flags |= LDB_FG_DEVICE_API_PRESENTER_QUEUES;
  }
  *out = result;
  return result.flags;
}
extern "C" DLLEXPORT unsigned int __stdcall LdbFgQueryQueueTopologyV1(void* opaque, LdbFgQueueTopologyV1* out) {
  if (!opaque || !out || out->version != LDB_FG_QUEUE_TOPOLOGY_API_VERSION || out->size != sizeof(*out)) return 0;
  auto device = static_cast<dxvk::D3D9DeviceEx*>(reinterpret_cast<IDirect3DDevice9*>(opaque))->GetDXVKDevice();
  uint32_t familyCount = 0;
  device->vki()->vkGetPhysicalDeviceQueueFamilyProperties(device->adapter()->handle(), &familyCount, nullptr);
  std::vector<VkQueueFamilyProperties> properties(familyCount);
  device->vki()->vkGetPhysicalDeviceQueueFamilyProperties(device->adapter()->handle(), &familyCount, properties.data());
  LdbFgQueueTopologyV1 result;
  result.familyCount = std::min(familyCount, LDB_FG_MAX_QUEUE_FAMILIES);
  for (uint32_t i = 0; i < result.familyCount; i++) {
    result.families[i].queueFlags = properties[i].queueFlags;
    result.families[i].physicalQueueCount = properties[i].queueCount;
  }
  const auto& queues = device->queues();
  auto add = [&](const dxvk::DxvkDeviceQueue& queue) {
    if (!queue.queueHandle || queue.queueFamily >= result.familyCount) return;
    auto& family = result.families[queue.queueFamily];
    for (uint32_t i = 0; i < family.createdQueueCount; i++)
      if (family.createdQueues[i] == reinterpret_cast<uintptr_t>(queue.queueHandle)) return;
    if (family.createdQueueCount < LDB_FG_MAX_CREATED_QUEUES_PER_FAMILY)
      family.createdQueues[family.createdQueueCount++] = reinterpret_cast<uintptr_t>(queue.queueHandle);
  };
  add(queues.graphics); add(queues.transfer); add(queues.sparse);
  for (uint32_t i = 0; i < queues.frameGenerationCount; i++) add(queues.frameGeneration[i]);
  for (uint32_t i = 0; i < result.familyCount; i++) result.createdFamilyCount += result.families[i].createdQueueCount != 0;
  *out = result;
  return 1;
}
''')
p=source/'src/d3d9/d3d9.def'
p.write_text(subprocess.check_output(['git','show','HEAD:src/d3d9/d3d9.def'],cwd=base,text=True)+'\n  LdbFsrDeviceFeatures\n  LdbFgQueryDeviceInfoV1\n  LdbFgQueryQueueTopologyV1\n')
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
 'presenter_queue_profile':'LDB_FG_PRESENTER=1 reserves up to four graphics queues; Host queries LdbFgQueryDeviceInfoV1',
 'patched_source_sha256':{str(p.relative_to(source)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [
   source/'src/dxvk/dxvk_adapter.cpp',source/'src/dxvk/dxvk_device.h',
   source/'src/d3d9/d3d9_device.cpp',source/'src/d3d9/d3d9_main.cpp',
   source/'src/d3d9/ldb_fg_presenter_api.h'] }},indent=2)+'\n')
print(dll)
