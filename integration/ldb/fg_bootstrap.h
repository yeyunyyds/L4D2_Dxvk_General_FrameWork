#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ldb_fg_bootstrap {

struct Settings {
  bool enabled = true;
  bool hud = true;
  bool viewmodel = true;
  bool dynamicModels = true;
  bool fpbProtect = false; // Experimental stronger dynamic-pixel protection.
  bool maskDebugWhite = false; // Debug: final FG protection pixels become white.
  std::wstring dynamicResidualThreshold = L"auto";
  bool contentProbe = false;
  bool monitor = false;
  bool mergeSubmissions = true;
  bool autoCapture = false;
  bool hotkeys = true;
  uint32_t captureFrames = 4;
  uint32_t pacingMs = 0;
};

inline bool hasEnvironment(const wchar_t *name) noexcept {
  return GetEnvironmentVariableW(name, nullptr, 0) != 0;
}

inline void setEnvironment(const wchar_t *name, const std::wstring &value,
                           bool replace) noexcept {
  if (replace || !hasEnvironment(name))
    SetEnvironmentVariableW(name, value.c_str());
}

inline std::wstring environment(const wchar_t *name) {
  const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
  if (!length)
    return {};
  std::wstring value(length, L'\0');
  const DWORD used = GetEnvironmentVariableW(name, value.data(), length);
  if (!used || used >= length)
    return {};
  value.resize(used);
  return value;
}

inline void prependEnvironmentPath(const wchar_t *name,
                                   const std::filesystem::path &path) {
  const std::wstring prefix = path.wstring();
  std::wstring current = environment(name);
  if (current.find(prefix) != std::wstring::npos)
    return;
  SetEnvironmentVariableW(
      name, current.empty() ? prefix.c_str() : (prefix + L";" + current).c_str());
}

inline void removeEnvironmentToken(const wchar_t *name,
                                   const std::wstring &token) noexcept {
  std::wstring current = environment(name), result;
  size_t begin = 0;
  while (begin <= current.size()) {
    const size_t end = current.find(L';', begin);
    const std::wstring value = current.substr(
        begin, end == std::wstring::npos ? end : end - begin);
    if (!value.empty() && _wcsicmp(value.c_str(), token.c_str()) != 0) {
      if (!result.empty())
        result += L';';
      result += value;
    }
    if (end == std::wstring::npos)
      break;
    begin = end + 1;
  }
  SetEnvironmentVariableW(name, result.empty() ? nullptr : result.c_str());
}

inline bool iniBool(const std::filesystem::path &path, const wchar_t *key,
                    bool fallback) noexcept {
  return GetPrivateProfileIntW(L"frame_generation", key, fallback ? 1 : 0,
                               path.c_str()) != 0;
}

inline uint32_t iniUint(const std::filesystem::path &path, const wchar_t *key,
                        uint32_t fallback, uint32_t maximum) noexcept {
  const UINT value = GetPrivateProfileIntW(L"frame_generation", key, fallback,
                                           path.c_str());
  return std::min<uint32_t>(value, maximum);
}

inline std::wstring iniText(const std::filesystem::path &path,
                            const wchar_t *key, const wchar_t *fallback) {
  wchar_t buffer[64]{};
  const DWORD count = GetPrivateProfileStringW(
      L"frame_generation", key, fallback, buffer, 64, path.c_str());
  return count ? std::wstring(buffer, count) : std::wstring(fallback);
}

inline Settings readSettings(const std::filesystem::path &path) noexcept {
  Settings result;
  result.enabled = iniBool(path, L"enabled", result.enabled);
  result.hud = iniBool(path, L"hud", result.hud);
  result.viewmodel = iniBool(path, L"viewmodel", result.viewmodel);
  result.dynamicModels =
      iniBool(path, L"dynamic_models", result.dynamicModels);
  result.fpbProtect = iniBool(path, L"fpb_protect", result.fpbProtect);
  result.dynamicResidualThreshold =
      iniText(path, L"dynamic_residual_threshold", L"auto");
  result.maskDebugWhite = iniBool(path, L"mask_debug_white", result.maskDebugWhite);
  result.contentProbe =
      iniBool(path, L"content_probe", result.contentProbe);
  result.monitor = iniBool(path, L"monitor", result.monitor);
  result.mergeSubmissions =
      iniBool(path, L"merge_submissions", result.mergeSubmissions);
  result.autoCapture = iniBool(path, L"auto_capture", result.autoCapture);
  result.hotkeys = iniBool(path, L"hotkeys", result.hotkeys);
  result.captureFrames =
      iniUint(path, L"capture_frames", result.captureFrames, 16);
  result.pacingMs = iniUint(path, L"pacing_ms", result.pacingMs, 8);
  return result;
}

inline std::filesystem::path executableDirectory() {
  std::vector<wchar_t> buffer(32768);
  const DWORD length =
      GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
  if (!length || length >= DWORD(buffer.size()))
    return {};
  return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

inline void launchMonitor(const std::filesystem::path &script,
                          const std::filesystem::path &directory) noexcept {
  try {
    if (!std::filesystem::is_regular_file(script))
      return;
    std::wstring command =
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" +
        script.wstring() + L"\" -LogDirectory \"" + directory.wstring() +
        L"\" -HostProcessId " + std::to_wstring(GetCurrentProcessId());
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                       CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup,
                       &process)) {
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
    }
  } catch (...) {
  }
}

struct Runtime {
  Settings settings{};
  std::filesystem::path bridgeDirectory;
  std::filesystem::path monitorPath;
  bool scriptMode = false;
  bool replace = false;
  bool initialized = false;
  bool logsAttached = false;
};

inline Runtime &runtime() noexcept {
  static Runtime value;
  return value;
}

inline void clearManagedEnvironment() noexcept {
  static const wchar_t *names[] = {
      L"LDB_FG_BACKEND",          L"LDB_FG_PRESENTER",
      L"LDB_FG_MODE",             L"LDB_FG_FAST_INPUTS",
      L"LDB_FG_HUD_BYPASS",       L"LDB_FG_VIEWMODEL",
      L"LDB_FG_DYNAMIC_MODELS",    L"LDB_FG_FPB_PROTECT",
      L"LDB_FG_MASK_DEBUG_WHITE",
      L"LDB_FG_DYNAMIC_RESIDUAL_THRESHOLD",
      L"LDB_FG_MERGE_SUBMISSIONS", L"LDB_FG_CAPTURE_FRAMES",
      L"LDB_FG_HOTKEYS",          L"LDB_FG_AUTO_CAPTURE",
      L"LDB_FG_PACING_MS",        L"LDB_FG_LOG_DIR",
      L"ASTERIA_FG_ESTIMATED_INTERNAL", L"ASTERIA_FG_ESTIMATED_DISPATCH",
      L"ASTERIA_FG_SEPARATE_UI",  L"ASTERIA_FG_CONTENT_PROBE",
      L"ASTERIA_FG_PROBE_OUTPUT", L"DXVK_LOG_PATH", L"DXVK_LOG_LEVEL"};
  for (const auto *name : names)
    SetEnvironmentVariableW(name, nullptr);
}

// L4D2Bridge64 starts before DXVK creates its Vulkan instance, so it is the
// reliable place to make the adjacent explicit Layer discoverable.  Ordinary
// launches read the INI.  The legacy diagnostic scripts set GF_FG_ISOLATION
// and retain authority over their inherited environment.
inline void initialize() noexcept {
  static const bool initialized = []() noexcept {
    try {
      auto &state = runtime();
      const auto bridgeDirectory = executableDirectory();
      if (bridgeDirectory.empty())
        return false;
      const auto configPath = bridgeDirectory / L"fg-config.ini";
      const auto layerDirectory = bridgeDirectory / L"vulkan-layer";
      const bool scriptMode = hasEnvironment(L"GF_FG_ISOLATION");
      const bool configExists = std::filesystem::is_regular_file(configPath);
      const Settings settings =
          configExists ? readSettings(configPath) : Settings{};
      const bool replace = configExists && !scriptMode;
      const auto flag = [](bool value) { return value ? L"1" : L"0"; };

      state.settings = settings;
      state.bridgeDirectory = bridgeDirectory;
      state.monitorPath = bridgeDirectory / L"fg_live_hud.ps1";
      state.scriptMode = scriptMode;
      state.replace = replace;

      setEnvironment(L"LDB_FG_ENABLE", flag(settings.enabled), replace);
      const bool enabled = environment(L"LDB_FG_ENABLE") == L"1";
      state.settings.enabled = enabled;
      setEnvironment(L"ASTERIA_FG_ENABLE", flag(enabled), replace);
      if (!enabled) {
        // An INI disable is authoritative.  Remove variables inherited from a
        // previous FG launch so this process remains equivalent to plain LDB.
        if (replace) {
          clearManagedEnvironment();
          removeEnvironmentToken(L"VK_ADD_LAYER_PATH",
                                 layerDirectory.wstring());
          removeEnvironmentToken(L"VK_LAYER_PATH", layerDirectory.wstring());
          removeEnvironmentToken(L"VK_INSTANCE_LAYERS",
                                 L"VK_LAYER_GF_Asteria_FrameGeneration");
        }
        state.initialized = true;
        return true;
      }

      setEnvironment(L"LDB_FG_BACKEND", L"custom", replace);
      setEnvironment(L"LDB_FG_MODE", L"provider", replace);
      setEnvironment(L"LDB_FG_FAST_INPUTS", L"1", replace);
      setEnvironment(L"LDB_FG_HUD_BYPASS", flag(settings.hud), replace);
      setEnvironment(L"LDB_FG_VIEWMODEL", flag(settings.viewmodel), replace);
      setEnvironment(L"LDB_FG_DYNAMIC_MODELS", flag(settings.dynamicModels),
                     replace);
      setEnvironment(L"LDB_FG_FPB_PROTECT", flag(settings.fpbProtect), replace);
      setEnvironment(L"LDB_FG_DYNAMIC_RESIDUAL_THRESHOLD",
                     settings.dynamicResidualThreshold, replace);
      setEnvironment(L"LDB_FG_MASK_DEBUG_WHITE", flag(settings.maskDebugWhite),
                     replace);
      setEnvironment(L"LDB_FG_MERGE_SUBMISSIONS",
                     flag(settings.mergeSubmissions), replace);
      setEnvironment(L"LDB_FG_CAPTURE_FRAMES",
                     std::to_wstring(settings.captureFrames), replace);
      setEnvironment(L"LDB_FG_HOTKEYS", flag(settings.hotkeys), replace);
      setEnvironment(L"LDB_FG_AUTO_CAPTURE", flag(settings.autoCapture),
                     replace);
      setEnvironment(L"LDB_FG_PACING_MS", std::to_wstring(settings.pacingMs),
                     replace);
      setEnvironment(L"ASTERIA_FG_ESTIMATED_INTERNAL", L"1", replace);
      setEnvironment(L"ASTERIA_FG_ESTIMATED_DISPATCH", L"0", replace);
      setEnvironment(L"ASTERIA_FG_SEPARATE_UI",
                     flag(settings.hud || settings.viewmodel), replace);
      setEnvironment(L"ASTERIA_FG_CONTENT_PROBE",
                     flag(settings.contentProbe), replace);

      if (std::filesystem::is_directory(layerDirectory)) {
        prependEnvironmentPath(L"VK_ADD_LAYER_PATH", layerDirectory);
        const std::wstring layer = L"VK_LAYER_GF_Asteria_FrameGeneration";
        std::wstring layers = environment(L"VK_INSTANCE_LAYERS");
        if (layers.find(layer) == std::wstring::npos)
          SetEnvironmentVariableW(
              L"VK_INSTANCE_LAYERS",
              layers.empty() ? layer.c_str() : (layer + L";" + layers).c_str());
      }

      state.initialized = true;
      return true;
    } catch (...) {
      return false;
    }
  }();
  (void)initialized;
}

// Bind FG output only after LDB v1.3.4 has created its GUID log session.  The
// optional script mode keeps the explicitly supplied isolation directory.
inline void attachLogSession(const std::filesystem::path &ldbSession) noexcept {
  initialize();
  auto &state = runtime();
  if (!state.initialized || !state.settings.enabled || state.logsAttached)
    return;
  try {
    std::filesystem::path directory;
    const auto inherited = environment(L"LDB_FG_LOG_DIR");
    if (state.scriptMode && !inherited.empty())
      directory = inherited;
    else
      directory = ldbSession / L"frame-generation";
    if (directory.empty() || !directory.is_absolute())
      return;
    std::filesystem::create_directories(directory);
    SetEnvironmentVariableW(L"LDB_FG_LOG_DIR", directory.c_str());
    SetEnvironmentVariableW(L"ASTERIA_FG_PROBE_OUTPUT",
                            (directory / L"asteria-layer.json").c_str());
    SetEnvironmentVariableW(L"DXVK_LOG_PATH", directory.c_str());
    setEnvironment(L"DXVK_LOG_LEVEL", L"info", state.replace);

    std::ofstream diagnostic(directory / L"fg-bootstrap.txt",
                             std::ios::binary);
    if (diagnostic)
      diagnostic << "enabled=1\n"
                 << "hud=" << state.settings.hud << "\n"
                 << "viewmodel=" << state.settings.viewmodel << "\n"
                 << "dynamic_models=" << state.settings.dynamicModels << "\n"
                 << "fpb_protect=" << state.settings.fpbProtect << "\n"
                 << "dynamic_residual_threshold="
                 << std::string(state.settings.dynamicResidualThreshold.begin(),
                                state.settings.dynamicResidualThreshold.end())
                 << "\n"
                 << "mask_debug_white=" << state.settings.maskDebugWhite << "\n"
                 << "content_probe=" << state.settings.contentProbe << "\n"
                 << "monitor=" << state.settings.monitor << "\n"
                 << "script_mode=" << state.scriptMode << "\n"
                 << "ldb_session=" << ldbSession.string() << "\n";
    if (state.settings.monitor && !state.scriptMode)
      launchMonitor(state.monitorPath, directory);
    state.logsAttached = true;
  } catch (...) {
  }
}

} // namespace ldb_fg_bootstrap
