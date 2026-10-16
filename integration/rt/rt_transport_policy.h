#pragma once
#include <atomic>
#include <cstdint>

namespace ldb_rt::transport_wait {
// Selected from a validated launch request before constructing IPC channels.
// RT pipeline compilation can block the consumer much longer than a normal
// frame. Keep a finite bound and the transport's existing peer/fault checks.
constexpr uint32_t kNormalMs = 2000, kWorldMs = 60000;
inline std::atomic<uint32_t> processBudget{kNormalMs};
inline void configure(bool world) noexcept {
  processBudget.store(world ? kWorldMs : kNormalMs, std::memory_order_release);
}
inline uint32_t producerMs() noexcept { return processBudget.load(std::memory_order_acquire); }
} // namespace ldb_rt::transport_wait
