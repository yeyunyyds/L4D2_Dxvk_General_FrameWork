#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ldb_rt {

constexpr uint32_t kMagic = 0x5452444c;
constexpr uint32_t kVersion = 1;
constexpr uint32_t kHeaderBytes = 104;
constexpr uint32_t kMaxPacketBytes = 1024 * 1024;
constexpr uint32_t kMaxBindings = 32;

enum class Kind : uint32_t {
  Hello = 1, BeginFrame, BeginView, UpdateResource, Draw,
  EndView, EndFrame, Reset, RetireResource, ChangeMap
};
enum class ViewKind : uint32_t { Main = 1, Sky, Weapon, Hud, Other };
enum class Error {
  Ok, Wire, Version, Identity, Sequence, Order, Camera,
  Resource, Limit, Thread, MissingDraw
};
inline const char* errorName(Error error) {
  static const char* names[] = {"ok", "wire", "version", "identity", "sequence",
    "order", "camera", "resource", "limit", "thread", "missing_draw"};
  return names[static_cast<unsigned>(error)];
}

struct Header {
  Kind kind = Kind::Hello;
  uint64_t session = 0, run = 0, device = 0, generation = 1, map = 1;
  uint64_t frame = 0, view = 0, sequence = 0;
  uint32_t producerThread = 0;
  uint64_t producerTicks = 0;
};
using Bytes = std::vector<uint8_t>;

struct Writer {
  Bytes data;
  void u32(uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) { data.push_back(uint8_t(value >> (8 * n))); }
  }
  void u64(uint64_t value) { u32(uint32_t(value)); u32(uint32_t(value >> 32)); }
  void f32(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); u32(bits); }
};
struct Reader {
  const uint8_t* data;
  size_t size, offset = 0;
  uint32_t u32() {
    if (offset > size || size - offset < 4) { throw std::out_of_range("rt wire"); }
    uint32_t value = 0;
    for (unsigned n = 0; n < 4; ++n) { value |= uint32_t(data[offset++]) << (8 * n); }
    return value;
  }
  uint64_t u64() { const uint64_t lo = u32(); return lo | (uint64_t(u32()) << 32); }
  float f32() { const auto bits = u32(); float value; std::memcpy(&value, &bits, 4); return value; }
  bool end() const { return offset == size; }
};

struct Packet { Header header; Bytes payload; };
inline Bytes encode(const Header& h, const Bytes& payload = {}) {
  if (payload.size() > kMaxPacketBytes - kHeaderBytes) { throw std::length_error("rt packet"); }
  Writer w;
  w.u32(kMagic); w.u32(kVersion); w.u32(kHeaderBytes);
  w.u32(uint32_t(kHeaderBytes + payload.size()));
  w.u32(static_cast<uint32_t>(h.kind)); w.u32(0);
  w.u64(h.session); w.u64(h.run); w.u64(h.device); w.u64(h.generation); w.u64(h.map);
  w.u64(h.frame); w.u64(h.view); w.u64(h.sequence);
  w.u32(h.producerThread); w.u32(0); w.u64(h.producerTicks);
  w.data.insert(w.data.end(), payload.begin(), payload.end());
  return w.data;
}
inline Error decode(const void* data, size_t size, Packet& output) {
  if (!data || size < kHeaderBytes || size > kMaxPacketBytes) { return Error::Wire; }
  Reader r{static_cast<const uint8_t*>(data), size};
  if (r.u32() != kMagic) { return Error::Wire; }
  if (r.u32() != kVersion) { return Error::Version; }
  if (r.u32() != kHeaderBytes || r.u32() != size) { return Error::Wire; }
  const auto kind = r.u32();
  if (kind < 1 || kind > 10 || r.u32()) { return Error::Wire; }
  Header h;
  h.kind = static_cast<Kind>(kind);
  h.session = r.u64(); h.run = r.u64(); h.device = r.u64();
  h.generation = r.u64(); h.map = r.u64(); h.frame = r.u64();
  h.view = r.u64(); h.sequence = r.u64(); h.producerThread = r.u32();
  if (r.u32()) { return Error::Wire; }
  h.producerTicks = r.u64();
  Packet next{h, Bytes(r.data + r.offset, r.data + size)};
  output = std::move(next);
  return Error::Ok;
}

struct Camera {
  ViewKind kind = ViewKind::Main;
  uint32_t width = 0, height = 0;
  std::array<float, 16> view{}, projection{};
  float nearPlane = 0, farPlane = 0;
};
inline Bytes cameraPayload(const Camera& camera) {
  Writer w;
  w.u32(static_cast<uint32_t>(camera.kind)); w.u32(camera.width); w.u32(camera.height); w.u32(0);
  for (float v : camera.view) { w.f32(v); }
  for (float v : camera.projection) { w.f32(v); }
  w.f32(camera.nearPlane); w.f32(camera.farPlane);
  return w.data;
}
inline bool invertible(const std::array<float, 16>& matrix) {
  double a[4][4];
  for (unsigned i = 0; i < 16; ++i) {
    if (!std::isfinite(matrix[i])) { return false; }
    a[i / 4][i % 4] = matrix[i];
  }
  for (unsigned c = 0; c < 4; ++c) {
    unsigned pivot = c;
    for (unsigned r = c + 1; r < 4; ++r) {
      if (std::abs(a[r][c]) > std::abs(a[pivot][c])) { pivot = r; }
    }
    if (std::abs(a[pivot][c]) < 1e-20) { return false; }
    for (unsigned k = 0; k < 4; ++k) { std::swap(a[c][k], a[pivot][k]); }
    for (unsigned r = c + 1; r < 4; ++r) {
      const double ratio = a[r][c] / a[c][c];
      for (unsigned k = c; k < 4; ++k) { a[r][k] -= ratio * a[c][k]; }
    }
  }
  return true;
}

struct Budget { std::atomic<size_t> bytes{0}; size_t limit; explicit Budget(size_t n) : limit(n) {} };
struct Resource {
  uint64_t id, version;
  uint32_t kind;
  Bytes data;
  std::shared_ptr<Budget> budget;
  Resource(uint64_t i, uint64_t v, uint32_t k, Bytes b, std::shared_ptr<Budget> owner)
      : id(i), version(v), kind(k), data(std::move(b)), budget(std::move(owner)) {
    size_t used = budget->bytes.load();
    do {
      if (data.size() > budget->limit || used > budget->limit - data.size()) {
        throw std::length_error("rt resource budget");
      }
    } while (!budget->bytes.compare_exchange_weak(used, used + data.size()));
  }
  ~Resource() { budget->bytes.fetch_sub(data.size()); }
  Resource(const Resource&) = delete;
  Resource& operator=(const Resource&) = delete;
};
using ResourceRef = std::shared_ptr<const Resource>;
struct DrawInfo {
  uint64_t id = 0, instance = 0;
  uint32_t categories = 0, modifiers = 0;
  std::array<float, 12> transform{};
  std::vector<std::pair<uint64_t, uint64_t>> bindings;
};
inline Bytes drawPayload(const DrawInfo& draw) {
  Writer w;
  w.u64(draw.id); w.u64(draw.instance); w.u32(uint32_t(draw.bindings.size()));
  w.u32(draw.categories); w.u32(draw.modifiers); w.u32(0);
  for (float v : draw.transform) { w.f32(v); }
  for (auto binding : draw.bindings) { w.u64(binding.first); w.u64(binding.second); }
  return w.data;
}
inline Bytes resourcePayload(uint64_t id, uint64_t version, uint32_t kind, const Bytes& bytes) {
  Writer w; w.u64(id); w.u64(version); w.u32(kind); w.u32(uint32_t(bytes.size()));
  w.data.insert(w.data.end(), bytes.begin(), bytes.end()); return w.data;
}
inline Bytes retirePayload(uint64_t id, uint64_t version) {
  Writer w; w.u64(id); w.u64(version); return w.data;
}
struct DrawSnapshot {
  Header header;
  Camera camera;
  DrawInfo draw;
  std::vector<ResourceRef> resources;
  uint64_t deviceCommand = 0;
};

// One consumer owns Stream. DrawSnapshot can be retained by asynchronous readers.
class Stream {
  struct View { uint64_t id; Camera camera; };
  Header m_identity{};
  uint64_t m_sequence = 0, m_frame = 0, m_draw = 0, m_consumer = 0;
  bool m_ready = false, m_inFrame = false, m_endedFrame = false, m_mainSeen = false;
  std::vector<View> m_views;
  std::set<uint64_t> m_viewIds;
  std::map<uint64_t, ResourceRef> m_resources;
  std::map<uint64_t, uint64_t> m_versions;
  std::unique_ptr<DrawSnapshot> m_pending;
  std::shared_ptr<Budget> m_budget;

  bool frameIdentity(const Header& h) const { return m_inFrame && h.frame == m_frame; }
  bool viewIdentity(const Header& h) const {
    return frameIdentity(h) && !m_views.empty() && m_views.back().id == h.view;
  }
public:
  explicit Stream(size_t budget = 16 * 1024 * 1024) : m_budget(std::make_shared<Budget>(budget)) {}
  size_t retainedBytes() const { return m_budget->bytes.load(); }
  bool ready() const { return m_ready; }
  bool inFrame() const { return m_inFrame; }
  bool awaitingPresent() const { return m_endedFrame; }
  uint64_t sequence() const { return m_sequence; }

  Error apply(const void* bytes, size_t size, uint64_t consumerThread, bool physicalResetBarrier = false) {
    Packet packet;
    const Error wire = decode(bytes, size, packet);
    if (wire != Error::Ok) { return wire; }
    const Header& h = packet.header;
    if (!consumerThread || !h.session || !h.run || !h.device || !h.generation || !h.map || !h.producerThread) {
      return Error::Identity;
    }
    if (m_consumer && m_consumer != consumerThread) { return Error::Thread; }
    if (h.kind == Kind::Hello) {
      if (m_ready || h.sequence != 1 || h.frame || h.view || !packet.payload.empty()) { return Error::Order; }
      m_identity = h; m_consumer = consumerThread; m_ready = true; m_sequence = 1;
      return Error::Ok;
    }
    if (!m_ready || h.session != m_identity.session || h.run != m_identity.run || h.device != m_identity.device) {
      return Error::Identity;
    }
    const bool reset = h.kind == Kind::Reset;
    if (m_sequence == UINT64_MAX ||
        (reset && physicalResetBarrier ? h.sequence <= m_sequence : h.sequence != m_sequence + 1)) {
      return Error::Sequence;
    }
    const bool mapChange = h.kind == Kind::ChangeMap;
    if (reset) {
      if (m_identity.generation == UINT64_MAX || h.generation != m_identity.generation + 1 || h.map != m_identity.map) {
        return Error::Identity;
      }
    } else if (mapChange) {
      if (m_identity.map == UINT64_MAX || h.map != m_identity.map + 1 || h.generation != m_identity.generation) {
        return Error::Identity;
      }
    } else if (h.generation != m_identity.generation || h.map != m_identity.map) { return Error::Identity; }

    Reader r{packet.payload.data(), packet.payload.size()};
    try {
      if (reset || mapChange) {
        if (!r.end() || h.frame || h.view) { return Error::Order; }
        m_views.clear(); m_viewIds.clear(); m_resources.clear(); m_versions.clear(); m_pending.reset();
        m_inFrame = false; m_endedFrame = false; m_mainSeen = false; m_identity = h;
      } else if (h.kind == Kind::BeginFrame) {
        if (!r.end() || m_inFrame || m_endedFrame || h.view || m_frame == UINT64_MAX || h.frame != m_frame + 1) {
          return Error::Order;
        }
        m_frame = h.frame; m_draw = 0; m_mainSeen = false; m_inFrame = true; m_viewIds.clear();
      } else if (h.kind == Kind::BeginView) {
        if (!frameIdentity(h) || !h.view || m_pending || m_views.size() >= 16 || m_viewIds.size() >= 256 || m_viewIds.count(h.view)) {
          return Error::Order;
        }
        Camera c;
        const auto kind = r.u32(); c.kind = static_cast<ViewKind>(kind);
        c.width = r.u32(); c.height = r.u32();
        if (r.u32()) { return Error::Wire; }
        for (float& v : c.view) { v = r.f32(); }
        for (float& v : c.projection) { v = r.f32(); }
        c.nearPlane = r.f32(); c.farPlane = r.f32();
        if (!r.end()) { return Error::Wire; }
        if (kind < 1 || kind > 5 || !c.width || !c.height || c.width > 32768 || c.height > 32768 ||
            !std::isfinite(c.nearPlane) || !std::isfinite(c.farPlane) || c.nearPlane <= 0 ||
            c.farPlane <= c.nearPlane || !invertible(c.view) || !invertible(c.projection)) { return Error::Camera; }
        if (c.kind == ViewKind::Main && (m_mainSeen || !m_views.empty())) { return Error::Order; }
        m_viewIds.insert(h.view); m_views.push_back({h.view, c});
        if (c.kind == ViewKind::Main) { m_mainSeen = true; }
      } else if (h.kind == Kind::UpdateResource) {
        if (h.frame || h.view || m_pending) { return Error::Order; }
        const auto id = r.u64(), version = r.u64();
        const auto kind = r.u32(), length = r.u32();
        if (!id || !version || kind < 1 || kind > 4 || !length || length != r.size - r.offset) { return Error::Resource; }
        const auto previous = m_versions.find(id);
        if (previous == m_versions.end()) {
          if (version != 1 || m_versions.size() >= 4096) { return Error::Resource; }
        } else if (previous->second == UINT64_MAX || version != previous->second + 1) { return Error::Resource; }
        auto value = std::make_shared<Resource>(id, version, kind, Bytes(r.data + r.offset, r.data + r.size), m_budget);
        m_resources[id] = std::move(value); m_versions[id] = version;
      } else if (h.kind == Kind::RetireResource) {
        if (h.frame || h.view || m_pending) { return Error::Order; }
        const auto id = r.u64(), version = r.u64();
        const auto it = m_resources.find(id);
        if (!r.end() || it == m_resources.end() || it->second->version != version) { return Error::Resource; }
        m_resources.erase(it);
      } else if (h.kind == Kind::Draw) {
        if (!viewIdentity(h) || m_pending) { return Error::Order; }
        auto draw = std::make_unique<DrawSnapshot>();
        draw->header = h; draw->camera = m_views.back().camera;
        auto& d = draw->draw;
        d.id = r.u64(); d.instance = r.u64();
        const auto count = r.u32(); d.categories = r.u32(); d.modifiers = r.u32();
        if (r.u32() || count > kMaxBindings) { return Error::Wire; }
        for (float& v : d.transform) { v = r.f32(); if (!std::isfinite(v)) { return Error::Wire; } }
        if (!d.instance || m_draw == UINT64_MAX || d.id != m_draw + 1) { return Error::Order; }
        for (uint32_t n = 0; n < count; ++n) {
          const auto id = r.u64(), version = r.u64();
          const auto it = m_resources.find(id);
          if (it == m_resources.end() || it->second->version != version) { return Error::Resource; }
          for (auto existing : d.bindings) { if (existing.first == id) { return Error::Resource; } }
          d.bindings.emplace_back(id, version); draw->resources.push_back(it->second);
        }
        if (!r.end()) { return Error::Wire; }
        m_draw = d.id; m_pending = std::move(draw);
      } else if (h.kind == Kind::EndView) {
        if (!r.end() || !viewIdentity(h) || m_pending) { return Error::Order; }
        m_views.pop_back();
      } else if (h.kind == Kind::EndFrame) {
        if (!r.end() || !frameIdentity(h) || h.view || !m_views.empty() || m_pending || !m_mainSeen) { return Error::Order; }
        m_inFrame = false; m_endedFrame = true;
      } else { return Error::Wire; }
    } catch (const std::out_of_range&) { return Error::Wire; }
      catch (const std::length_error&) { return Error::Limit; }
    m_sequence = h.sequence;
    return Error::Ok;
  }

  Error draw(uint64_t deviceCommand, uint64_t consumerThread, std::unique_ptr<DrawSnapshot>& output) {
    if (consumerThread != m_consumer) { return Error::Thread; }
    if (!m_pending) { return Error::MissingDraw; }
    m_pending->deviceCommand = deviceCommand; output = std::move(m_pending);
    return Error::Ok;
  }
  Error present(uint64_t consumerThread, uint64_t& frame) {
    if (consumerThread != m_consumer) { return Error::Thread; }
    if (!m_endedFrame || m_inFrame || m_pending || !m_views.empty()) { return Error::Order; }
    frame = m_frame; m_endedFrame = false; return Error::Ok;
  }
};

} // namespace ldb_rt
