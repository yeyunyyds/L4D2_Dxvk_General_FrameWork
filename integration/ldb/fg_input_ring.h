#pragma once

#include "fsr_presenter_api.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ldb_fg {

inline constexpr size_t kInputRingSize = 3;
inline constexpr size_t kInvalidSlot = std::numeric_limits<size_t>::max();

enum class InputSlotState : uint8_t { Free, Writing, Ready, InFlight };
enum class PublishResult : uint8_t { Published, Full, Invalid, OutOfOrder };

struct InputLease {
  size_t slot = kInvalidSlot;
  LdbFgFrameInputsV1 input{};
  uint64_t readyValue = 0;
  uint64_t retireValue = 0;

  explicit operator bool() const noexcept { return slot != kInvalidSlot; }
};

struct InputRingStatistics {
  uint64_t published = 0;
  uint64_t acquired = 0;
  uint64_t completed = 0;
  uint64_t droppedFull = 0;
  uint64_t droppedStale = 0;
  uint64_t rejected = 0;
};

// A bounded single-producer/single-consumer ownership ring. The producer owns
// the ordering fields and publishes a complete input with release semantics.
// The consumer may keep several slots in flight, but a slot cannot be reused
// until the GPU completion timeline reaches its retire value.
class InputRing {
  struct Slot {
    std::atomic<InputSlotState> state{InputSlotState::Free};
    LdbFgFrameInputsV1 input{};
    uint64_t readyValue = 0;
    uint64_t retireValue = 0;
  };

  std::array<Slot, kInputRingSize> m_slots{};
  uint64_t m_lastGeneration = 0; // producer only
  uint64_t m_lastFrame = 0;      // producer only
  std::atomic<uint64_t> m_published{0};
  std::atomic<uint64_t> m_acquired{0};
  std::atomic<uint64_t> m_completed{0};
  std::atomic<uint64_t> m_droppedFull{0};
  std::atomic<uint64_t> m_droppedStale{0};
  std::atomic<uint64_t> m_rejected{0};

  void retireReadyBefore(uint64_t generation, uint64_t frame) noexcept {
    for (auto &slot : m_slots) {
      auto state = slot.state.load(std::memory_order_acquire);
      if (state != InputSlotState::Ready)
        continue;
      const auto &ticket = slot.input.ticket;
      if (ticket.generation > generation ||
          (ticket.generation == generation && ticket.frameId >= frame))
        continue;
      if (slot.state.compare_exchange_strong(
              state, InputSlotState::Free, std::memory_order_acq_rel,
              std::memory_order_acquire))
        m_droppedStale.fetch_add(1, std::memory_order_relaxed);
    }
  }

public:
  PublishResult publish(const LdbFgFrameInputsV1 &input) noexcept {
    const auto generation = input.ticket.generation;
    const auto frame = input.ticket.frameId;
    if (!LdbFgValidateFrameInputsV1(input) || !frame) {
      m_rejected.fetch_add(1, std::memory_order_relaxed);
      return PublishResult::Invalid;
    }
    if (generation < m_lastGeneration ||
        (generation == m_lastGeneration && frame <= m_lastFrame) ||
        (m_lastGeneration && generation > m_lastGeneration &&
         !(input.flags & LDB_FG_INPUT_RESET))) {
      m_rejected.fetch_add(1, std::memory_order_relaxed);
      return PublishResult::OutOfOrder;
    }

    Slot *target = nullptr;
    for (auto &slot : m_slots) {
      auto expected = InputSlotState::Free;
      if (slot.state.compare_exchange_strong(
              expected, InputSlotState::Writing, std::memory_order_acquire,
              std::memory_order_relaxed)) {
        target = &slot;
        break;
      }
    }
    if (!target) {
      m_droppedFull.fetch_add(1, std::memory_order_relaxed);
      return PublishResult::Full;
    }

    target->input = input;
    target->readyValue = input.readyValue;
    target->retireValue = 0;
    target->state.store(InputSlotState::Ready, std::memory_order_release);
    m_lastGeneration = generation;
    m_lastFrame = frame;
    m_published.fetch_add(1, std::memory_order_relaxed);
    return PublishResult::Published;
  }

  // Acquires the newest ready input and retires any older ready inputs. The
  // supplied value must be the signal value of the final GPU use of this slot.
  InputLease acquireLatest(uint64_t retireValue) noexcept {
    if (!retireValue) {
      m_rejected.fetch_add(1, std::memory_order_relaxed);
      return {};
    }
    size_t selected = kInvalidSlot;
    uint64_t generation = 0, frame = 0;
    for (size_t index = 0; index < m_slots.size(); ++index) {
      auto &slot = m_slots[index];
      if (slot.state.load(std::memory_order_acquire) != InputSlotState::Ready)
        continue;
      const auto &ticket = slot.input.ticket;
      if (selected == kInvalidSlot || ticket.generation > generation ||
          (ticket.generation == generation && ticket.frameId > frame)) {
        selected = index;
        generation = ticket.generation;
        frame = ticket.frameId;
      }
    }
    if (selected == kInvalidSlot)
      return {};

    auto expected = InputSlotState::Ready;
    auto &slot = m_slots[selected];
    if (!slot.state.compare_exchange_strong(
            expected, InputSlotState::InFlight, std::memory_order_acq_rel,
            std::memory_order_acquire))
      return {};
    slot.retireValue = retireValue;
    InputLease lease{selected, slot.input, slot.readyValue, retireValue};
    m_acquired.fetch_add(1, std::memory_order_relaxed);
    retireReadyBefore(generation, frame);
    return lease;
  }

  bool complete(const InputLease &lease, uint64_t completedValue) noexcept {
    if (!lease || lease.slot >= m_slots.size()) {
      m_rejected.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    auto &slot = m_slots[lease.slot];
    if (slot.state.load(std::memory_order_acquire) !=
            InputSlotState::InFlight ||
        slot.input.ticket.generation != lease.input.ticket.generation ||
        slot.input.ticket.frameId != lease.input.ticket.frameId ||
        slot.retireValue != lease.retireValue ||
        completedValue < slot.retireValue) {
      m_rejected.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    slot.input = {};
    slot.readyValue = 0;
    slot.retireValue = 0;
    slot.state.store(InputSlotState::Free, std::memory_order_release);
    m_completed.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  InputRingStatistics statistics() const noexcept {
    return {m_published.load(std::memory_order_relaxed),
            m_acquired.load(std::memory_order_relaxed),
            m_completed.load(std::memory_order_relaxed),
            m_droppedFull.load(std::memory_order_relaxed),
            m_droppedStale.load(std::memory_order_relaxed),
            m_rejected.load(std::memory_order_relaxed)};
  }

  InputSlotState state(size_t slot) const noexcept {
    return slot < m_slots.size()
               ? m_slots[slot].state.load(std::memory_order_acquire)
               : InputSlotState::Free;
  }
};

} // namespace ldb_fg
