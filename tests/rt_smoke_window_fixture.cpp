// A real DLL window procedure verifies production cleanup ordering without a GPU.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static WNDPROC previous = nullptr;
static bool destroyed = false;
static const wchar_t* statusPath = nullptr;

LRESULT CALLBACK hooked(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCDESTROY) { destroyed = true; }
  return CallWindowProcW(previous, window, message, w, l);
}

extern "C" __declspec(dllexport) BOOL WINAPI fixture_install(HWND window, const wchar_t* status) {
  statusPath = status;
  previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hooked)));
  return previous != nullptr;
}

extern "C" __declspec(dllexport) unsigned long WINAPI fixture_shutdown() {
  return destroyed && statusPath && GetFileAttributesW(statusPath) != INVALID_FILE_ATTRIBUTES ? 0ul : 1ul;
}
