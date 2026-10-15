#include "rt_protocol.h"
#include "rt_source_queue.h"
#include "rt_client.h"
#include "rt_host.h"
#include "rt_static_device.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>

using namespace ldb_rt;
void require(bool value, const char* why) { if (!value) { throw std::runtime_error(why); } }

// Owns SDK queue references exactly as execution/flush do. This is a test queue,
// not a claim to have run CMatQueuedRenderContext in the game.
class Queue final : public ICallQueue {
  std::mutex m_mutex;
  std::condition_variable m_ready;
  std::deque<CFunctor*> m_tasks;
  bool m_closed = false;
  void QueueFunctorInternal(CFunctor* task) override {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_ready.wait(lock, [&] { return m_closed || m_tasks.size() < 64; });
    if (m_closed) { lock.unlock(); task->Release(); return; }
    m_tasks.push_back(task); lock.unlock(); m_ready.notify_all();
  }
public:
  ~Queue() { close(); flush(); }
  void close() {
    { std::lock_guard<std::mutex> lock(m_mutex); m_closed = true; }
    m_ready.notify_all();
  }
  bool runOne() {
    CFunctor* task;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_ready.wait(lock, [&] { return m_closed || !m_tasks.empty(); });
      if (m_tasks.empty()) { return false; }
      task = m_tasks.front(); m_tasks.pop_front();
    }
    m_ready.notify_all();
    try { (*task)(); } catch (...) { task->Release(); throw; }
    task->Release(); return true;
  }
  void flush() {
    std::deque<CFunctor*> tasks;
    { std::lock_guard<std::mutex> lock(m_mutex); tasks.swap(m_tasks); }
    for (auto* task : tasks) { task->Release(); }
    m_ready.notify_all();
  }
};

class Command final : public CFunctor {
  int m_refs = 1;
  std::function<void()> m_call;
public:
  explicit Command(std::function<void()> call) : m_call(std::move(call)) { m_nUserID = 0; }
  int AddRef() override { return ++m_refs; }
  int Release() override { const int n = --m_refs; if (!n) { delete this; } return n; }
  void operator()() override { m_call(); }
};
void command(Queue& queue, std::function<void()> call) {
  auto* task = new Command(std::move(call)); queue.QueueFunctor(task); task->Release();
}

struct Device final : IUnknown {
  std::atomic<ULONG> refs{1};
  uint32_t uid = 0;
  uint64_t id;
  DWORD producer = 0;
  uint64_t frames = 0, overlap = 0;
  std::atomic<uint64_t> published{0};
  Stream mirror;
  rt_test::Graphics graphics;
  bool expectStatic = false;
  bool resetBarrier = false;
  explicit Device(uint64_t value) : id(value) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
    if (!output) { return E_POINTER; }
    *output = nullptr;
    if (iid != IID_IUnknown) { return E_NOINTERFACE; }
    *output = static_cast<IUnknown*>(this); AddRef(); return S_OK;
  }
  uint64_t getId() const { return id; }
  HRESULT submitRtPacket(const void* bytes, uint32_t size, bool hello, uint32_t* output) {
    Packet packet;
    require(decode(bytes, size, packet) == Error::Ok, "source wire");
    require(packet.header.producerThread == producer && packet.header.producerTicks != 0, "producer identity lost");
    const bool barrier = resetBarrier && packet.header.kind == Kind::Reset;
    require(mirror.apply(bytes, size, GetCurrentThreadId(), barrier) == Error::Ok, "source consumer state");
    if (barrier) { resetBarrier = false; }
    *output = ++uid;
    return ldb_rt_host::submit(id, bytes, size, *output, hello);
  }
  void draw(uint64_t frame, uint64_t generation) {
    const auto drawUid = ++uid;
    std::unique_ptr<DrawSnapshot> snapshot;
    require(mirror.draw(drawUid, GetCurrentThreadId(), snapshot) == Error::Ok, "source draw association");
    require(snapshot->header.frame == frame && snapshot->header.generation == generation, "stale source identity");
    require(snapshot->camera.view[12] == float(frame), "source used later camera");
    require(snapshot->resources[0]->data == Bytes({uint8_t(frame), uint8_t(frame >> 8)}), "source used later resource");
    if (published.load() > frame) { ++overlap; }
    const auto result = ldb_rt_host::drawCommand(id, drawUid, &graphics, D3DPT_TRIANGLELIST, false, [&] {
      if (expectStatic) {
        require(!graphics.currentShader && graphics.transforms[1].m[3][0] == float(frame), "static scope used wrong frame/owner");
      }
      return S_OK;
    });
    require(SUCCEEDED(result) && graphics.restored() && graphics.leasesReleased() && !graphics.foreign, "static scope leaked into next draw");
    ldb_rt_host::drawResult(id, drawUid, result);
  }
  void present(uint64_t expected) {
    uint64_t frame = 0;
    require(mirror.present(GetCurrentThreadId(), frame) == Error::Ok && frame == expected, "source present");
    ldb_rt_host::present(id, ++uid, S_OK); ++frames;
  }
  void reset(HRESULT result) {
    ldb_rt_host::reset(id, ++uid, result); resetBarrier = SUCCEEDED(result);
  }
};

Header identity(uint64_t device) { Header h; h.session = 900; h.run = 901; h.device = device; return h; }

void postFrame(SourceQueue& lane, Queue* queue, Header h, uint64_t frame, uint64_t version) {
  auto post = [&](Kind kind, const Bytes& payload = Bytes{}, uint64_t f = 0, uint64_t v = 0) {
    h.kind = kind; h.frame = f; h.view = v;
    require(SUCCEEDED(lane.post(queue, h, payload)), "source enqueue");
  };
  Bytes vertices{uint8_t(frame), uint8_t(frame >> 8)};
  auto resource = resourcePayload(1, version, 1, vertices);
  post(Kind::ResourceUpdate, resource); resource.assign(resource.size(), 0);
  post(Kind::BeginFrame, {}, frame);
  Camera camera; camera.width = 1920; camera.height = 1080; camera.nearPlane = 1; camera.farPlane = 10000;
  for (unsigned i = 0; i < 4; ++i) { camera.view[i * 5] = camera.projection[i * 5] = 1; }
  camera.view[12] = float(frame);
  auto payload = cameraPayload(camera);
  post(Kind::BeginView, payload, frame, 1); payload.assign(payload.size(), 0);
  DrawInfo draw; draw.id = 1; draw.instance = 7; draw.bindings.push_back({1, version});
  draw.categories = kStaticOpaqueWorld;
  draw.transform[0] = draw.transform[5] = draw.transform[10] = 1;
  post(Kind::Draw, drawPayload(draw), frame, 1);
}
void endFrame(SourceQueue& lane, Queue* queue, Header h, uint64_t frame) {
  h.frame = frame; h.view = 1; h.kind = Kind::EndView;
  require(SUCCEEDED(lane.post(queue, h)), "source end view");
  h.view = 0; h.kind = Kind::EndFrame;
  require(SUCCEEDED(lane.post(queue, h)), "source end frame");
}

void immediateAndRecovery() {
  Device device(192); device.producer = GetCurrentThreadId();
  auto* api = new ClientStream<Device>(&device);
  {
    SourceQueue lane(api, identity(device.id)); api->Release();
    Header h = identity(device.id);
    require(SUCCEEDED(lane.post(nullptr, h)), "immediate hello");
    postFrame(lane, nullptr, h, 1, 1); device.draw(1, 1);
    endFrame(lane, nullptr, h, 1); device.present(1);
    Queue oldQueue;
    postFrame(lane, &oldQueue, h, 2, 2); // Never execute this old generation.
    lane.stop(); device.reset(E_FAIL); h.kind = Kind::Reset; ++h.generation;
    require(FAILED(lane.afterDeviceReset(nullptr, h, E_FAIL)), "failed reset resumed source lane");
    require(FAILED(lane.post(nullptr, identity(device.id))), "stopped source lane accepted data");
    device.reset(S_OK);
    require(SUCCEEDED(lane.afterDeviceReset(nullptr, h, S_OK)), "source reset recovery");
    oldQueue.flush(); // Cancellation of an OLD token must not kill recovery.
    postFrame(lane, nullptr, h, 5, 1); device.draw(5, 2);
    endFrame(lane, nullptr, h, 5); device.present(5);
    const auto stats = lane.stats();
    require(stats.cancelled == 4 && !stats.pending && !stats.retainedBytes && SUCCEEDED(stats.lastError), "source recovery ownership");
  }
  require(device.refs.load() == 1, "source sink lease leaked");
  auto& observer = *ldb_rt_host::observers().at(device.id);
  require(observer.draws == 2 && observer.presents == 2 && !observer.failed, "source Host recovery");
  ldb_rt_host::destroy(device.id);
}

void cancellationAndLimits() {
  Device device(193); device.producer = GetCurrentThreadId();
  auto* api = new ClientStream<Device>(&device);
  {
    Queue queue;
    SourceQueue lane(api, identity(device.id), kHeaderBytes, 1);
    require(SUCCEEDED(lane.post(&queue, identity(device.id))), "bounded hello");
    require(lane.post(&queue, identity(device.id)) == E_OUTOFMEMORY, "source budget not enforced");
    queue.close(); while (queue.runOne()) { }
    const auto stats = lane.stats();
    require(stats.stale == 1 && stats.rejected == 1 && !stats.pending && !stats.retainedBytes, "bounded source cleanup");
  }
  {
    Queue queue;
    SourceQueue lane(api, identity(device.id));
    require(SUCCEEDED(lane.post(&queue, identity(device.id))), "flush hello");
    queue.flush();
    require(FAILED(lane.post(nullptr, identity(device.id))) && lane.stats().cancelled == 1, "flush did not stop lane");
  }
  {
    Queue queue;
    { SourceQueue lane(api, identity(device.id)); require(SUCCEEDED(lane.post(&queue, identity(device.id))), "owner lifetime"); }
    require(device.refs.load() == 2, "pending callback lost COM lease");
    queue.close(); while (queue.runOne()) { }
  }
  api->Release(); require(device.refs.load() == 1, "cancelled source lease leaked");
}

void overlappingSourceFrames() {
  constexpr uint64_t frames = 512;
  SetEnvironmentVariableW(L"LDB_RT_STATIC_DIAGNOSTIC", L"1");
  Device device(194); device.expectStatic = true;
  auto* api = new ClientStream<Device>(&device);
  {
    SourceQueue lane(api, identity(device.id)); api->Release();
    Queue queue;
    std::promise<void> publishedTwo; auto ready = publishedTwo.get_future();
    auto producer = std::async(std::launch::async, [&] {
      try {
        device.producer = GetCurrentThreadId();
        Header h = identity(device.id);
        require(SUCCEEDED(lane.post(&queue, h)), "queued hello");
        for (uint64_t frame = 1; frame <= frames; ++frame) {
          postFrame(lane, &queue, h, frame, frame);
          command(queue, [&, frame] { device.draw(frame, 1); });
          endFrame(lane, &queue, h, frame);
          command(queue, [&, frame] { device.present(frame); });
          device.published.store(frame);
          if (frame == 2) { publishedTwo.set_value(); }
        }
        queue.close();
      } catch (...) {
        try { publishedTwo.set_exception(std::current_exception()); } catch (const std::future_error&) { }
        queue.close(); throw;
      }
    });
    try { ready.get(); while (queue.runOne()) { } producer.get(); }
    catch (...) { queue.close(); producer.wait(); throw; }
    const auto stats = lane.stats();
    require(device.producer != GetCurrentThreadId() && stats.lastConsumerThread == GetCurrentThreadId(), "no separate source producer");
    require(stats.accepted == 1 + 6 * frames && stats.executed == stats.accepted &&
            !stats.pending && !stats.retainedBytes && !stats.rejected && !stats.cancelled, "source execution accounting");
    auto& observer = *ldb_rt_host::observers().at(device.id);
    require(device.frames == frames && device.overlap > 0 && observer.draws == frames &&
            observer.presents == frames && !observer.failed, "source Host frame association");
    std::cout << "source_frames=" << frames << " overlapped_draws=" << device.overlap << '\n';
  }
  require(device.refs.load() == 1, "overlapping source lease leaked");
  ldb_rt_host::destroy(device.id);
  SetEnvironmentVariableW(L"LDB_RT_STATIC_DIAGNOSTIC", nullptr);
}

int main() {
  try {
    immediateAndRecovery(); cancellationAndLimits(); overlappingSourceFrames();
    std::cout << "RT L4D2 SDK queue/COM/Host: PASS (3 groups, ptr_bits=" << 8 * sizeof(void*) << ")\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
