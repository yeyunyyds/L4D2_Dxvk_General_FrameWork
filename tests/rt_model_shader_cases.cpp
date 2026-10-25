#include "rt_model_shader.h"
#include <iostream>
#include <stdexcept>
using namespace ldb_rt;
void check(bool value,const char* why) { if (!value) { throw std::runtime_error(why); } }
int main() {
  try {
    {
      sky_shader::Constants c{};c[0][1]=1;c[58]={0,1,0,123};c[59]={-1,0,0,-25};c[60]={0,0,1,8};
      c[48]={7,3,0,9}; // Infected baseUV must not use the older model UV transform.
      std::vector<uint8_t> input(2*48);const float v[]={1,2,3,0,3,4,.2f,.7f,1,0,0,1};
      std::memcpy(input.data(),v,48);std::memcpy(input.data()+48,v,48);std::memset(input.data()+48+12,0,12);
      auto r=model_shader::infected(c,input);check(r.valid&&r.vertices.size()==64&&r.world[12]==123&&r.world[1]==-1,"infected c58 world transform/canonical stride");
      float out[8]{};std::memcpy(out,r.vertices.data(),32);check(out[0]==1&&std::abs(out[3]-.2f)<1e-6&&std::abs(out[4]-.7f)<1e-6&&std::abs(out[6]-.6f)<1e-6&&std::abs(out[7]-.8f)<1e-6,"infected position/baseUV/smooth normal");
      std::memcpy(out,r.vertices.data()+32,32);check(out[5]==0&&out[6]==0&&out[7]==0,"invalid source normal not marked for fallback");
      check(model_shader::profile("e9f1650bedd1575e46c486f522d3d0bb8a40e1abb8d78eb0d281daf93a267d72")==model_shader::Profile::InfectedRigid,"infected full-program gate missing");
      c[58][1]=2;check(!model_shader::infected(c,input).valid,"infected nonrigid normal transform admitted");c[58][1]=1;
      const float bad=NAN;std::memcpy(input.data()+12,&bad,4);check(!model_shader::infected(c,input).valid,"infected nonfinite normal admitted");
      std::cout<<"RT Infected rigid48 exact program/c58/baseUV/source normals/invalid fallback: PASS\n";
    }
    for(const auto* sha:{"b9081b1aa48487b133f168edcf34a302ad06d9963e1bedab8d9dd62d98fbc606","803be4995dac57ebc289dbcc97004a6b1f4c8ad616c7db8dc39f41c200663706","1028c5ef031a8f9763c878ac9514dba6e628f87d3746ec373f4fc3b3e4486775"}) { check(model_shader::profile(sha)==model_shader::Profile::Rigid,"v21 rigid program missing"); }
    for(const auto* sha:{"d7f9daf0698882776cffeb478a6bfdf2a480ade55a7bc659bd76b36c589d5174","3c653c42a2ce6a7b2201a5b26af6cee4b501625b2d32f1063caafa78d9261d2b"}) { check(model_shader::profile(sha)==model_shader::Profile::Skin3,"v21 skin program missing"); }
    check(model_shader::profile("b9081b1aa48487b133f168edcf34a302ad06d9963e1bedab8d9dd62d98fbc607")==model_shader::Profile::Unknown,"unknown adjacent program admitted");
    check(model_shader::profile("7aab351541ae0799d92e5a2c11fee647df4a5b3319037e20ea3721093de9fc33")==model_shader::Profile::Rigid&&model_shader::profile("6812e9511ea1734cb3a529f9cd30b234f60bb9262a5dc35c55bec26636889210")==model_shader::Profile::Skin3,"163633 audited programs missing");
    std::cout<<"RT two 163633 VertexLitGeneric exact program profiles: PASS\n";
    std::cout<<"RT five 141352 VertexLitGeneric exact program profiles: PASS\n";
    check(model_shader::profile("21a836464d57307495a547b937729a9a197b4a210864605bf7acdb16aa03931b")==model_shader::Profile::Skin3,"204426 audited skin program missing");
    std::cout<<"RT 204426 three-weight VertexLitGeneric exact program profile: PASS\n";
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
    check(model_shader::profile("3566b4c8bed75da732f7c8cdb1bce1bb1b5025c932916b2d739ec748f9b8526b")==model_shader::Profile::Rigid,"new audited rigid shader missing");
    std::vector<uint8_t> large;large.reserve(model_shader::kMaxVertices*32);
    for (unsigned i=0;i<8951;++i) { large.insert(large.end(),bytes.begin(),bytes.begin()+32); }
    check(model_shader::reconstruct(model_shader::Profile::Rigid,c,large,32,0,24,-1,-1).valid,"captured-size rigid model rejected by old budget");
    large.resize((model_shader::kMaxVertices+1)*32);
    check(!model_shader::reconstruct(model_shader::Profile::Rigid,c,large,32,0,24,-1,-1).valid,"model geometry allocation cap missing");
    // Three distinct palette transforms and exact packed weights exercise the
    // third weight, byte channel order, world-space output and every vertex.
    auto palette=c;palette[8][3]=0;
    for (unsigned k=0;k<3;++k) {
      palette[58+k*3]={1,0,0,float(k*10)};palette[59+k*3]={0,1,0,float(k*20)};palette[60+k*3]={0,0,1,float(k*30)};
    }
    auto skinned=bytes;
    const int16_t weights[]={8191,16383}; // 1/4,1/2,1/4
    const uint8_t bones[]={2,0,1,255}; // VS D3DCOLOR.zyx reads these first3 bytes
    for (unsigned i=0;i<3;++i) { std::memcpy(skinned.data()+i*32+12,weights,4);std::memcpy(skinned.data()+i*32+16,bones,4); }
    for (auto p:{model_shader::Profile::Skin3,model_shader::Profile::Neko3}) {
      auto rebuilt=model_shader::reconstruct(p,palette,skinned,32,0,24,12,16);
      check(rebuilt.valid && rebuilt.world[0]==1 && rebuilt.world[12]==0,"skinning must emit world positions exactly once");
      for (unsigned i=0;i<3;++i) {
        float v[3]{};std::memcpy(v,rebuilt.vertices.data()+i*32,12);
        check(v[0]==float(i)+7.5f && v[1]==17 && v[2]==12.5f,"three-bone matrix blend/channel order wrong");
      }
      auto morph=std::vector<sky_shader::Vec>{{1,2,3,999},{4,5,6,999},{7,8,9,999}};
      auto mc=palette;mc[13][0]=.5f;
      check(!model_shader::reconstruct(p,mc,skinned,32,0,24,12,16).valid,"missing morph silently ignored");
      rebuilt=model_shader::reconstruct(p,mc,skinned,32,0,24,12,16,morph);
      float v[3]{};std::memcpy(v,rebuilt.vertices.data()+64,12);
      check(rebuilt.valid && v[0]==13 && v[1]==21 && v[2]==17,"per-vertex morph before skinning wrong");
      morph.back()[2]=NAN;
      check(!model_shader::reconstruct(p,mc,skinned,32,0,24,12,16,morph).valid,"nonfinite final morph vertex accepted");
      for (unsigned fault=0;fault<4;++fault) {
        auto bad=skinned;auto pc=palette;
        if (fault==0) { const int16_t invalid[]={32767,0};std::memcpy(bad.data()+64+12,invalid,4); }
        if (fault==1) { bad[64+16]=66; }
        if (fault==2) { pc[58+2*3][0]=NAN; }
        if (fault==3) { pc[0][1]=2; }
        check(!model_shader::reconstruct(p,pc,bad,32,0,24,12,16).valid,"invalid skin palette/weight/control accepted");
      }
    }
    auto zeroWeight=skinned;const int16_t rigidWeights[]={32767,-1};const uint8_t threeAddresses[]={0,1,2,255};
    for(unsigned i=0;i<3;++i) { std::memcpy(zeroWeight.data()+i*32+12,rigidWeights,4);std::memcpy(zeroWeight.data()+i*32+16,threeAddresses,4); }
    auto zeroWeightPalette=palette;zeroWeightPalette[61][0]=NAN;
    check(!model_shader::reconstruct(model_shader::Profile::Skin3,zeroWeightPalette,zeroWeight,32,0,24,12,16).valid,
      "zero-weight shader matrix address escaped finite validation");
    palette[12][3]=.01f;
    check(!model_shader::reconstruct(model_shader::Profile::Neko3,palette,skinned,32,0,24,12,16).valid,"Neko clip depth bias silently lost");
    // 004208 VP coefficients: direct per-vertex relative clip testing is
    // unstable at large world coordinates; matrix identity is camera proof.
    view={-.840787113f,.174497947f,-.512471974f,0,-.541365921f,-.271010071f,.795912325f,0,-1.97363077e-17f,.946627676f,.322329015f,0,-3987.125f,-1319.37939f,-360.735565f,1};
    projection={.737271011f,0,0,0,0,1.17963362f,0,0,0,0,-1.00000703f,-1,0,0,-7.00004911f,0};
    for (unsigned j=0;j<4;++j) { for (unsigned i=0;i<4;++i) { double v=0;for (unsigned k=0;k<4;++k) { v+=double(view[i*4+k])*projection[k*4+j]; }c[8+j][i]=float(v); } }
    check(model_shader::cameraCompatible(rigid,c,view,projection),"actual camera coefficient proof failed");
    for (unsigned j=0;j<4;++j) { for (unsigned i=0;i<4;++i) {
      auto foreign=c;foreign[8+j][i]+=.01f;
      check(!model_shader::cameraCompatible(rigid,foreign,view,projection),"foreign coefficient accepted as main camera");
    } }
    c[8][0]=NAN;check(!model_shader::cameraCompatible(rigid,c,view,projection),"nonfinite camera accepted");
    check(model_shader::profile("f29d14d0f408c16686dbba5b391c48a8e7445de0d47162b7118d0baaca5519a3")==model_shader::Profile::Neko3,"audited Neko VS missing");
    std::cout<<"RT model matrix proof/foreign camera/three-weight skin/morph/rejections: PASS\n";
    std::cout<<"RT larger audited rigid model/bounded allocation: PASS\n";
    std::cout<<"RT audited model matrix/UV/bone0/range/rejection: PASS\n";return 0;
  } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
