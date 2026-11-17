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
  bool contentProbe = false;
  bool monitor = true;
  bool mergeSubmissions = true;
  bool autoCapture = true;
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

inline Settings readSettings(const std::filesystem::path &path) noexcept {
  Settings result;
  result.enabled = iniBool(path, L"enabled", result.enabled);
  result.hud = iniBool(path, L"hud", result.hud);
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

inline std::filesystem::path createSessionDirectory(
    const std::filesystem::path &gameRoot) {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  wchar_t name[128]{};
  swprintf_s(name, L"session-%04u%02u%02u-%02u%02u%02u-%lu", time.wYear,
             time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
             GetCurrentProcessId());
  auto result = gameRoot / L"fg-results" / name;
  std::filesystem::create_directories(result);
  return result;
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

// L4D2Bridge64 starts before DXVK creates its Vulkan instance, so it is the
// reliable place to make the adjacent explicit Layer discoverable.  Ordinary
// launches read the INI.  The legacy diagnostic scripts set GF_FG_ISOLATION
// and retain authority over their inherited environment.
inline void initialize() noexcept {
  static const bool initialized = []() noexcept {
    try {
      const auto bridgeDirectory = executableDirectory();
      if (bridgeDirectory.empty())
        return false;
      const auto gameRoot = bridgeDirectory.parent_path().parent_path();
      const auto configPath = bridgeDirectory / L"fg-config.ini";
      const auto layerDirectory = bridgeDirectory / L"vulkan-layer";
      const auto monitorPath = bridgeDirectory / L"fg_live_hud.ps1";
      const bool scriptMode = hasEnvironment(L"GF_FG_ISOLATION");
      const bool configExists = std::filesystem::is_regular_file(configPath);
      const Settings settings =
          configExists ? readSettings(configPath) : Settings{};
      const bool replace = configExists && !scriptMode;
      const auto flag = [](bool value) { return value ? L"1" : L"0"; };

      setEnvironment(L"LDB_FG_ENABLE", flag(settings.enabled), replace);
      setEnvironment(L"LDB_FG_BACKEND", L"custom", replace);
      setEnvironment(L"LDB_FG_PRESENTER", L"1", replace);
      setEnvironment(L"LDB_FG_MODE", L"provider", replace);
      setEnvironment(L"LDB_FG_FAST_INPUTS", L"1", replace);
      setEnvironment(L"LDB_FG_HUD_BYPASS", flag(settings.hud), replace);
      setEnvironment(L"LDB_FG_MERGE_SUBMISSIONS",
                     flag(settings.mergeSubmissions), replace);
      setEnvironment(L"LDB_FG_CAPTURE_FRAMES",
                     std::to_wstring(settings.captureFrames), replace);
      setEnvironment(L"LDB_FG_HOTKEYS", flag(settings.hotkeys), replace);
      setEnvironment(L"LDB_FG_AUTO_CAPTURE", flag(settings.autoCapture),
                     replace);
      setEnvironment(L"LDB_FG_PACING_MS", std::to_wstring(settings.pacingMs),
                     replace);
      setEnvironment(L"ASTERIA_FG_ENABLE", flag(settings.enabled), replace);
      setEnvironment(L"ASTERIA_FG_ESTIMATED_INTERNAL", L"1", replace);
      setEnvironment(L"ASTERIA_FG_ESTIMATED_DISPATCH", L"0", replace);
      setEnvironment(L"ASTERIA_FG_SEPARATE_UI", flag(settings.hud), replace);
      setEnvironment(L"ASTERIA_FG_CONTENT_PROBE",
                     flag(settings.contentProbe), replace);

      if (settings.enabled && std::filesystem::is_directory(layerDirectory)) {
        prependEnvironmentPath(L"VK_ADD_LAYER_PATH", layerDirectory);
        const std::wstring layer = L"VK_LAYER_GF_Asteria_FrameGeneration";
        std::wstring layers = environment(L"VK_INSTANCE_LAYERS");
        if (layers.find(layer) == std::wstring::npos)
          SetEnvironmentVariableW(
              L"VK_INSTANCE_LAYERS",
              layers.empty() ? layer.c_str() : (layer + L";" + layers).c_str());
      }

      std::filesystem::path session;
      const auto inheritedLog = environment(L"LDB_FG_LOG_DIR");
      if (!inheritedLog.empty() && scriptMode) {
        session = inheritedLog;
        std::filesystem::create_directories(session);
      } else {
        session = createSessionDirectory(gameRoot);
        SetEnvironmentVariableW(L"LDB_FG_LOG_DIR", session.c_str());
      }
      SetEnvironmentVariableW(L"ASTERIA_FG_PROBE_OUTPUT",
                              (session / L"asteria-layer.json").c_str());
      SetEnvironmentVariableW(L"DXVK_LOG_PATH", session.c_str());
      setEnvironment(L"DXVK_LOG_LEVEL", L"info", replace);

      std::ofstream diagnostic(session / L"fg-bootstrap.txt",
                               std::ios::binary);
      if (diagnostic)
        diagnostic << "enabled=" << settings.enabled << "\n"
                   << "hud=" << settings.hud << "\n"
                   << "content_probe=" << settings.contentProbe << "\n"
                   << "monitor=" << settings.monitor << "\n"
                   << "script_mode=" << scriptMode << "\n";
      if (settings.enabled && settings.monitor && !scriptMode)
        launchMonitor(monitorPath, session);
      return true;
    } catch (...) {
      return false;
    }
  }();
  (void)initialized;
}

} // namespace ldb_fg_bootstrap
