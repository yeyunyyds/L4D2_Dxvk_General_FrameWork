#include "geometry.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <limits>
void require(bool ok,const char* text) { if(!ok) { throw std::runtime_error(text); } }
template<class T> void append(std::string& out,const T& v) { out.append(reinterpret_cast<const char*>(&v),sizeof(v)); }
std::string fixture() {
  std::string out="LDBRPLY1";for(uint32_t x:{1u,640u,400u,1u}) { append(out,x); }
  replay::Matrix identity{};for(unsigned i=0;i<4;++i) { identity[i*5]=1; }
  append(out,identity);append(out,identity);append(out,uint64_t(7));append(out,uint32_t(3));append(out,uint32_t(3));append(out,identity);
  const replay::Vertex vertices[]={{{0,0,0},{0,0},{0,0,0}},{{1,0,0},{1,0},{0,0,0}},{{0,1,0},{0,1},{0,0,0}}};
  append(out,vertices);for(uint32_t i=0;i<3;++i) { append(out,i); }return out;
}
int main(int argc,char** argv) {
  try {
    const auto bytes=fixture();std::istringstream in(bytes);const auto scene=replay::load(in);
    require(scene.parts.size()==1&&scene.parts[0].indices[2]==2,"valid scene failed");
    for(size_t size:{size_t(0),size_t(8),size_t(24),bytes.size()-1}) {
      bool rejected=false;try { std::istringstream x(bytes.substr(0,size));replay::load(x); }catch(const std::runtime_error&) { rejected=true; }
      require(rejected,"truncation accepted");
    }
    for(unsigned fault=0;fault<5;++fault) {
      auto changed=bytes;
      if(fault==0) { changed[0]='X'; }
      if(fault==1) { changed.push_back('x'); }
      if(fault==2) { const uint32_t index=3;std::memcpy(&changed[changed.size()-4],&index,4); }
      if(fault==3) { const float nan=std::numeric_limits<float>::quiet_NaN();std::memcpy(&changed[24],&nan,4); }
      if(fault==4) { const uint32_t count=65;std::memcpy(&changed[20],&count,4); }
      bool rejected=false;try { std::istringstream x(changed);replay::load(x); }catch(const std::runtime_error&) { rejected=true; }
      require(rejected,"malformed replay accepted");
    }
    const auto& v=scene.parts[0].vertices;const auto n=replay::normal(v[0],v[1],v[2]);
    require(n[0]==0&&n[1]==0&&n[2]==1&&!replay::degenerateUv(v[0],v[1],v[2]),"normal/UV orientation");
    auto degenerate=v[2];std::memcpy(degenerate.uv,v[0].uv,8);require(replay::degenerateUv(v[0],v[1],degenerate),"degenerate UV missed");
    std::cout<<"REPLAY_GEOMETRY_PASS: valid scene, nine malformed/truncated inputs, flat normal, UV degeneracy\n";
    for(int i=1;i<argc;++i) { std::ifstream input(argv[i],std::ios::binary);const auto real=replay::load(input);size_t vertices=0,triangles=0;
      for(const auto& p:real.parts) { vertices+=p.vertices.size();triangles+=p.indices.size()/3; }
      std::cout<<"CAPTURE_REPLAY_INPUT: parts="<<real.parts.size()<<" vertices="<<vertices<<" triangles="<<triangles<<'\n'; }
    return 0;
  }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
