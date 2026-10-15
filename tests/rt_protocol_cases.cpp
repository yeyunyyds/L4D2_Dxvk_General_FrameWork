#include "rt_protocol.h"

#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

using namespace ldb_rt;
void check(bool value, const char* why) { if (!value) { throw std::runtime_error(why); } }
Camera camera(float x = 0) {
  Camera c; c.width = 1920; c.height = 1200; c.nearPlane = 1; c.farPlane = 20000;
  for (unsigned i = 0; i < 4; ++i) { c.view[5 * i] = c.projection[5 * i] = 1; }
  c.view[12] = x; return c;
}
DrawInfo geometry(uint64_t version = 1) {
  DrawInfo d; d.id = 1; d.instance = 7; d.bindings = {{9, version}};
  d.transform[0] = d.transform[5] = d.transform[10] = 1; return d;
}
struct Fixture {
  Stream stream;
  Header h;
  uint64_t consumer = 77;
  explicit Fixture(size_t budget = 16 * 1024 * 1024) : stream(budget) {
    h.session = 13; h.run = 17; h.device = 19; h.producerThread = 23;
    send(Kind::Hello);
  }
  Bytes packet(Kind kind, const Bytes& payload = {}, uint64_t frame = 0, uint64_t view = 0) {
    h.kind = kind; h.frame = frame; h.view = view; ++h.sequence; return encode(h, payload);
  }
  void send(Kind kind, const Bytes& payload = {}, uint64_t frame = 0, uint64_t view = 0) {
    auto b = packet(kind, payload, frame, view);
    check(stream.apply(b.data(), b.size(), consumer) == Error::Ok, "valid packet rejected");
  }
  void setup(uint64_t frame = 1, uint64_t version = 1) {
    send(Kind::UpdateResource, resourcePayload(9, version, 1, {1, 2, 3, 4}));
    send(Kind::BeginFrame, {}, frame); send(Kind::BeginView, cameraPayload(camera(float(frame))), frame, 100);
  }
  std::unique_ptr<DrawSnapshot> draw(uint64_t frame = 1, uint64_t version = 1) {
    send(Kind::Draw, drawPayload(geometry(version)), frame, 100);
    std::unique_ptr<DrawSnapshot> result;
    check(stream.draw(812, consumer, result) == Error::Ok, "physical draw missing"); return result;
  }
  void end(uint64_t frame = 1) {
    send(Kind::EndView, {}, frame, 100); send(Kind::EndFrame, {}, frame);
    uint64_t presented = 0;
    check(stream.present(consumer, presented) == Error::Ok && presented == frame, "present mismatch");
  }
};

void malformed() {
  Fixture f;
  auto b = f.packet(Kind::BeginFrame, {}, 1);
  for (size_t size = 0; size < b.size(); ++size) {
    Packet p;
    check(decode(b.data(), size, p) != Error::Ok, "truncated packet accepted");
  }
  for (size_t offset : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(16), size_t(20), size_t(92)}) {
    auto bad = b; bad[offset] = 0xff;
    check(f.stream.apply(bad.data(), bad.size(), 77) != Error::Ok, "malformed header accepted");
    check(f.stream.sequence() == 1, "rejection consumed sequence");
  }
  check(f.stream.apply(b.data(), b.size(), 78) == Error::Thread, "wrong consumer accepted");
  check(f.stream.apply(b.data(), b.size(), 77) == Error::Ok, "valid retry rejected");
}
void nestedAndOrdered() {
  Fixture f; f.setup();
  f.send(Kind::BeginView, cameraPayload([] { auto c = camera(80); c.kind = ViewKind::Weapon; return c; }()), 1, 101);
  auto d = geometry();
  f.send(Kind::Draw, drawPayload(d), 1, 101);
  auto wrong = f.packet(Kind::EndView, {}, 1, 100);
  check(f.stream.apply(wrong.data(), wrong.size(), 77) == Error::Order, "unconsumed draw ignored");
  --f.h.sequence;
  std::unique_ptr<DrawSnapshot> owned;
  check(f.stream.draw(1, 77, owned) == Error::Ok && owned->camera.view[12] == 80, "nested camera mismatch");
  f.send(Kind::EndView, {}, 1, 101);
  d.id = 2; f.send(Kind::Draw, drawPayload(d), 1, 100);
  check(f.stream.draw(2, 77, owned) == Error::Ok && owned->camera.view[12] == 1, "parent view lost");
  f.end();
  uint64_t frame;
  check(f.stream.present(77, frame) == Error::Order, "double present accepted");
}
void invalidCameraAndBinding() {
  Fixture f;
  f.send(Kind::BeginFrame, {}, 1);
  for (unsigned variant = 0; variant < 5; ++variant) {
    auto c = camera();
    if (variant == 0) { c.view.fill(0); }
    if (variant == 1) { c.nearPlane = c.farPlane; }
    if (variant == 2) { c.projection[0] = std::numeric_limits<float>::quiet_NaN(); }
    if (variant == 3) { c.width = 0; }
    if (variant == 4) { c.farPlane = std::numeric_limits<float>::infinity(); }
    auto b = f.packet(Kind::BeginView, cameraPayload(c), 1, 100);
    check(f.stream.apply(b.data(), b.size(), 77) == Error::Camera, "bad camera accepted");
    --f.h.sequence;
  }
  f.send(Kind::BeginView, cameraPayload(camera()), 1, 100);
  auto b = f.packet(Kind::Draw, drawPayload(geometry()), 1, 100);
  check(f.stream.apply(b.data(), b.size(), 77) == Error::Resource, "unknown binding accepted");
  --f.h.sequence;
  f.send(Kind::UpdateResource, resourcePayload(9, 1, 1, {1}));
  auto wrongVersion = geometry(2);
  b = f.packet(Kind::Draw, drawPayload(wrongVersion), 1, 100);
  check(f.stream.apply(b.data(), b.size(), 77) == Error::Resource, "stale version accepted");
  --f.h.sequence; f.draw(); f.end();
}
void lifetimesAndBudget() {
  Fixture f(8); f.setup(); auto old = f.draw(); f.end();
  auto payload = resourcePayload(9, 2, 1, {5, 6, 7, 8});
  f.send(Kind::UpdateResource, payload);
  payload.assign(payload.size(), 0);
  check(old->resources[0]->data == Bytes({1, 2, 3, 4}), "old version overwritten");
  check(f.stream.retainedBytes() == 8, "retained memory not accounted");
  auto b = f.packet(Kind::UpdateResource, resourcePayload(9, 3, 1, {9}));
  check(f.stream.apply(b.data(), b.size(), 77) == Error::Limit, "retained memory cap bypassed");
  --f.h.sequence; old.reset();
  f.send(Kind::UpdateResource, resourcePayload(9, 3, 1, {9}));
  f.send(Kind::RetireResource, retirePayload(9, 3));
  b = f.packet(Kind::UpdateResource, resourcePayload(9, 1, 1, {2}));
  check(f.stream.apply(b.data(), b.size(), 77) == Error::Resource, "retired identity reused");
  --f.h.sequence;
  f.send(Kind::UpdateResource, resourcePayload(9, 4, 1, {2}));
  f.send(Kind::BeginFrame, {}, 2); f.send(Kind::BeginView, cameraPayload(camera(2)), 2, 100);
  auto retained = f.draw(2, 4);
  ++f.h.generation; f.send(Kind::Reset);
  check(retained->resources[0]->data == Bytes({2}) && f.stream.retainedBytes() == 1, "reset freed reader data");
  auto stale = f.h; --stale.generation; stale.kind = Kind::BeginFrame; stale.frame = 3; ++stale.sequence;
  b = encode(stale);
  check(f.stream.apply(b.data(), b.size(), 77) == Error::Identity, "old generation accepted");
  ++f.h.map; f.send(Kind::ChangeMap);
  retained.reset(); check(!f.stream.retainedBytes(), "resource retention leaked");
}

void overlappingFrames() {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<Bytes> queue;
  bool done = false;
  uint64_t published = 0;
  constexpr uint64_t frames = 3000;
  auto producer = std::async(std::launch::async, [&] {
    Header h; h.session = 1; h.run = 2; h.device = 3; h.producerThread = 11;
    auto emit = [&](Kind kind, Bytes payload = {}, uint64_t frame = 0, uint64_t view = 0) {
      h.kind = kind; h.frame = frame; h.view = view; ++h.sequence;
      auto bytes = encode(h, payload);
      std::unique_lock<std::mutex> lock(mutex);
      ready.wait(lock, [&] { return queue.size() < 64; });
      queue.push_back(std::move(bytes)); lock.unlock(); ready.notify_all();
    };
    emit(Kind::Hello);
    uint64_t version = 0;
    for (uint64_t frame = 1; frame <= frames; ++frame) {
      if (frame > 1 && frame % 32 == 0) { ++h.generation; emit(Kind::Reset); version = 0; }
      Bytes vertices{uint8_t(frame), uint8_t(frame >> 8)};
      emit(Kind::UpdateResource, resourcePayload(9, ++version, 1, vertices));
      vertices.assign(2, 0);
      emit(Kind::BeginFrame, {}, frame);
      auto c = camera(float(frame)); emit(Kind::BeginView, cameraPayload(c), frame, frame);
      c.view[12] = -100;
      emit(Kind::Draw, drawPayload(geometry(version)), frame, frame);
      emit(Kind::EndView, {}, frame, frame); emit(Kind::EndFrame, {}, frame);
      { std::lock_guard<std::mutex> lock(mutex); published = frame; }
      ready.notify_all();
    }
    { std::lock_guard<std::mutex> lock(mutex); done = true; }
    ready.notify_all();
  });
  Stream consumer;
  uint64_t rendered = 0, command = 0, overlapped = 0;
  std::deque<std::unique_ptr<DrawSnapshot>> retained;
  {
    std::unique_lock<std::mutex> lock(mutex);
    ready.wait(lock, [&] { return published >= 2; });
  }
  for (;;) {
    Bytes bytes;
    {
      std::unique_lock<std::mutex> lock(mutex);
      ready.wait(lock, [&] { return done || !queue.empty(); });
      if (queue.empty()) { break; }
      bytes = std::move(queue.front()); queue.pop_front();
    }
    ready.notify_all();
    Packet p; check(decode(bytes.data(), bytes.size(), p) == Error::Ok, "overlap wire");
    check(consumer.apply(bytes.data(), bytes.size(), 22) == Error::Ok, "overlap state");
    ++command;
    if (p.header.kind == Kind::Draw) {
      std::unique_ptr<DrawSnapshot> snapshot;
      check(consumer.draw(command, 22, snapshot) == Error::Ok, "overlap draw");
      check(snapshot->camera.view[12] == float(p.header.frame), "next-frame camera used");
      check(snapshot->resources[0]->data == Bytes({uint8_t(p.header.frame), uint8_t(p.header.frame >> 8)}), "next-frame resource used");
      retained.push_back(std::move(snapshot));
      if (retained.size() > 16) { retained.pop_front(); }
      std::lock_guard<std::mutex> lock(mutex);
      if (published > p.header.frame) { ++overlapped; }
    }
    if (p.header.kind == Kind::EndFrame) {
      uint64_t frame;
      check(consumer.present(22, frame) == Error::Ok && frame == ++rendered, "overlap present");
    }
  }
  producer.get();
  check(rendered == frames && overlapped > 0, "no actual producer overlap");
  std::cout << "overlapped_frames=" << overlapped << " rendered_frames=" << rendered << '\n';
}

int main() {
  try {
    malformed(); nestedAndOrdered(); invalidCameraAndBinding(); lifetimesAndBudget(); overlappingFrames();
    std::cout << "RT protocol execution cases: PASS (5 groups)\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
