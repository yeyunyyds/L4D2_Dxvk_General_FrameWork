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
  uint32_t changeCell = 0;
  bool read(uint32_t address, void* out, size_t bytes) {
    if (++reads == failAt || address > data.size() || bytes > data.size() - address) { return false; }
    std::memcpy(out, data.data() + address, bytes);
    if (changeCell && address == changeCell && bytes == 4) {
      const uint32_t other = 0x7600; std::memcpy(data.data() + address, &other, 4);
    }
    if (changeTop && reads == 12) { const uint32_t changed = 1; std::memcpy(data.data() + 0x201c, &changed, 4); }
    return true;
  }
  bool code(uint32_t address, void* out, size_t bytes) { return read(address, out, bytes); }
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
    auto malformed = bytes; malformed[4] = 4; require(!unpack(malformed.data(),malformed.size(),copy),"reject version");
    auto version2=bytes;version2.resize(kSnapshotV2Bytes);version2[4]=2;
    require(unpack(version2.data(),version2.size(),copy)&&copy.materialInfo.flags==s.materialInfo.flags,"preserve version2 trace compatibility");
    auto legacy = bytes; legacy.resize(kSnapshotV1Bytes); legacy[4] = 1;
    require(unpack(legacy.data(), legacy.size(), copy) && copy.materialInfo.flags == 0, "preserve version1 trace compatibility");
    {
      const auto bones=std::vector<model_scope::Matrix3x4>(3,model_scope::Matrix3x4{1,0,0,4,0,1,0,5,0,0,1,6});
      s.modelScope=7;s.modelResource=8;s.modelInstance=9;s.modelSurface=0;s.modelBoneCount=3;s.modelEntity=11;
      s.modelFlags=kModelPresent|kModelFirstSurface;s.modelBones=bones;const auto modelBytes=pack(s);
      require(modelBytes.size()==kSnapshotBytes+3*sizeof(model_scope::Matrix3x4)&&unpack(modelBytes.data(),modelBytes.size(),copy),"model snapshot rejected");
      require(copy.modelResource==8&&copy.modelInstance==9&&copy.modelBones==bones,"model snapshot changed");
      auto bad=modelBytes;bad.back()=0x7f;bad[bad.size()-2]=0xff;require(!unpack(bad.data(),bad.size(),copy),"nonfinite palette accepted");
    }
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
    {
      Memory data;
      data.put(0x4000, uint32_t(0x5000)); data.put(0x4038, uint64_t(0x480003)); data.put(0x4014, uint32_t(0x7000));
      data.put(0x5050, uint32_t(0x6000)); data.put(0x50c4, uint32_t(0x6100));
      const uint8_t format[] = {0x8b,0x41,0x38,0x8b,0x51,0x3c,0xc3};
      const uint8_t getter[] = {0x83,0x79,0x14,0x00,0x74,0x09,0x8b,0x49,0x14,0x8b,0x01,0x8b,0x10,0xff,0xe2};
      std::memcpy(data.data.data()+0x6000,format,sizeof(format)); std::memcpy(data.data.data()+0x6100,getter,sizeof(getter));
      data.put(0x7000, uint32_t(0x7200)); data.put(0x7200, uint32_t(0x7300));
      data.data[0x7300]=0xb8; data.put(0x7301,uint32_t(0x7400)); data.data[0x7305]=0xc3;
      std::memcpy(data.data.data()+0x7400,"LightmappedGeneric_DX9",22);
      const auto decoded = material_decode::decode(data,0x4000,0x5000);
      require(decoded.flags==3 && decoded.format==0x480003 && material_decode::worldShader(decoded.shader),"bounded shader literal decoder");
      const uint8_t fieldBody[]={0x8b,0x41,0x04,0xc3,0,0,0,0};
      std::memcpy(data.data.data()+0x7300,fieldBody,sizeof(fieldBody));data.put(0x7004,uint32_t(0x7400));
      require(material_decode::decode(data,0x4000,0x5000).flags==3,"bounded shader field decoder");
      data.data[0x7300]=0xa1;data.put(0x7301,uint32_t(0x7500));data.data[0x7305]=0xc3;data.put(0x7500,uint32_t(0x7400));
      const auto global=material_decode::decode(data,0x4000,0x5000);
      require(global.flags==3 && global.nameSlot==0x7500 && global.nameAddress==0x7400 &&
        material_decode::worldShader(global.shader),"A1 global pointer leaf resolves bounded world name");
      data.changeCell=0x7500;
      require(material_decode::decode(data,0x4000,0x5000).flags==0,"changed global name pointer must fail closed");
      data.changeCell=0;data.put(0x7500,uint32_t(0x7400));
      data.put(0x7301,uint32_t(UINT32_MAX-1));
      require(material_decode::decode(data,0x4000,0x5000).flags==1,"global name pointer read cannot wrap address space");
      data.put(0x7301,uint32_t(0x7500));data.put(0x7500,uint32_t(0xfff0));
      require(material_decode::decode(data,0x4000,0x5000).flags==1,"empty global name is never a classified world");
      data.put(0x7500,uint32_t(0x7400));std::memset(data.data.data()+0x7400,0,64);
      std::memcpy(data.data.data()+0x7400,"NekoToon",9);
      const auto custom=material_decode::decode(data,0x4000,0x5000);
      require(custom.flags==3 && !material_decode::worldShader(custom.shader),"global custom shader must not bypass world whitelist");
      data.data[0x7300]=0xe8;
      require(material_decode::decode(data,0x4000,0x5000).flags==1,"never execute or guess a call instruction");
      data.data[0x7300]=0xb8;data.put(0x7301,uint32_t(0x7400));data.data[0x7305]=0xc3;std::memset(data.data.data()+0x7400,'A',64);
      const auto tooLong=material_decode::decode(data,0x4000,0x5000);
      require(tooLong.flags==1 && tooLong.shader[0]==0,"reject truncated classification string");
      require(material_decode::decode(data,UINT32_MAX-4,0x5000).flags==0,"bound material address overflow");
    }
    std::cout << "RT bounded A1 shader-name pointer reread: PASS; private functions never called\n";
    std::cout << "RT Draw packet and bounded matrix observation: PASS; game ABI/queue/RT unrun\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
