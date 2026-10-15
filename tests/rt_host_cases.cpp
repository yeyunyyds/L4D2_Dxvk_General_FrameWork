#include "rt_client.h"
#include "rt_host.h"

#include <iostream>

using namespace ldb_rt;
void require(bool value, const char* why) { if (!value) { throw std::runtime_error(why); } }

// Executes the production COM adapter and Host observer without a GPU or game.
struct Device final : IUnknown {
  ULONG refs = 1;
  uint32_t uid = 1;
  uint64_t id;
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
    *output = ++uid; return ldb_rt_host::submit(id, bytes, size, *output, hello);
  }
};

int main() {
  try {
    Device device(190);
    IMetadataStream* api = new ClientStream<Device>(&device);
    uint32_t version = 0, uid = 0; uint64_t id = 0;
    require(SUCCEEDED(api->GetBinding(&version, &id)) && version == 1 && id == 190, "binding");
    IUnknown* identity = nullptr;
    require(SUCCEEDED(api->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity))) && identity == &device, "COM identity");
    identity->Release();
    Header h; h.session = 101; h.run = 102; h.device = id; h.producerThread = GetCurrentThreadId();
    auto send = [&](Kind kind, const Bytes& payload = Bytes{}, uint64_t frame = 0, uint64_t view = 0) {
      h.kind = kind; h.frame = frame; h.view = view; ++h.sequence;
      const auto bytes = encode(h, payload);
      return api->Submit(bytes.data(), uint32_t(bytes.size()), &uid);
    };
    require(FAILED(send(Kind::BeginFrame, {}, 1)), "metadata allowed before handshake");
    --h.sequence;
    require(SUCCEEDED(send(Kind::Hello)), "Host handshake");
    const auto menuUid = ++device.uid;
    ldb_rt_host::draw(id, menuUid); ldb_rt_host::drawResult(id, menuUid, S_OK);
    ldb_rt_host::present(id, ++device.uid, S_OK);
    for (uint64_t ordinal = 1; ordinal <= 4; ++ordinal) {
      const uint64_t frame = ordinal <= 2 ? ordinal : ordinal + 2;
      if (ordinal == 3) {
        ldb_rt_host::reset(id, ++device.uid, E_FAIL);
        require(FAILED(send(Kind::BeginFrame, {}, frame)), "lost-device metadata was consumed");
        ++h.generation;
        require(FAILED(send(Kind::Reset)), "failed physical reset acknowledged");
        --h.sequence;
        ldb_rt_host::reset(id, ++device.uid, S_OK);
        require(SUCCEEDED(send(Kind::Reset)), "successful reset failed recovery");
      }
      require(SUCCEEDED(send(Kind::BeginFrame, {}, frame)), "frame");
      Camera camera; camera.width = 1920; camera.height = 1200; camera.nearPlane = 1; camera.farPlane = 10000;
      for (unsigned i = 0; i < 4; ++i) { camera.view[i * 5] = camera.projection[i * 5] = 1; }
      camera.view[12] = float(frame);
      require(SUCCEEDED(send(Kind::BeginView, cameraPayload(camera), frame, 1)), "view");
      DrawInfo draw; draw.id = 1; draw.instance = 5;
      draw.transform[0] = draw.transform[5] = draw.transform[10] = 1;
      require(SUCCEEDED(send(Kind::Draw, drawPayload(draw), frame, 1)), "draw metadata");
      const auto drawUid = ++device.uid;
      ldb_rt_host::draw(id, drawUid); ldb_rt_host::drawResult(id, drawUid, S_OK);
      require(SUCCEEDED(send(Kind::EndView, {}, frame, 1)), "end view");
      require(SUCCEEDED(send(Kind::EndFrame, {}, frame)), "end frame");
      ldb_rt_host::present(id, ++device.uid, S_OK);
    }
    auto& observer = *ldb_rt_host::observers().at(id);
    require(observer.draws == 4 && observer.presents == 4 && observer.unlabelled == 1 && !observer.failed, "Host counts");
    api->Release(); require(device.refs == 1, "adapter leaked parent reference");
    ldb_rt_host::destroy(id);
    std::cout << "RT COM/Host observer: PASS (4 frames, failed/successful Reset, ptr_bits=" << 8 * sizeof(void*) << ")\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
