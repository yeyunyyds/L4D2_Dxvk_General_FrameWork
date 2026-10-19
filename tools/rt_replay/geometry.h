#pragma once
// Private, bounded frozen-pose transport. No pointers, shader execution or game hooks.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <istream>
#include <stdexcept>
#include <vector>

namespace replay {
using Matrix = std::array<float,16>;
struct Vertex { float position[3], uv[2], unused[3]; };
static_assert(sizeof(Vertex)==32, "capture vertex ABI");
struct Part { uint64_t ordinal=0; Matrix world{}; std::vector<Vertex> vertices; std::vector<uint32_t> indices; };
struct Scene { uint32_t width=0,height=0; Matrix view{},projection{}; std::vector<Part> parts; };
inline void read(std::istream& in, void* data,size_t size) {
  if (!in.read(static_cast<char*>(data),std::streamsize(size))) { throw std::runtime_error("truncated replay input"); }
}
template<class T> T value(std::istream& in) { T x{};read(in,&x,sizeof(x));return x; }
inline void matrix(std::istream& in,Matrix& m) {
  read(in,m.data(),sizeof(m));for(float x:m) { if(!std::isfinite(x)||std::abs(x)>1e7f) { throw std::runtime_error("invalid matrix"); } }
}
inline Scene load(std::istream& in) {
  char magic[8]{};read(in,magic,8);if(std::memcmp(magic,"LDBRPLY1",8)) { throw std::runtime_error("wrong replay magic"); }
  if(value<uint32_t>(in)!=1) { throw std::runtime_error("wrong replay version"); }
  Scene s;s.width=value<uint32_t>(in);s.height=value<uint32_t>(in);const auto count=value<uint32_t>(in);
  if(s.width!=640||s.height!=400||!count||count>64) { throw std::runtime_error("replay limits"); }
  matrix(in,s.view);matrix(in,s.projection);uint64_t budget=0;
  for(uint32_t n=0;n<count;++n) {
    Part p;p.ordinal=value<uint64_t>(in);const auto vertices=value<uint32_t>(in),indices=value<uint32_t>(in);
    if(!vertices||vertices>32768||!indices||indices>393216||indices%3) { throw std::runtime_error("geometry limits"); }
    budget+=uint64_t(vertices)*32+uint64_t(indices)*4;if(budget>32*1024*1024) { throw std::runtime_error("scene byte budget"); }
    matrix(in,p.world);if(p.world[3]||p.world[7]||p.world[11]||p.world[15]!=1) { throw std::runtime_error("non-affine world"); }
    p.vertices.resize(vertices);p.indices.resize(indices);read(in,p.vertices.data(),vertices*sizeof(Vertex));read(in,p.indices.data(),indices*4);
    for(const auto& v:p.vertices) {
      for(float x:v.position) { if(!std::isfinite(x)||std::abs(x)>1e7f) { throw std::runtime_error("invalid position"); } }
      for(float x:v.uv) { if(!std::isfinite(x)||std::abs(x)>1e7f) { throw std::runtime_error("invalid uv"); } }
    }
    for(auto i:p.indices) { if(i>=vertices) { throw std::runtime_error("index outside vertices"); } }
    s.parts.push_back(std::move(p));
  }
  if(in.peek()!=std::char_traits<char>::eof()) { throw std::runtime_error("trailing replay bytes"); }return s;
}
inline std::array<float,3> normal(const Vertex& a,const Vertex& b,const Vertex& c) {
  double u[3]{},v[3]{};for(unsigned j=0;j<3;++j) { u[j]=double(b.position[j])-a.position[j];v[j]=double(c.position[j])-a.position[j]; }
  const double n[]={u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
  const double l=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
  if(l<1e-12) { return {0,0,1}; }return {float(n[0]/l),float(n[1]/l),float(n[2]/l)};
}
inline bool degenerateUv(const Vertex& a,const Vertex& b,const Vertex& c) {
  const double d=(double(a.uv[0])-c.uv[0])*(double(b.uv[1])-c.uv[1])-(double(a.uv[1])-c.uv[1])*(double(b.uv[0])-c.uv[0]);
  return !std::isfinite(d)||std::abs(d)<1e-8;
}
} // namespace replay
