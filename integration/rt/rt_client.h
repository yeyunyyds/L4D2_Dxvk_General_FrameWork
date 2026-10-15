#pragma once

#include "rt_bridge_api.h"
#include "rt_protocol.h"

#include <atomic>
#include <mutex>

namespace ldb_rt {

template<class Device> class ClientStream final : public IMetadataStream {
  std::atomic<ULONG> m_refs{1};
  Device* m_device;
  std::mutex m_mutex;
  bool m_acknowledged = false;
public:
  explicit ClientStream(Device* device) : m_device(device) { m_device->AddRef(); }
  ~ClientStream() { m_device->Release(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
    if (!output) { return E_POINTER; }
    *output = nullptr;
    if (iid == IID_IUnknown) { return m_device->QueryInterface(iid, output); }
    if (iid != kStreamGuid) { return E_NOINTERFACE; }
    *output = static_cast<IMetadataStream*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --m_refs;
    if (!remaining) { delete this; }
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE GetBinding(uint32_t* version, uint64_t* device) override {
    if (!version || !device) { return E_POINTER; }
    *version = kVersion; *device = m_device->getId(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Submit(const void* bytes, uint32_t size, uint32_t* commandUid) override {
    if (!bytes || !commandUid) { return E_POINTER; }
    *commandUid = 0;
    if (size < kHeaderBytes || size > kMaxPacketBytes) { return E_INVALIDARG; }
    try {
      Bytes owned(static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + size);
      Packet p;
      if (decode(owned.data(), owned.size(), p) != Error::Ok || p.header.device != m_device->getId()) {
        return E_INVALIDARG;
      }
      std::lock_guard<std::mutex> lock(m_mutex);
      const bool hello = p.header.kind == Kind::Hello;
      if (!hello && !m_acknowledged) { return E_UNEXPECTED; }
      const HRESULT result = m_device->submitRtPacket(owned.data(), size, hello, commandUid);
      if (hello) { m_acknowledged = SUCCEEDED(result); }
      return result;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
  }
};

} // namespace ldb_rt
