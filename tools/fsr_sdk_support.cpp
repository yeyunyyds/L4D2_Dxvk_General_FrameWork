// Explicit component-only backend: no WSI proxy is linked or silently emulated.
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_assert.h>
#include <FidelityFX/host/ffx_message.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

FfxErrorCode
ffxSetFrameGenerationConfigToSwapchainVK(const FfxFrameGenerationConfig *) {
  return FFX_ERROR_INVALID_ARGUMENT;
}
void ffxSetPrintMessageCallback(ffxMessageCallback, uint32_t) {}
void ffxPrintMessage(uint32_t type, const wchar_t *message) {
  std::fprintf(stderr, "SDK message %u: %ls\n", type, message);
  if (type == FFX_MESSAGE_TYPE_ERROR)
    std::abort();
}
void ffxAssertSetPrintingCallback(FfxAssertCallback) {}
bool ffxAssertReport(const char *file, int32_t line, const char *condition,
                     const char *message) {
  std::fprintf(stderr, "SDK assertion: %s:%d: %s\n", file, line,
               message     ? message
               : condition ? condition
                           : "unknown");
  throw std::runtime_error(message     ? message
                           : condition ? condition
                                       : "SDK assertion");
}
