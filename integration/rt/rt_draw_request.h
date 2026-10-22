#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdint>

namespace ldb_rt::draw_observe {
namespace fs = std::filesystem;
struct Request { uint64_t session = 0; fs::path output, runtime; bool world = false, beta = false, performance = false; };
inline fs::path requestFile() {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&requestFile), &module)) { return {}; }
  wchar_t path[32768]{};
  const auto size = GetModuleFileNameW(module, path, 32768);
  if (!size || size >= 32768) { return {}; }
  auto directory = fs::path(path).parent_path();
  if (directory.filename() == L".l4d2bridge") { directory = directory.parent_path(); }
  return directory / "ldb-rt-draw.request";
}
inline bool readRequest(Request& output) noexcept {
  try {
    const auto file = requestFile();
    if (file.empty() || !fs::is_regular_file(file) || fs::file_size(file) > 32768) { return false; }
    std::ifstream input(file, std::ios::binary);
    std::string magic, session, directory, runtime, extra;
    if (!std::getline(input, magic) || (magic != "LDB_RT_DRAW_1" && magic != "LDB_RT_WORLD_1" && magic != "LDB_RT_BETA_1" && magic != "LDB_RT_PERF_1") || !std::getline(input, session) ||
        !std::getline(input, directory)) { return false; }
    const bool world = magic == "LDB_RT_WORLD_1" || magic == "LDB_RT_BETA_1" || magic == "LDB_RT_PERF_1";
    if ((world && !std::getline(input, runtime)) || std::getline(input, extra)) { return false; }
    if (session.empty() || session.size() > 16 || directory.empty() || directory.find('\0') != std::string::npos) { return false; }
    for (char c : session) { if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; } }
    Request value; value.session = std::stoull(session, nullptr, 16); value.output = fs::u8path(directory); value.world = world;
    value.beta = magic == "LDB_RT_BETA_1";
    value.performance = magic == "LDB_RT_PERF_1";
    if (world) {
      if (runtime.empty() || runtime.find('\0') != std::string::npos) { return false; }
      value.runtime = fs::u8path(runtime);
      if (!value.runtime.is_absolute() || !fs::is_regular_file(value.runtime / "d3d9.dll")) { return false; }
    }
    if (!value.session || !value.output.is_absolute() || !fs::is_directory(value.output)) { return false; }
    output = std::move(value); return true;
  } catch (...) { return false; }
}
inline fs::path doneFile(const Request& r) { return r.output / ("rt-draw-done-" + std::to_string(r.session) + ".json"); }
inline fs::path previewRunningFile(const Request& r) { return r.output/("rt-preview-running-"+std::to_string(r.session)+".json"); }
inline fs::path previewDoneFile(const Request& r) { return r.output/("rt-preview-done-"+std::to_string(r.session)+".json"); }
} // namespace ldb_rt::draw_observe
