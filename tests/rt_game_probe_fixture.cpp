// Live x86 target for the external read-only Windows collector. No game SDK.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <cstdio>

class ProbeContext {
public:
  virtual int first() { return 17; }
  virtual int second() { return 29; }
  virtual int third() { return 146; }
};

ProbeContext context;

__declspec(noinline) void* create_context() { return &context; }

struct InterfaceEntry {
  void* (*factory)();
  const char* name;
  InterfaceEntry* next;
};

// Layout independently supplied by the fixture; the collector must find it.
InterfaceEntry material_interface = {create_context, "VMaterialSystem079", nullptr};

int main() {
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  const auto table = *reinterpret_cast<const std::uintptr_t* const*>(&context);
  std::printf("{\"pid\":%lu,\"base\":%llu,\"vtable_rva\":%llu,\"first_rva\":%llu,\"registry_rva\":%llu}\n",
    GetCurrentProcessId(), static_cast<unsigned long long>(base),
    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(table) - base),
    static_cast<unsigned long long>(table[0] - base),
    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&material_interface) - base));
  std::fflush(stdout);
  return std::getchar() == 'q' ? 0 : 1;
}
