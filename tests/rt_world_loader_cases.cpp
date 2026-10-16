#include "rt_world_runtime.h"
#include <iostream>

int wmain(int argc,wchar_t** argv) {
  namespace fs=std::filesystem;
  using namespace ldb_rt;
  try {
    if(argc!=3){throw std::runtime_error("runtime directory and new evidence directory required");}
    const fs::path runtime(argv[1]),output(argv[2]);
    if(!runtime.is_absolute() || !fs::create_directory(output)){throw std::runtime_error("preserve existing loader evidence");}
    draw_observe::Request request;request.world=true;request.session=42;
    request.runtime=output/"wrong-runtime";request.output=output/"negative";
    fs::create_directory(request.runtime);fs::create_directory(request.output);
    {std::ofstream file(request.runtime/"d3d9.dll");file << "wrong runtime identity; never executable";}
    if(world_runtime::load(request) || world_runtime::state().active){throw std::runtime_error("wrong runtime accepted");}
    world_runtime::shutdown();
    request.runtime=runtime;request.output=output/"positive";fs::create_directory(request.output);
    {std::ofstream file(request.output/"world.conf");file << "rtx.useVertexCapture = False\n";}
    const auto module=world_runtime::load(request);
    if(!module || !world_runtime::state().active || !world_runtime::state().bind || !world_runtime::state().capture ||
       !GetProcAddress(module,"Direct3DCreate9")){throw std::runtime_error("pinned loader/API initialization failed");}
    // No device/window/GPU is created. In particular do not use API Shutdown,
    // which force-releases an externally-owned D3D9 device when registered.
    world_runtime::shutdown();
    std::cout << "Pinned world loader/CNG/0.6.4 config: PASS; GPU/game execution unrun\n";
    return 0;
  }catch(const std::exception& error){std::cerr << error.what() << '\n';return 1;}
}
