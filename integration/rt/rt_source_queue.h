#pragma once

#include "rt_bridge_api.h"
#include "rt_protocol.h"

#include <limits>

// Compile against the pinned L4D2 SDK, not a hand-written CFunctor vtable.
#include <tier1/callqueue.h>

namespace ldb_rt {

// One lane belongs to one Source material command stream. The caller must insert
// callbacks beside the corresponding render commands on that SAME context.
// This class does not discover game interfaces, install hooks, or change CVars.
class SourceQueue final {
public:
  struct Stats {
    uint64_t accepted, executed, cancelled, stale, rejected;
    size_t pending, retainedBytes;
    HRESULT lastError;
    DWORD lastConsumerThread;
  };

private:
  struct State {
    IMetadataStream* sink;
    Header identity;
    const size_t byteLimit, taskLimit;
    // Even = accepting; odd = stopped. Recovery advances to a fresh even token.
    std::atomic<uint64_t> token{2};
    std::atomic<uint64_t> accepted{0}, executed{0}, cancelled{0}, stale{0}, rejected{0};
    std::atomic<size_t> pending{0}, retainedBytes{0};
    std::atomic<HRESULT> lastError{S_OK};
    std::atomic<DWORD> lastConsumerThread{0};
    std::atomic_flag consuming = ATOMIC_FLAG_INIT;
    uint64_t sequence = 0; // Only touched by the serial Source consumer.

    State(IMetadataStream* value, Header h, size_t bytes, size_t tasks)
      : sink(value), identity(h), byteLimit(bytes), taskLimit(tasks) { sink->AddRef(); }
    ~State() { sink->Release(); }

    void stop(uint64_t expected, HRESULT reason) noexcept {
      if (!(expected & 1) && token.compare_exchange_strong(expected, expected | 1)) {
        lastError.store(reason);
      }
    }
    bool reserve(size_t size) noexcept {
      auto count = pending.load();
      do { if (count >= taskLimit) { return false; } }
      while (!pending.compare_exchange_weak(count, count + 1));
      auto used = retainedBytes.load();
      do {
        if (size > byteLimit || used > byteLimit - size) { --pending; return false; }
      } while (!retainedBytes.compare_exchange_weak(used, used + size));
      return true;
    }
    void release(size_t size) noexcept { retainedBytes.fetch_sub(size); --pending; }
  };

  class Task final : public CFunctor {
    std::atomic<int> m_refs{1};
    std::shared_ptr<State> m_state;
    const uint64_t m_token;
    Bytes m_bytes;
    std::atomic<bool> m_called{false};
  public:
    Task(std::shared_ptr<State> state, uint64_t token, Bytes bytes) noexcept
      : m_state(std::move(state)), m_token(token), m_bytes(std::move(bytes)) { m_nUserID = 0; }
    ~Task() {
      if (!m_called.load()) {
        ++m_state->cancelled;
        // Flushing a partial view must stop later labels, not silently shift them.
        m_state->stop(m_token, HRESULT_FROM_WIN32(ERROR_CANCELLED));
      }
      m_state->release(m_bytes.size());
    }
    int AddRef() override { return ++m_refs; }
    int Release() override {
      const int remaining = --m_refs;
      if (!remaining) { delete this; }
      return remaining;
    }
    void operator()() noexcept override {
      if (m_called.exchange(true)) { m_state->stop(m_token, E_UNEXPECTED); return; }
      auto& state = *m_state;
      if (state.token.load() != m_token) { ++state.stale; return; }
      // Diagnose an invalid mixed-context binding; do not serialize producers
      // behind a global rendering mutex and call that a threading fix.
      if (state.consuming.test_and_set()) { state.stop(m_token, E_UNEXPECTED); return; }
      struct Unlock { State& state; ~Unlock() { state.consuming.clear(); } } unlock{state};
      if (state.token.load() != m_token) { ++state.stale; return; }
      if (state.sequence == (std::numeric_limits<uint64_t>::max)()) {
        state.stop(m_token, E_UNEXPECTED); return;
      }
      // Assign transport order at execution, preserving producer ID/time and
      // captured frame/view/camera. Sequence is the u64 at wire offset 80.
      const uint64_t sequence = ++state.sequence;
      for (unsigned n = 0; n < 8; ++n) { m_bytes[80 + n] = uint8_t(sequence >> (8 * n)); }
      state.lastConsumerThread.store(GetCurrentThreadId());
      uint32_t uid = 0;
      HRESULT result = E_FAIL;
      try { result = state.sink->Submit(m_bytes.data(), uint32_t(m_bytes.size()), &uid); }
      catch (...) { result = E_FAIL; } // Never unwind through Source's executor.
      if (FAILED(result)) { state.stop(m_token, result); }
      else { ++state.executed; }
    }
  };

  std::shared_ptr<State> m_state;

  HRESULT enqueue(ICallQueue* queue, Header header, const Bytes& payload, bool recovery) {
    const auto state = m_state;
    const uint64_t token = state->token.load();
    if (token & 1) { return state->lastError.load(); }
    const auto& id = state->identity;
    if (header.session != id.session || header.run != id.run || header.device != id.device ||
        (header.kind == Kind::Reset && !recovery) || payload.size() > kMaxPacketBytes - kHeaderBytes) {
      ++state->rejected; state->stop(token, E_INVALIDARG); return E_INVALIDARG;
    }
    const size_t size = kHeaderBytes + payload.size();
    if (!state->reserve(size)) {
      ++state->rejected; state->stop(token, E_OUTOFMEMORY); return E_OUTOFMEMORY;
    }
    Task* task = nullptr;
    try {
      header.sequence = 0;
      header.producerThread = GetCurrentThreadId();
      LARGE_INTEGER ticks{}; QueryPerformanceCounter(&ticks); header.producerTicks = ticks.QuadPart;
      task = new Task(state, token, encode(header, payload));
    } catch (const std::bad_alloc&) {
      state->release(size); ++state->rejected; state->stop(token, E_OUTOFMEMORY); return E_OUTOFMEMORY;
    }
    ++state->accepted;
    // The SDK adds a queue reference. Our reference is released in both queued
    // and immediate modes; Source releases its reference on execution OR flush.
    if (queue) { queue->QueueFunctor(task); }
    else { (*task)(); }
    task->Release();
    return S_OK; // Accepted locally, not proof of Host acceptance or rendering.
  }

public:
  SourceQueue(IMetadataStream* sink, Header identity, size_t byteLimit = 16 * 1024 * 1024,
              size_t taskLimit = 4096) {
    if (!sink || !identity.session || !identity.run || !identity.device || !byteLimit || !taskLimit) {
      throw std::invalid_argument("RT Source queue binding");
    }
    uint32_t version = 0; uint64_t device = 0;
    if (FAILED(sink->GetBinding(&version, &device)) || version != kVersion || device != identity.device) {
      throw std::invalid_argument("RT Source queue device/version");
    }
    m_state = std::make_shared<State>(sink, identity, byteLimit, taskLimit);
  }
  SourceQueue(const SourceQueue&) = delete;
  SourceQueue& operator=(const SourceQueue&) = delete;
  ~SourceQueue() { stop(); }

  HRESULT post(ICallQueue* queue, Header header, const Bytes& payload = {}) {
    return enqueue(queue, header, payload, false);
  }
  void stop() noexcept {
    const auto token = m_state->token.load();
    m_state->stop(token, HRESULT_FROM_WIN32(ERROR_CANCELLED));
  }
  // Invoke at the confirmed successful physical Reset boundary, before any new
  // render commands. The caller provides the new generation/map and must order
  // this callback on the same Source context as all subsequent metadata.
  HRESULT afterDeviceReset(ICallQueue* queue, Header reset, HRESULT physicalResult) {
    if (FAILED(physicalResult)) {
      m_state->stop(m_state->token.load(), physicalResult); return physicalResult;
    }
    if (reset.kind != Kind::Reset || reset.frame || reset.view) { stop(); return E_INVALIDARG; }
    auto token = m_state->token.load();
    for (;;) {
      if (token >= UINT64_MAX - 2) { stop(); return E_UNEXPECTED; }
      if (m_state->token.compare_exchange_weak(token, (token | 1) + 1)) { break; }
    }
    m_state->lastError.store(S_OK);
    return enqueue(queue, reset, {}, true);
  }
  Stats stats() const noexcept {
    const auto& s = *m_state;
    return {s.accepted.load(), s.executed.load(), s.cancelled.load(), s.stale.load(), s.rejected.load(),
            s.pending.load(), s.retainedBytes.load(), s.lastError.load(), s.lastConsumerThread.load()};
  }
};

} // namespace ldb_rt
