#pragma once
#include <array>
#include <string>
#include <utility>
#include <vector>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <limits>
namespace ldb_camera {
using Matrix=std::array<float,16>;
// Walk bytecode comments only; all CTAB offsets are bounded within that comment.
inline int viewProjectionRegister(const std::vector<uint8_t>& code) {
  if(code.size()<8||code.size()%4)return -1;
  auto word=[&](size_t offset){uint32_t n=0;std::memcpy(&n,code.data()+offset,4);return n;};
  const uint32_t version=word(0);if((version>>16)!=0xfffe||((version>>8)&255)<2||((version>>8)&255)>3)return -1;
  size_t i=4;
  while(i+4<=code.size()) {
    uint32_t op=word(i);i+=4;if((op&65535)==65535)break;
    size_t bytes=((op&65535)==65534?((op>>16)&32767):((op>>24)&15))*4;
    if(bytes>code.size()-i)return -1;
    if((op&65535)==65534&&bytes>=32&&word(i)==0x42415443) {
      const uint8_t* body=code.data()+i+4;size_t size=bytes-4;
      auto read=[&](size_t at){uint32_t n=0;if(at+4<=size)std::memcpy(&n,body+at,4);return n;};
      uint32_t count=read(12),start=read(16);if(count>256||start>size||size-start<size_t(count)*20)return -1;
      for(uint32_t n=0;n<count;n++) {
        size_t at=start+size_t(n)*20;uint32_t name=read(at),sets=read(at+4),counts=read(at+8);
        constexpr char target[]="cViewProj";
        if(name<=size&&size-name>=sizeof(target)&&std::memcmp(body+name,target,sizeof(target))==0&&
          (sets&65535)==2&&(counts&65535)==4&&((sets>>16)+4)<=256)return int(sets>>16);
      }
    }
    i+=bytes;
  }
  return -1;
}
inline bool inverse(const Matrix& input,std::array<double,16>& result) {
  double a[4][8]{};
  for(int r=0;r<4;r++)for(int col=0;col<4;col++){if(!std::isfinite(input[size_t(r)*4+col]))return false;a[r][col]=input[size_t(r)*4+col];a[r][col+4]=(r==col);}
  for(int col=0;col<4;col++) {
    int pivot=col;for(int r=col+1;r<4;r++)if(std::abs(a[r][col])>std::abs(a[pivot][col]))pivot=r;
    if(std::abs(a[pivot][col])<1e-12)return false;
    for(int k=0;k<8;k++)std::swap(a[col][k],a[pivot][k]);
    double d=a[col][col];for(int k=0;k<8;k++)a[col][k]/=d;
    for(int r=0;r<4;r++)if(r!=col){d=a[r][col];for(int k=0;k<8;k++)a[r][k]-=d*a[col][k];}
  }
  for(int r=0;r<4;r++)for(int c=0;c<4;c++){result[size_t(r)*4+c]=a[r][c+4];if(!std::isfinite(a[r][c+4]))return false;}
  return true;
}
inline bool transform(const Matrix& previous,const Matrix& current,Matrix& result) {
  std::array<double,16> inv{};if(!inverse(current,inv))return false;
  for(int r=0;r<4;r++)for(int c=0;c<4;c++){double value=0;for(int k=0;k<4;k++)value+=double(previous[size_t(r)*4+k])*inv[size_t(k)*4+c];result[size_t(r)*4+c]=float(value);if(!std::isfinite(result[size_t(r)*4+c]))return false;}
  return true;
}
struct Candidate { Matrix matrix{};uint64_t sequence=0;int reg=-1;uint32_t matches=0;bool conflict=false;std::string shader; };
struct History {
  std::vector<float> pixels;Matrix matrix{};std::string prefix;
  uint64_t frame=UINT64_MAX,color=0,depth=0;uint32_t generation=0,width=0,height=0,stride=0;int colorFormat=0,depthFormat=0;
};
}
