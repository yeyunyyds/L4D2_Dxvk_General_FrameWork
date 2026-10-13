// Standalone diagnostic scene. No game injection, IPC changes or frame generation.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <vulkan/vulkan.h>
#include "dxvk_capture_shared.h"

#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static_assert(sizeof(void*) == 8, "Build this diagnostic for Windows x64");

namespace {

using namespace dxvk_capture;

struct Options {
  std::filesystem::path dll;
  std::string adapter = "B580";
  std::string runId;
  std::string depthFormat = "auto";
  uint32_t frames = 60, width = 640, height = 360;
  int resetFrame = -2;
};

Options options(int argc, wchar_t** argv) {
  Options out;
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) throw std::runtime_error("Arguments require values");
    std::wstring key = argv[index], value = argv[index + 1];
    if (key == L"--dxvk") out.dll = value;
    else if (key == L"--adapter") out.adapter = utf8(value);
    else if (key == L"--run-id") out.runId = utf8(value);
    else if (key == L"--depth-format") {
      out.depthFormat = utf8(value);
      if (out.depthFormat != "auto" && out.depthFormat != "d32" && out.depthFormat != "d24")
        throw std::runtime_error("Depth format must be auto, d32 or d24");
    }
    else {
      size_t consumed = 0;
      int number = std::stoi(value, &consumed);
      if (consumed != value.size()) throw std::runtime_error("Invalid numeric argument");
      if (key == L"--frames" && number >= 2 && number <= 1000) out.frames = uint32_t(number);
      else if (key == L"--width" && number >= 128 && number <= 4096) out.width = uint32_t(number);
      else if (key == L"--height" && number >= 128 && number <= 4096) out.height = uint32_t(number);
      else if (key == L"--reset-frame") out.resetFrame = number;
      else throw std::runtime_error("Unknown option or invalid range");
    }
  }
  if (out.dll.empty() || out.runId.empty() || out.adapter.empty())
    throw std::runtime_error("Required: --dxvk <x64 DLL> --run-id <id>; optional --adapter B580 --frames 60 --width 640 --height 360 --reset-frame 30");
  if (out.resetFrame == -2) out.resetFrame = int(out.frames / 2);
  if (out.resetFrame < -1 || out.resetFrame == 0 || out.resetFrame >= int(out.frames))
    throw std::runtime_error("Reset frame must be -1 (disabled) or between 1 and frames-1");
  out.dll = std::filesystem::absolute(out.dll);
  return out;
}

HMODULE loadDxvk(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  IMAGE_DOS_HEADER dos{};
  input.read(reinterpret_cast<char*>(&dos), sizeof(dos));
  if (!input || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < int(sizeof(dos)) || dos.e_lfanew > 16 * 1024 * 1024)
    throw std::runtime_error("Selected DLL is not a valid PE file");
  input.seekg(dos.e_lfanew);
  DWORD signature = 0;
  IMAGE_FILE_HEADER header{};
  input.read(reinterpret_cast<char*>(&signature), sizeof(signature));
  input.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (!input || signature != IMAGE_NT_SIGNATURE || header.Machine != IMAGE_FILE_MACHINE_AMD64 || !(header.Characteristics & IMAGE_FILE_DLL))
    throw std::runtime_error("Selected backend must be a Windows AMD64 DLL");
  auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!module) throw std::runtime_error("LoadLibraryExW failed: " + std::to_string(GetLastError()));
  // Keep DXVK loaded until process exit, after all driver/device threads stop.
  return module;
}

class Window {
public:
  Window(uint32_t width, uint32_t height) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"L4D2DxvkCaptureDiagnostic";
    if (!RegisterClassW(&wc)) throw std::runtime_error("RegisterClassW failed");
    RECT area{0, 0, LONG(width), LONG(height)};
    if (!AdjustWindowRect(&area, WS_OVERLAPPEDWINDOW, FALSE)) throw std::runtime_error("AdjustWindowRect failed");
    handle = CreateWindowW(wc.lpszClassName, L"DXVK capture diagnostic", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, area.right-area.left, area.bottom-area.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!handle) throw std::runtime_error("CreateWindowW failed");
    ShowWindow(handle, SW_SHOW);
  }
  ~Window() { if (handle) DestroyWindow(handle); }
  void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) throw std::runtime_error("Window closed before capture completed");
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (!IsWindow(handle)) throw std::runtime_error("Window closed before capture completed");
  }
  HWND handle = nullptr;
};


void draw(IDirect3DDevice9* device, uint32_t frame, uint32_t width, uint32_t height, uint32_t background) {
  hr(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, background, 1.0f, 0), "Clear");
  hr(device->BeginScene(), "BeginScene");
  hr(device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE), "ZENABLE");
  hr(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE), "ZWRITEENABLE");
  hr(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS), "ZFUNC");
  hr(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "CULLMODE");
  hr(device->SetRenderState(D3DRS_LIGHTING, FALSE), "LIGHTING");
  hr(device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE), "SRGBWRITEENABLE");
  hr(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "COLOROP");
  hr(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "COLORARG1");
  hr(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE), "SetFVF");
  struct Vertex { float x, y, z, rhw; DWORD color; };
  float shift = std::sin(float(frame) * 0.1f) * float(width) * 0.04f;
  std::array<Vertex, 3> vertices{{
    {float(width)*0.1f+shift, float(height)*0.1f, 0.25f, 1, D3DCOLOR_XRGB(32,220,64)},
    {float(width)*0.9f+shift, float(height)*0.1f, 0.25f, 1, D3DCOLOR_XRGB(32,220,64)},
    {float(width)*0.5f+shift, float(height)*0.9f, 0.25f, 1, D3DCOLOR_XRGB(32,220,64)}
  }};
  hr(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices.data(), sizeof(Vertex)), "DrawPrimitiveUP");
  hr(device->EndScene(), "EndScene");
}

void run(const Options& args) {
  Window window(args.width, args.height);
  HMODULE module = loadDxvk(args.dll);
  using Create9 = IDirect3D9* (WINAPI*)(UINT);
  auto create = reinterpret_cast<Create9>(GetProcAddress(module, "Direct3DCreate9"));
  if (!create) throw std::runtime_error("Selected DLL does not export Direct3DCreate9");
  Com<IDirect3D9> api;
  *api.put() = create(D3D_SDK_VERSION);
  if (!api.get()) throw std::runtime_error("Direct3DCreate9 failed");
  UINT selected = UINT_MAX;
  D3DADAPTER_IDENTIFIER9 identifier{};
  for (UINT index = 0; index < api->GetAdapterCount(); index++) {
    hr(api->GetAdapterIdentifier(index, 0, &identifier), "GetAdapterIdentifier");
    if (std::string(identifier.Description).find(args.adapter) != std::string::npos) { selected = index; break; }
  }
  if (selected == UINT_MAX) throw std::runtime_error("No adapter matching " + args.adapter + "; no fallback device used");
  D3DFORMAT depthFormat = args.depthFormat == "d24" ? D3DFMT_D24S8 : D3DFMT_D32F_LOCKABLE;
  if (args.depthFormat == "auto" && FAILED(api->CheckDeviceFormat(selected, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, depthFormat)))
    depthFormat = D3DFMT_D24S8;
  D3DPRESENT_PARAMETERS params{};
  params.BackBufferWidth = args.width; params.BackBufferHeight = args.height;
  params.BackBufferFormat = D3DFMT_A8R8G8B8; params.BackBufferCount = 1;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD; params.hDeviceWindow = window.handle;
  params.Windowed = TRUE; params.EnableAutoDepthStencil = TRUE; params.AutoDepthStencilFormat = depthFormat;
  params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  Com<IDirect3DDevice9> device;
  hr(api->CreateDevice(selected, D3DDEVTYPE_HAL, window.handle, D3DCREATE_HARDWARE_VERTEXPROCESSING,
    &params, device.put()), "CreateDevice");
  Com<ID3D9VkInteropDevice> interop;
  hr(device->QueryInterface(__uuidof(ID3D9VkInteropDevice), reinterpret_cast<void**>(interop.put())), "Device Vulkan interop QI");
  auto capture = std::make_unique<Capture>(interop.get());
  if (std::string(capture->physicalProperties.deviceName).find(args.adapter) == std::string::npos)
    throw std::runtime_error("Physical Vulkan device does not match the requested adapter");
  std::cout << "{\"event\":\"device\",\"run_id\":" << quote(args.runId) << ",\"adapter\":" << quote(identifier.Description)
    << ",\"backend\":" << quote(utf8(args.dll.wstring())) << ",\"frames_requested\":" << args.frames
    << ",\"reset_frame\":" << args.resetFrame << ",\"width\":" << params.BackBufferWidth << ",\"height\":" << params.BackBufferHeight
    << ",\"depth_d3d_format\":" << depthFormat << ",\"physical_device_name\":" << quote(capture->physicalProperties.deviceName)
    << ",\"physical_vendor_id\":" << capture->physicalProperties.vendorID << ",\"submission_queue_family\":" << capture->family
    << ",\"gpu_fg_execution\":\"unrun\",\"diagnostic_gpu_waits\":true}\n" << std::flush;
  uint32_t generation = 0;
  LARGE_INTEGER frequency{}, start{}, stop{};
  if (!QueryPerformanceFrequency(&frequency)) throw std::runtime_error("QueryPerformanceFrequency failed");
  for (uint32_t frame = 0; frame < args.frames; frame++) {
    window.pump();
    if (int(frame) == args.resetFrame) {
      capture.reset();
      params.BackBufferWidth = args.width - 64; params.BackBufferHeight = args.height - 64;
      hr(device->Reset(&params), "Reset");
      generation++;
      std::cout << "{\"event\":\"reset\",\"run_id\":" << quote(args.runId) << ",\"generation\":" << generation
        << ",\"before_frame\":" << frame << ",\"width\":" << params.BackBufferWidth << ",\"height\":" << params.BackBufferHeight << "}\n";
    }
    QueryPerformanceCounter(&start);
    uint32_t background = D3DCOLOR_XRGB(16 + frame % 100, 32, 192);
    draw(device.get(), frame, params.BackBufferWidth, params.BackBufferHeight, background);
    {
      Com<IDirect3DSurface9> colorSurface, depthSurface;
      hr(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, colorSurface.put()), "GetBackBuffer");
      hr(device->GetDepthStencilSurface(depthSurface.put()), "GetDepthStencilSurface");
      Com<ID3D9VkInteropTexture> colorInterop, depthInterop;
      auto color = imageInfo(colorSurface.get(), colorInterop), depth = imageInfo(depthSurface.get(), depthInterop);
      if (!capture) capture = std::make_unique<Capture>(interop.get());
      if (!capture->allocated()) capture->initialize(color, depth);
      VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      interop->TransitionTextureLayout(colorInterop.get(), &colorRange, color.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkImageSubresourceRange depthRange{capture->aspects[1],0,1,0,1};
      interop->TransitionTextureLayout(depthInterop.get(), &depthRange, depth.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      interop->FlushRenderingCommands();
      if (!interop->WaitForResource(colorSurface.get(), 0) || !interop->WaitForResource(depthSurface.get(), 0))
        throw std::runtime_error("DXVK resource wait failed");
      capture->copy(color, depth);
      capture->verify(background);
      interop->TransitionTextureLayout(colorInterop.get(), &colorRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color.layout);
      interop->TransitionTextureLayout(depthInterop.get(), &depthRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth.layout);
      interop->FlushRenderingCommands();
      if (!interop->WaitForResource(colorSurface.get(), 0) || !interop->WaitForResource(depthSurface.get(), 0))
        throw std::runtime_error("DXVK restoration wait failed");
      hr(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
      QueryPerformanceCounter(&stop);
      double elapsed = double(stop.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart);
      std::cout << "{\"event\":\"frame\",\"run_id\":" << quote(args.runId) << ",\"render_frame_id\":" << frame
        << ",\"generation\":" << generation << ",\"color\":" << imageJson(color) << ",\"depth\":" << imageJson(depth)
        << ",\"color_copy_verified\":true,\"depth_copy_verified\":true,\"source_layout_restored\":true,\"present_succeeded\":true"
        << ",\"diagnostic_cpu_ms\":" << elapsed << "}\n" << std::flush;
    }
  }
  capture.reset();
  std::cout << "{\"event\":\"completed\",\"run_id\":" << quote(args.runId) << ",\"frames\":" << args.frames
    << ",\"resets\":" << generation << ",\"gpu_fg_execution\":\"unrun\"}\n" << std::flush;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  std::string runId;
  try {
    auto args = options(argc, argv); runId = args.runId;
    run(args); return 0;
  } catch (const std::exception& error) {
    std::cout << "{\"event\":\"error\",\"run_id\":" << quote(runId) << ",\"message\":" << quote(error.what()) << "}\n" << std::flush;
    return 2;
  }
}
