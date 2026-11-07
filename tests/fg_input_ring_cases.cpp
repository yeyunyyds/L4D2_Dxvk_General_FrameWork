#include "fg_input_ring.h"

#include <iostream>
#include <stdexcept>
#include <atomic>
#include <thread>

using namespace ldb_fg;

void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

LdbFgFrameInputsV1 input(uint64_t generation, uint64_t frame,
                         uint32_t extraFlags = 0) {
  LdbFgFrameInputsV1 value{};
  value.flags = LDB_FG_INPUT_COLOR | LDB_FG_INPUT_DEPTH |
                LDB_FG_INPUT_CAMERA_MOTION | extraFlags;
  value.width = 1920;
  value.height = 1080;
  value.colorFormat = 1;
  value.depthFormat = 2;
  value.ticket.generation = generation;
  value.ticket.frameId = frame;
  value.color = 11;
  value.depth = 12;
  value.frameTimeMs = 16.67f;
  value.cameraNear = .1f;
  value.cameraFar = 1000.f;
  value.cameraVerticalFov = 1.f;
  value.metersPerUnit = .0254f;
  return value;
}

int main() {
  try {
    {
      InputRing ring;
      check(ring.publish(input(1, 1)) == PublishResult::Published,
            "initial publish failed");
      const auto lease = ring.acquireLatest(7);
      check(lease && lease.input.ticket.frameId == 1,
            "ordered acquire failed");
      check(!ring.complete(lease, 6), "slot retired before GPU completion");
      check(ring.state(lease.slot) == InputSlotState::InFlight,
            "early completion released slot");
      check(ring.complete(lease, 7), "timeline completion failed");
      check(ring.state(lease.slot) == InputSlotState::Free,
            "completed slot was not released");
    }
    {
      InputRing ring;
      InputLease leases[kInputRingSize];
      for (uint64_t frame = 1; frame <= kInputRingSize; ++frame) {
        check(ring.publish(input(1, frame)) == PublishResult::Published,
              "ring fill publish failed");
        leases[frame - 1] = ring.acquireLatest(10 + frame);
        check(bool(leases[frame - 1]), "ring fill acquire failed");
      }
      check(ring.publish(input(1, 4)) == PublishResult::Full,
            "full ring accepted another frame");
      check(ring.complete(leases[0], 11), "oldest slot completion failed");
      check(ring.publish(input(1, 4)) == PublishResult::Published,
            "full-ring retry was rejected as out of order");
    }
    {
      InputRing ring;
      check(ring.publish(input(4, 10)) == PublishResult::Published,
            "old generation publish failed");
      check(ring.publish(input(5, 1)) == PublishResult::OutOfOrder,
            "generation change without reset accepted");
      check(ring.publish(input(5, 1, LDB_FG_INPUT_RESET)) ==
                PublishResult::Published,
            "generation reset publish failed");
      const auto lease = ring.acquireLatest(20);
      check(lease && lease.input.ticket.generation == 5 &&
                lease.input.ticket.frameId == 1,
            "old generation survived reset");
      check(ring.statistics().droppedStale == 1,
            "reset did not account for stale input");
    }
    {
      InputRing ring;
      check(ring.publish(input(1, 2)) == PublishResult::Published,
            "ordering setup failed");
      check(ring.publish(input(1, 2)) == PublishResult::OutOfOrder &&
                ring.publish(input(1, 1)) == PublishResult::OutOfOrder,
            "duplicate or older frame accepted");
      auto invalid = input(1, 3);
      invalid.depth = 0;
      check(ring.publish(invalid) == PublishResult::Invalid,
            "invalid input accepted");
    }
    {
      InputRing ring;
      check(ring.publish(input(1, 1)) == PublishResult::Published &&
                ring.publish(input(1, 2)) == PublishResult::Published &&
                ring.publish(input(1, 3)) == PublishResult::Published,
            "latest selection setup failed");
      const auto lease = ring.acquireLatest(30);
      check(lease && lease.input.ticket.frameId == 3,
            "newest input was not selected");
      const auto stats = ring.statistics();
      check(stats.droppedStale == 2 && stats.acquired == 1,
            "older ready inputs were not retired");
    }
    {
      InputRing ring;
      constexpr uint64_t count = 20000;
      std::atomic<bool> producerDone{false};
      std::atomic<bool> producerFailed{false};
      std::thread producer([&] {
        for (uint64_t frame = 1; frame <= count;) {
          const auto result = ring.publish(input(1, frame));
          if (result == PublishResult::Published)
            ++frame;
          else if (result != PublishResult::Full) {
            producerFailed.store(true, std::memory_order_relaxed);
            break;
          }
          else
            std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
      });
      while (true) {
        const auto lease = ring.acquireLatest(1000000 + count);
        if (lease)
          check(ring.complete(lease, lease.retireValue),
                "concurrent completion failed");
        else
          std::this_thread::yield();
        const auto stats = ring.statistics();
        if (producerDone.load(std::memory_order_acquire) &&
            (producerFailed.load(std::memory_order_relaxed) ||
             stats.completed + stats.droppedStale == stats.published))
          break;
      }
      producer.join();
      const auto stats = ring.statistics();
      check(!producerFailed.load(std::memory_order_relaxed) &&
                stats.published == count &&
                stats.completed + stats.droppedStale == count,
            "concurrent ring lost ownership accounting");
    }
    std::cout << "passed: ordering, full-ring retry, generation reset, "
                 "validation, latest selection, timeline retirement, SPSC "
                 "stress\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
