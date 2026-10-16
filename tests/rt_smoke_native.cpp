// Exercise actual diagnostic helpers and a loaded DLL window hook. No GPU pass.
#define wmain diagnostic_entry
#include "../integration/rt/rt_runtime_smoke.cpp"
#undef wmain
#include <vector>
#include <limits>
#include "pinned-alpha-fixture.h"

void require(bool condition, const char* reason) {
  if (!condition) { throw std::runtime_error(reason); }
}

template<class Function> Function procedure(HMODULE module, const char* name) {
  const auto pointer = GetProcAddress(module, name);
  require(pointer != nullptr, name);
  Function result = nullptr;
  static_assert(sizeof(result) == sizeof(pointer));
  std::memcpy(&result, &pointer, sizeof(result));
  return result;
}

int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 3, "fixture DLL and output directory required");
    const fs::path output(argv[2]);
    require(fs::create_directory(output), "use new fixture output");
    static_assert(sizeof(remixapi_HardcodedVertex) == 64, "pinned API vertex stride");
    static_assert(offsetof(remixapi_HardcodedVertex, texcoord) == 24, "pinned API UV offset");
    static_assert(offsetof(remixapi_HardcodedVertex, color) == 32, "pinned API color offset");
    auto opaque = opaqueTriangleMaterial();
    require(validOpaqueTriangleMaterial(opaque), "production material alpha must allow opaque pixels");
    require(pinned_alpha_fixture::evaluate(opaque.alphaTestType, opaque.opacityConstant) == 1.f,
            "real pinned shader alpha prefix accepts production material");
    // Reproduce the prior production initialization through the same upstream
    // shader branch, rather than inventing a second alpha-test implementation.
    opaque.alphaTestType = 0;
    require(!validOpaqueTriangleMaterial(opaque), "reject prior zero-initialized NEVER alpha state");
    require(pinned_alpha_fixture::evaluate(opaque.alphaTestType, opaque.opacityConstant) == 0.f,
            "real pinned shader alpha prefix rejects the prior production material");
    opaque = opaqueTriangleMaterial(); opaque.opacityConstant = 0;
    require(!validOpaqueTriangleMaterial(opaque), "reject transparent fixed-scene material");
    opaque = opaqueTriangleMaterial(); opaque.useDrawCallAlphaState = TRUE;
    require(!validOpaqueTriangleMaterial(opaque), "reject unexpected alpha-state ownership");
    std::cout << "PINNED_SHADER_ALPHA_CONTRACT_PASS: old NEVER=0, fixed ALWAYS=1; GPU unrun\n";
    auto triangle = triangleVertices();
    require(validTriangle(triangle), "production triangle has finite normals and independent UVs");
    require(std::abs(uvDeterminant(triangle) - 1.0f) < 1e-6f, "nondegenerate production UV determinant");
    for (auto& vertex : triangle) { vertex.texcoord[0] = 0; vertex.texcoord[1] = 0; }
    require(!validTriangle(triangle), "reject old all-zero UV input");
    triangle = triangleVertices(); triangle[0].normal[0] = std::numeric_limits<float>::quiet_NaN();
    require(!validTriangle(triangle), "reject nonfinite normal");
    triangle = triangleVertices(); triangle[0].texcoord[0] = std::numeric_limits<float>::infinity();
    require(!validTriangle(triangle), "reject nonfinite UV");
    PickResult pick;
    Outcome outcome;
    outcome.failedAt = "verify_pick_and_readback";
    outcome.error = "callback missing, but color readback completed";
    // Nonzero pitch padding and BGRA ordering exercise the real PPM writer.
    const unsigned pitch = kWidth * 4 + 16;
    std::vector<uint8_t> pixels(pitch * kHeight, 0);
    const auto center = (kHeight / 2) * pitch + (kWidth / 2) * 4;
    pixels[center] = 10; pixels[center + 1] = 30; pixels[center + 2] = 200;
    D3DLOCKED_RECT locked{INT(pitch), pixels.data()};
    auto stats = writeImage(locked, output / "hot-pixel.ppm");
    require(!stats.hasSignal() && stats.nonblack == 1, "one hot pixel must fail visual filter");
    require(stats.center[0] == 200 && stats.center[1] == 30 && stats.center[2] == 10, "BGRA to RGB");
    // The observed GPU failure had only 1..5/255. It must not be accepted.
    for (unsigned y = 0; y < kHeight; ++y) {
      for (unsigned x = 0; x < kWidth; ++x) {
        const auto at = y * pitch + x * 4;
        pixels[at] = 2; pixels[at + 1] = 3; pixels[at + 2] = uint8_t(1 + x % 5);
      }
    }
    require(!writeImage(locked, output / "near-black.ppm").hasSignal(), "near-black dither must fail");
    // A broad, centered orange shape with a dark background exercises the
    // native first filter; the independent Python tests verify full geometry.
    for (unsigned y = 120; y < 240; ++y) {
      for (unsigned x = 220; x < 420; ++x) {
        const auto at = y * pitch + x * 4;
        pixels[at] = 10; pixels[at + 1] = 80; pixels[at + 2] = 200;
      }
    }
    stats = writeImage(locked, output / "independent-color.ppm");
    require(stats.hasSignal(), "visible warm region must reach independent geometry checks");
    require(!pick.called.load(), "readback must not require a picking callback");
    outcome.captures = 1; outcome.signal = stats.hasSignal();
    const auto status = output / "status.json";
    outcome.save(status, pick, "pending");
    require(fs::file_size(status) > 0 && !outcome.passed, "image without hit cannot pass");

    Log log(output / "cleanup-events.jsonl");
    Runtime runtime;
    runtime.module = LoadLibraryW(argv[1]);
    require(runtime.module != nullptr, "load window-hook DLL");
    using Install = BOOL (WINAPI*)(HWND, const wchar_t*);
    const auto install = procedure<Install>(runtime.module, "fixture_install");
    runtime.api.Shutdown = procedure<decltype(runtime.api.Shutdown)>(runtime.module, "fixture_shutdown");
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"RtSmokeNativeLifetime";
    require(RegisterClassW(&wc) != 0, "register fixture window");
    runtime.window = CreateWindowW(wc.lpszClassName, L"fixture", 0, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    require(runtime.window != nullptr, "create fixture window");
    const auto statusText = status.wstring();
    require(install(runtime.window, statusText.c_str()) != FALSE, "install real DLL hook");
    runtime.registered = true;
    runtime.cleanup(log); // DLL checks WM_NCDESTROY and pre-existing status here.
    require(runtime.window == nullptr && !runtime.registered, "cleanup completion");
    require(GetModuleHandleW(fs::path(argv[1]).filename().c_str()) != nullptr, "DLL remains mapped after cleanup");
    outcome.save(status, pick, "complete");

    // The actual handler writes an owned-process normal minidump and a code.
    crashPath = (output / "crash.json").wstring(); dumpPath = (output / "crash.dmp").wstring();
    CONTEXT context{}; RtlCaptureContext(&context);
    EXCEPTION_RECORD record{}; record.ExceptionCode = 0xe042534d;
    EXCEPTION_POINTERS exception{&record, &context};
    require(crashReport(&exception) == EXCEPTION_EXECUTE_HANDLER, "exception handler result");
    require(fs::file_size(output / "crash.json") > 0 && fs::file_size(output / "crash.dmp") > 32, "preserved crash evidence");
    std::cout << "SMOKE_HELPERS_AND_REAL_DLL_WINDOW_CLEANUP_PASS; GPU execution unrun\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
