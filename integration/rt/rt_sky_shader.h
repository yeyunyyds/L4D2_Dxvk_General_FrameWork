#pragma once
// Bounded straight-line SM2/3 VS replay. Unknown instructions, relative access,
// flow control and undefined input reads reject the entire reconstruction.
#include <array>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace ldb_rt::sky_shader {
using Vec=std::array<float,4>;
using Constants=std::array<Vec,256>;
inline unsigned type(uint32_t t) { return ((t>>28)&7)|((t>>8)&24); }
inline unsigned index(uint32_t t) { return t&2047; }
inline bool token(uint32_t t) { return (t&0x80000000u) && !(t&0x2000u); }
struct Instruction { unsigned op=0;uint32_t dest=0;std::array<uint32_t,3> src{}; };
struct Program {
  std::vector<Instruction> instructions;
  std::array<unsigned,16> inputs{}; // 1 POSITION0, 2 TEXCOORD0; other inputs unavailable
  Constants literals{};std::array<bool,256> literal{};
  unsigned positionType=4,positionIndex=0,uvIndex=0;
  bool modern=false,valid=false;
  const char* reason="invalid_shader";
};
inline Program parse(const std::vector<uint32_t>& words) {
  Program p;
  if (words.empty() || words.size()>16384 || (words[0]!=0xfffe0200u && words[0]!=0xfffe0300u)) { return p; }
  p.modern=words[0]==0xfffe0300u;p.positionType=p.modern?6u:4u;
  bool pos=!p.modern,uv=!p.modern,ended=false;
  for (size_t at=1;at<words.size();) {
    const uint32_t code=words[at++];const unsigned op=code&65535, len=(code>>24)&15;
    if (op==65535) { ended=at==words.size();break; }
    if (op==65534) { const size_t n=(code>>16)&32767;if (n>words.size()-at) { return p; }at+=n;continue; }
    if (code&0x50000000u || len>words.size()-at || p.instructions.size()>=512) { return p; }
    const auto* a=words.data()+at;at+=len;
    if (op==0 && len==0) { continue; }
    if (op==31 && len==2 && token(a[1])) {
      const unsigned usage=a[0]&15,usageIndex=(a[0]>>16)&15,reg=index(a[1]);
      if (type(a[1])==1 && reg<16) { p.inputs[reg]=usage==0 && !usageIndex?1u:usage==5 && !usageIndex?2u:0u; }
      else if (p.modern && type(a[1])==6 && reg<12) {
        if (usage==0 && !usageIndex) { p.positionIndex=reg;pos=true; }
        else if (usage==5 && !usageIndex) { p.uvIndex=reg;uv=true; }
        else { p.reason="extra_vs_output";return p; }
      } else { return p; }
      continue;
    }
    if (op==81 && len==5 && token(a[0]) && type(a[0])==2 && index(a[0])<256) {
      const auto reg=index(a[0]);std::memcpy(p.literals[reg].data(),a+1,16);p.literal[reg]=true;continue;
    }
    unsigned sources=0;
    switch (op) {
      case 1:case 6:case 35:sources=1;break;
      case 2:case 5:case 8:case 9:case 10:case 11:case 20:sources=2;break;
      case 4:sources=3;break;
      default:p.reason="unsupported_opcode";return p;
    }
    if (len!=sources+1 || !token(a[0]) || ((a[0]>>20)&15)>1 || ((a[0]>>24)&15)) { return p; }
    const unsigned dt=type(a[0]),di=index(a[0]);
    if (!((dt==0 && di<32) || (dt==4 && !p.modern && di<3) || (dt==5 && !p.modern && di<2) || (dt==6 && di<12))) { p.reason="unsupported_destination";return p; }
    Instruction ins;ins.op=op;ins.dest=a[0];
    for (unsigned j=0;j<sources;++j) {
      const auto t=a[j+1];const unsigned st=type(t),si=index(t),modifier=(t>>24)&15;
      if (!token(t) || (modifier>1 && modifier!=11 && modifier!=12) || !((st==0 && si<32)||(st==1 && si<16)||(st==2 && si<256))) { p.reason="unsupported_source";return p; }
      ins.src[j]=t;
    }
    if (op==20 && (type(ins.src[1])!=2 || index(ins.src[1])>252 || ((ins.src[1]>>16)&255)!=228 || ((ins.src[1]>>24)&15))) { return p; }
    p.instructions.push_back(ins);
  }
  p.valid=ended && pos && uv && !p.instructions.empty();if (p.valid) { p.reason="supported"; }return p;
}
// The FFP replacement supplies only TEXCOORD0 to the retained PS. Reject other
// declared interpolants and PS1. DCL samplers and literal definitions are allowed.
inline bool pixelInputs(const std::vector<uint32_t>& words) {
  if (words.empty() || (words[0]!=0xffff0200u && words[0]!=0xffff0300u)) { return false; }
  for (size_t at=1;at<words.size();) {
    const uint32_t code=words[at++];const unsigned op=code&65535,len=(code>>24)&15;
    if (op==65535) { return at==words.size(); }
    if (op==65534) { const size_t n=(code>>16)&32767;if (n>words.size()-at) { return false; }at+=n;continue; }
    if (len>words.size()-at) { return false; }
    if (op==31) {
      if (len!=2 || !token(words[at+1])) { return false; }
      const auto dest=words[at+1];
      if (type(dest)!=10) {
        if (((dest>>16)&15)&~3u) { return false; }
        if (words[0]==0xffff0200u) { if (type(dest)!=3 || index(dest)!=0) { return false; } }
        else if (type(dest)!=1 || (words[at]&15)!=5 || ((words[at]>>16)&15)) { return false; }
      }
    }
    at+=len;
  }
  return false;
}
struct Register { Vec v{};unsigned mask=0; };
inline bool evaluate(const Program& p,const Constants& constants,const Vec& position,const Vec& texcoord,Vec& clip,Vec& uv) {
  if (!p.valid) { return false; }
  std::array<Register,32> temp{};std::array<Register,16> input{};std::array<Register,12> output{};std::array<Register,3> raster{};std::array<Register,2> color{};
  for (unsigned i=0;i<16;++i) { if (p.inputs[i]) { input[i]={p.inputs[i]==1?position:texcoord,15}; } }
  const auto source=[&](uint32_t t,unsigned channel,float& value) {
    const unsigned reg=index(t),component=(t>>(16+channel*2))&3,kind=type(t);
    if (kind==2) { value=p.literal[reg]?p.literals[reg][component]:constants[reg][component]; }
    else { const auto& r=kind==0?temp[reg]:input[reg];if (!(r.mask&(1u<<component))) { return false; }value=r.v[component]; }
    const unsigned modifier=(t>>24)&15;if (modifier==11 || modifier==12) { value=std::abs(value); }
    if (modifier==1 || modifier==12) { value=-value; }return std::isfinite(value);
  };
  for (const auto& ins:p.instructions) {
    const unsigned mask=(ins.dest>>16)&15;Vec v{};
    for (unsigned c=0;c<4;++c) {
      if (!(mask&(1u<<c))) { continue; }
      float a=0,b=0,d=0,value=0;
      if (ins.op==8 || ins.op==9 || ins.op==20) {
        const unsigned n=ins.op==8?3u:4u;uint32_t right=ins.src[1];if (ins.op==20) { right+=c; }
        for (unsigned j=0;j<n;++j) { if (!source(ins.src[0],j,a)||!source(right,j,b)) { return false; }value+=a*b; }
      } else if (ins.op==6) { if (!source(ins.src[0],0,a) || std::abs(a)<1e-20f) { return false; }value=1/a; }
      else {
        if (!source(ins.src[0],c,a)) { return false; }
        if (ins.op==1) { value=a; }
        else if (ins.op==35) { value=std::abs(a); }
        else {
          if (!source(ins.src[1],c,b)) { return false; }
          if (ins.op==2) { value=a+b; }else if (ins.op==5) { value=a*b; }
          else if (ins.op==10) { value=std::min(a,b); }else if (ins.op==11) { value=std::max(a,b); }
          else { if (!source(ins.src[2],c,d)) { return false; }value=a*b+d; }
        }
      }
      if (!std::isfinite(value)) { return false; }v[c]=((ins.dest>>20)&1)?std::clamp(value,0.f,1.f):value;
    }
    auto& dest=type(ins.dest)==0?temp[index(ins.dest)]:type(ins.dest)==4?raster[index(ins.dest)]:type(ins.dest)==5?color[index(ins.dest)]:output[index(ins.dest)];
    for (unsigned c=0;c<4;++c) { if (mask&(1u<<c)) { dest.v[c]=v[c]; } }dest.mask|=mask;
  }
  const auto& result=p.modern?output[p.positionIndex]:raster[0];const auto& tex=output[p.uvIndex];
  if (result.mask!=15 || (tex.mask&3)!=3) { return false; }clip=result.v;uv=tex.v;return true;
}
inline bool inverseViewProjection(const std::array<float,16>& view,const std::array<float,16>& projection,std::array<double,16>& inverse) {
  double a[4][8]{};
  for (unsigned i=0;i<4;++i) { for (unsigned j=0;j<4;++j) { for (unsigned k=0;k<4;++k) { a[i][j]+=double(view[i*4+k])*projection[k*4+j]; } }a[i][i+4]=1; }
  for (unsigned i=0;i<4;++i) {
    unsigned pivot=i;for (unsigned j=i+1;j<4;++j) { if (std::abs(a[j][i])>std::abs(a[pivot][i])) { pivot=j; } }
    if (!std::isfinite(a[pivot][i]) || std::abs(a[pivot][i])<1e-12) { return false; }
    for (unsigned j=0;j<8;++j) { std::swap(a[i][j],a[pivot][j]); }
    const double divisor=a[i][i];for (double& v:a[i]) { v/=divisor; }
    for (unsigned j=0;j<4;++j) { if (j!=i) { const double scale=a[j][i];for (unsigned k=0;k<8;++k) { a[j][k]-=scale*a[i][k]; } } }
  }
  for (unsigned i=0;i<4;++i) { for (unsigned j=0;j<4;++j) { inverse[i*4+j]=a[i][j+4]; } }return true;
}
inline bool unproject(const Vec& clip,const std::array<double,16>& inverse,Vec& world,double& w) {
  std::array<double,4> result{};for (unsigned j=0;j<4;++j) { for (unsigned i=0;i<4;++i) { result[j]+=double(clip[i])*inverse[i*4+j]; } }
  w=result[3];if (!std::isfinite(w) || w<=1e-8) { return false; }
  for (unsigned j=0;j<3;++j) { const auto v=result[j]/w;if (!std::isfinite(v)||std::abs(v)>1e8) { return false; }world[j]=float(v); }world[3]=1;return true;
}
} // namespace ldb_rt::sky_shader
