#pragma once

#include <unknwn.h>
#include <cstdint>

namespace ldb_rt {

inline constexpr GUID kStreamGuid = {0x78c1b01d, 0x1dd5, 0x4b43, {0x94, 0xb6, 0x3d, 0x80, 0x06, 0x51, 0xb9, 0x64}};

// Query on the LDB proxy device. Hello acknowledges Host support before other packets.
struct IMetadataStream : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetBinding(uint32_t* version, uint64_t* device) = 0;
  virtual HRESULT STDMETHODCALLTYPE Submit(const void* bytes, uint32_t size, uint32_t* commandUid) = 0;
};

} // namespace ldb_rt
