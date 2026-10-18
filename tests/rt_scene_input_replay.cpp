// CPU regression runner; captured shader bytes stay outside the repository.
#include "rt_model_shader.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <iomanip>
using namespace ldb_rt;
struct Reader {
  std::ifstream f;explicit Reader(const char* path):f(path,std::ios::binary) { if (!f) { throw std::runtime_error("input missing"); } }
  void read(void* p,size_t n) { if (n>1024*1024 || (n && !f.read(static_cast<char*>(p),std::streamsize(n)))) { throw std::runtime_error("input truncated/oversized"); } }
  uint32_t u32() { uint32_t n=0;read(&n,4);return n; }
  template<class T> std::vector<T> vector(size_t max) { const auto n=u32();if (n>max) { throw std::runtime_error("input budget"); }std::vector<T> v(n);read(v.data(),v.size()*sizeof(T));return v; }
};
int main(int argc,char** argv) {
  try {
    if (argc!=2) { return 1; }Reader r(argv[1]);if (r.u32()!=0x3152534cu) { throw std::runtime_error("input magic"); }
    const auto count=r.u32();if (count>96) { return 1; }std::cout<<std::setprecision(12)<<"[";
    for (uint32_t sample=0;sample<count;++sample) {
      const auto ordinal=r.u32(),kind=r.u32(),stride=r.u32(),pos=r.u32(),uv=r.u32();const int weight=int32_t(r.u32()),indices=int32_t(r.u32());
      std::array<char,64> hash{};r.read(hash.data(),hash.size());sky_shader::Constants c{};r.read(c.data(),sizeof(c));std::array<float,16> view{},projection{};r.read(view.data(),64);r.read(projection.data(),64);
      auto vs=r.vector<uint32_t>(16384),ps=r.vector<uint32_t>(16384);auto bytes=r.vector<uint8_t>(4096*128);
      if (stride!=32 || pos>stride-12 || uv>stride-8 || bytes.empty() || bytes.size()%stride) { throw std::runtime_error("invalid vertex layout"); }
      bool valid=false;const char* reason="invalid_kind";size_t vertices=0;unsigned front=0,inside=0;double maxUV=0;
      if (kind==0) {
        std::vector<sky_shader::Vec> p(bytes.size()/stride),t(p.size());
        for (size_t i=0;i<p.size();++i) { p[i]={0,0,0,1};t[i]={0,0,0,1};std::memcpy(p[i].data(),bytes.data()+i*stride+pos,12);std::memcpy(t[i].data(),bytes.data()+i*stride+uv,8); }
        auto result=sky_shader::rebuild(sky_shader::parse(vs),sky_shader::pixelInterface(ps),c,p,t,view,projection);valid=result.valid;reason=result.reason;vertices=result.vertices.size();
        // Independent expression for NekoSky's four-neighbor sample coordinates.
        if (valid) { for (size_t i=0;i<t.size();++i) {
          const float q[3]={t[i][0]*c[0][1]+c[0][0],t[i][1]*c[0][1]+c[0][0],t[i][0]*c[0][0]+c[0][1]};
          const float a=q[0]*c[49][0]+q[1]*c[49][1]+q[2]*c[49][3],b=q[0]*c[50][0]+q[1]*c[50][1]+q[2]*c[50][3];
          const float x=a+c[48][0],y=a-c[48][0],z=b-c[48][1],w=b+c[48][1];
          const float expected[5][2]={{y,z},{y,w},{x,z},{x,w},{y*c[48][2],z*c[48][3]}};
          for (unsigned j=0;j<5;++j) { for (unsigned k=0;k<2;++k) { maxUV=std::max(maxUV,std::abs(double(result.vertices[i][3+j*4+k])-expected[j][k])); } }
        } }
      } else {
        auto result=model_shader::reconstruct(model_shader::profile(std::string(hash.data(),hash.size())),c,bytes,stride,pos,uv,weight,indices);
        valid=result.valid && model_shader::cameraCompatible(result,c,view,projection);reason=valid?result.reason:result.valid?"model_clip_camera_mismatch":result.reason;vertices=result.vertices.size()/32;
        if (valid) { for (size_t at=0;at<result.vertices.size();at+=32) {
          sky_shader::Vec local{0,0,0,1};std::memcpy(local.data(),result.vertices.data()+at,12);double world[4]={0,0,0,1},v[4]{},clip[4]{};
          for (unsigned j=0;j<3;++j) { for (unsigned i=0;i<4;++i) { world[j]+=double(local[i])*result.world[i*4+j]; } }
          for (unsigned j=0;j<4;++j) { for (unsigned i=0;i<4;++i) { v[j]+=world[i]*view[i*4+j]; } }
          for (unsigned j=0;j<4;++j) { for (unsigned i=0;i<4;++i) { clip[j]+=v[i]*projection[i*4+j]; } }
          if (clip[3]>0) { ++front;if (std::abs(clip[0])<=clip[3] && std::abs(clip[1])<=clip[3] && clip[2]>=0 && clip[2]<=clip[3]) { ++inside; } }
        } }
      }
      if (sample) { std::cout<<","; }std::cout<<"{\"ordinal\":"<<ordinal<<",\"kind\":"<<kind<<",\"valid\":"<<(valid?"true":"false")<<",\"reason\":\""<<reason<<"\",\"vertices\":"<<vertices<<",\"front\":"<<front<<",\"inside_clip\":"<<inside<<",\"max_uv_error\":"<<maxUV<<"}";
    }
    std::cout<<"]\n";return 0;
  } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
