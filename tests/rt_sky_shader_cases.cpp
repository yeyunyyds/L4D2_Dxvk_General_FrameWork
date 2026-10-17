#include "rt_sky_shader.h"
#include <iostream>
#include <stdexcept>
using namespace ldb_rt::sky_shader;
void require(bool condition,const char* why) { if (!condition) { throw std::runtime_error(why); } }
uint32_t reg(unsigned kind,unsigned number,unsigned mask=15) { return 0x80000000u|((kind&7)<<28)|((kind&24)<<8)|number|(mask<<16); }
uint32_t src(unsigned kind,unsigned number,unsigned swizzle=228) { return reg(kind,number,0)|(swizzle<<16); }
std::vector<uint32_t> shader(bool modern=false) {
  std::vector<uint32_t> w{modern?0xfffe0300u:0xfffe0200u,0x0200001f,0x80000000,reg(1,0),0x0200001f,0x80000005,reg(1,1)};
  if (modern) { w.insert(w.end(),{0x0200001f,0x80000000,reg(6,3),0x0200001f,0x80000005,reg(6,5)}); }
  for (unsigned i=0;i<4;++i) { w.insert(w.end(),{0x03000009,reg(modern?6:4,modern?3:0,1u<<i),src(1,0),src(2,i)}); }
  w.insert(w.end(),{0x04000004,reg(6,modern?5:0,3),src(1,1),src(2,4),src(2,5),0x0000ffff});return w;
}
int main() {
 try {
  Constants c{};for (unsigned i=0;i<4;++i) { c[i][i]=1; }c[4]={2,-1,0,0};c[5]={.25f,1,0,0};
  for (bool modern:{false,true}) {
   const auto code=shader(modern);const auto p=parse(code);require(p.valid,"straight-line shader rejected");Vec clip{},uv{};
   require(evaluate(p,c,{2,3,-5,1},{.3f,.8f,0,1},clip,uv),"defined shader failed");
   require(clip==Vec{2,3,-5,1} && std::abs(uv[0]-.85f)<1e-6 && std::abs(uv[1]-.2f)<1e-6,"position or transformed UV incorrect");
   auto changed=code;changed[1]=0x02000028;require(!parse(changed).valid,"control flow accepted");
   changed=code;changed[3]|=0x2000;require(!parse(changed).valid,"relative destination accepted");
   changed=code;changed.pop_back();require(!parse(changed).valid,"missing END accepted");
  }
  auto code=shader();code[10]=src(0,0);auto p=parse(code);Vec clip{},uv{};require(p.valid && !evaluate(p,c,{1,2,3,1},{.2f,.3f,0,1},clip,uv),"undefined temporary accepted");
  code=shader();code[10]=src(2,255);c[255][0]=NAN;p=parse(code);require(!evaluate(p,c,{1,2,3,1},{.2f,.3f,0,1},clip,uv),"nonfinite read accepted");
  std::array<float,16> view{},projection{};for (unsigned i=0;i<4;++i) { view[i*5]=1; }view[12]=3;
  projection={1,0,0,0,0,2,0,0,0,0,-1.0001f,-1,0,0,-7.0007f,0};std::array<double,16> inverse{};
  require(inverseViewProjection(view,projection,inverse),"inverse failed");Vec world{};double homogeneous=0;
  require(unproject({5,6,3.0003f,10},inverse,world,homogeneous),"unproject failed");
  // Analytical row-vector transformation for world(2,3,-10,1).
  require(std::abs(world[0]-2)<.001 && std::abs(world[1]-3)<.001 && std::abs(world[2]+10)<.001,"unproject matrix order wrong");
  projection={};require(!inverseViewProjection(view,projection,inverse),"singular transform accepted");
  require(pixelInputs({0xffff0200,0x0200001f,0x80000000,reg(3,0,3),0xffff}),"PS texcoord0 rejected");
  require(!pixelInputs({0xffff0200,0x0200001f,0x80000000,reg(3,1,3),0xffff}),"PS extra interpolant accepted");
  std::cout << "RT original sky shader bounded replay/UV/unprojection/rejection: PASS\n";return 0;
 } catch (const std::exception& e) { std::cerr << e.what() << '\n';return 1; }
}
