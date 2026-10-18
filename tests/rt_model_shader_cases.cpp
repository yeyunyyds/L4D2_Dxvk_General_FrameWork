#include "rt_model_shader.h"
#include <iostream>
#include <stdexcept>
using namespace ldb_rt;
void check(bool value,const char* why) { if (!value) { throw std::runtime_error(why); } }
int main() {
  try {
    sky_shader::Constants c{};c[0][1]=1;c[58]={0,2,0,123};c[59]={-3,0,0,10};c[60]={0,0,.5f,-20};c[48]={2,-1,0,.25f};c[49]={1,3,0,-.5f};
    std::vector<uint8_t> bytes(3*32);
    for (unsigned i=0;i<3;++i) {
      const float pos[]={float(i),2.f,-10.f};const float uv[]={.2f,.3f};const int16_t weights[]={32767,-1};
      std::memcpy(bytes.data()+i*32,pos,12);std::memcpy(bytes.data()+i*32+12,weights,4);std::memcpy(bytes.data()+i*32+24,uv,8);
    }
    const auto bone=model_shader::reconstruct(model_shader::Profile::Bone0,c,bytes,32,0,24,12,16);
    check(bone.valid && bone.world[12]==123 && bone.world[1]==-3 && bone.world[4]==2,"model matrix transpose/placement");
    float uv[2]{};std::memcpy(uv,bone.vertices.data()+12,8);check(std::abs(uv[0]-.35f)<1e-6 && std::abs(uv[1]-.6f)<1e-6,"model original UV transform");
    for (unsigned fault=0;fault<8;++fault) {
      auto changed=bytes;auto constants=c;
      if (fault==0) { int16_t w=32766;std::memcpy(changed.data()+2*32+12,&w,2); }
      if (fault==1) { changed[2*32+16]=1; }
      if (fault==2) { constants[13][0]=.1f; }
      if (fault==3) { constants[0][1]=2; }
      if (fault==4) { constants[60][2]=-.5f; }
      if (fault==5) { constants[58][3]=NAN; }
      if (fault==6) { float v=NAN;std::memcpy(changed.data(),&v,4); }
      if (fault==7) { changed.pop_back(); }
      check(!model_shader::reconstruct(model_shader::Profile::Bone0,constants,changed,32,0,24,12,16).valid,"unsupported model accepted");
    }
    check(!model_shader::reconstruct(model_shader::Profile::Rigid,c,bytes,32,0,24,12,16).valid,"bone layout accepted as unskinned");
    check(!model_shader::reconstruct(model_shader::Profile::Unknown,c,bytes,32,0,24,-1,-1).valid,"unknown program accepted");
    check(model_shader::profile(std::string(64,'0'))==model_shader::Profile::Unknown,"unknown digest accepted");
    auto rigid=model_shader::reconstruct(model_shader::Profile::Rigid,c,bytes,32,0,24,-1,-1);check(rigid.valid,"rigid transform rejected");
    std::array<float,16> view{},projection{};for (unsigned i=0;i<4;++i) { view[i*5]=projection[i*5]=1;c[8+i][i]=1; }
    check(model_shader::cameraCompatible(rigid,c,view,projection),"matching model camera rejected");c[8][3]=100;
    check(!model_shader::cameraCompatible(rigid,c,view,projection),"foreign model camera accepted");
    std::cout<<"RT audited model matrix/UV/bone0/range/rejection: PASS\n";return 0;
  } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
