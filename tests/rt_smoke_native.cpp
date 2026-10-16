// Exercise actual diagnostic helpers and a loaded DLL window hook. No GPU pass.
#define wmain diagnostic_entry
#include "../integration/rt/rt_runtime_smoke.cpp"
#undef wmain
#include <vector>

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
