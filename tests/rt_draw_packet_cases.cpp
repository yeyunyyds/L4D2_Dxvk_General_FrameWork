#include "rt_draw_packet.h"
#include <iostream>
#include <limits>

using namespace ldb_rt;
using namespace ldb_rt::draw_observe;
void require(bool value, const char* why) { if (!value) { throw std::runtime_error(why); } }
struct Memory {
  std::vector<uint8_t> data = std::vector<uint8_t>(65536);
  unsigned reads = 0, failAt = 0;
  bool changeTop = false;
  bool read(uint32_t address, void* out, size_t bytes) {
    if (++reads == failAt || address > data.size() || bytes > data.size() - address) { return false; }
    std::memcpy(out, data.data() + address, bytes);
    if (changeTop && reads == 12) { const uint32_t changed = 1; std::memcpy(data.data() + 0x201c, &changed, 4); }
    return true;
  }
  template<class T> void put(size_t at, const T& v) { std::memcpy(data.data() + at, &v, sizeof(v)); }
};
Memory fixture() {
  Memory m;
  const uint32_t fields[] = {0x270,0x248,0x24c};
  for (unsigned i = 0; i < 3; ++i) {
    const uint32_t stack = 0x2000 + i * 0x100, base = 0x3000 + i * 0x100;
    m.put(0x1000 + fields[i], stack); m.put(stack + 8, base);
    std::array<float,16> matrix{}; matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
    matrix[12] = float(i + 5); m.put(base,matrix);
  }
  return m;
}
int main() {
  try {
    auto m = fixture(); Snapshot s; s.session = 42; s.ordinal = 1; s.thread = 17;
    s.viewport = {0,0,1920,1080}; s.status = matrices(m,0x1000,s);
    require(s.status == Status::Valid && s.world[12] == 5 && s.view[12] == 6 && s.projection[12] == 7,"consume owned stack matrices");
    const auto bytes = pack(s); require(bytes.size() == kSnapshotBytes,"wire size");
    Snapshot copy; require(unpack(bytes.data(),bytes.size(),copy) && copy.world == s.world && copy.view == s.view,"wire roundtrip");
    require(!unpack(bytes.data(),bytes.size()-1,copy),"reject truncated snapshot");
    auto malformed = bytes; malformed[4] = 2; require(!unpack(malformed.data(),malformed.size(),copy),"reject version");
    s.projection.fill(0); const auto singular = pack(s);
    require(!unpack(singular.data(),singular.size(),copy),"valid flag cannot hide singular camera");
    for (unsigned failure = 1; failure <= 18; ++failure) {
      auto invalid = fixture(); invalid.failAt = failure;
      require(matrices(invalid,0x1000,s) == Status::ReadFailed,"read failure cannot become valid");
    }
    m = fixture(); m.put(0x201c,uint32_t(1025)); require(matrices(m,0x1000,s) == Status::StackInvalid,"bound matrix index");
    m = fixture(); m.changeTop = true; require(matrices(m,0x1000,s) == Status::ChangedDuringRead,"reject changed stack");
    m = fixture(); m.put(0x3100,std::numeric_limits<float>::quiet_NaN());
    require(matrices(m,0x1000,s) == Status::InvalidMatrices,"reject nonfinite view");
    m = fixture(); require(matrices(m,UINT32_MAX-4,s) == Status::ReadFailed,"bound address overflow");
    std::cout << "RT Draw packet and bounded matrix observation: PASS; game ABI/queue/RT unrun\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
