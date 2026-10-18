#include "rt_sky_shader.h"
#include <iostream>
#include <stdexcept>
#include <string>
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
  // Five PS interpolants with nonmatching SM3 semantic/register indices.
  auto multi=shader(true);
  for (unsigned i=1;i<5;++i) { multi.insert(multi.begin()+13,{0x0200001f,0x80000005u|(i<<16),reg(6,5+i,3)}); }
  multi.pop_back();for (unsigned i=1;i<5;++i) { multi.insert(multi.end(),{0x03000002,reg(6,5+i,3),src(1,1),src(2,6+i)}); }multi.push_back(0xffff);
  std::vector<uint32_t> pixel{0xffff0300};for (unsigned i=0;i<5;++i) { pixel.insert(pixel.end(),{0x0200001f,0x80000005u|(i<<16),reg(1,7-i,3)}); }pixel.push_back(0xffff);
  const auto interface=pixelInterface(pixel);require(interface.valid && interface.masks[4]==3,"SM3 semantic mapping");
  const auto program=parse(multi);require(program.valid,"multiple UV outputs rejected");
  view={1,0,0,0,0,1,0,0,0,0,1,0,-3000,-1500,200,1};projection={.7f,0,0,0,0,1.1f,0,0,0,0,-1.00001f,-1,0,0,-7.00007f,0};
  for (unsigned j=0;j<4;++j) { for (unsigned i=0;i<4;++i) { double v=0;for (unsigned k=0;k<4;++k) { v+=double(view[i*4+k])*projection[k*4+j]; }c[j][i]=float(v); } }
  std::vector<Vec> positions{{574110,575043,-576007,1},{574110,-579655,578691,1}},texcoords{{.2f,.3f,0,1},{.6f,.7f,0,1}};
  auto rebuilt=rebuild(program,interface,c,positions,texcoords,view,projection);
  require(rebuilt.valid && std::string(rebuilt.reason)=="original_world_positions_full_ps_uv","large-coordinate world positions were inversely reconstructed");
  require(rebuilt.vertices[0][0]==positions[0][0] && rebuilt.vertices[1][2]==positions[1][2],"large world coordinate changed");
  for (unsigned i=1;i<5;++i) { require(std::abs(rebuilt.vertices[0][3+4*i]-(texcoords[0][0]+c[6+i][0]))<1e-6,"extra UV output lost"); }
  auto missing=interface;missing.masks[7]=3;require(!rebuild(program,missing,c,positions,texcoords,view,projection).valid,"undefined PS interpolant accepted");
  auto wrong=c;wrong[3][2]*=2;require(!rebuild(program,interface,wrong,positions,texcoords,view,projection).valid,"nonuniform homogeneous warp accepted");
  // Actual 204924 camera: clip.w cancellation fails the old per-vertex
  // relative test although each VS coefficient is the stored main VP product.
  view={-.622058868f,.111499012f,-.774990797f,0,-.782970488f,-.0885843709f,.61571914f,0,-8.71951101e-18f,.98980844f,.142405123f,0,-3834.6123f,-1166.54065f,-1315.81934f,1};
  projection={.737271011f,0,0,0,0,1.17963362f,0,0,0,0,-1.00000703f,-1,0,0,-7.00004911f,0};
  c[0]={-.458625972f,-.577261448f,-6.42864258e-18f,-2827.14844f};
  c[1]={.131527975f,-.104497105f,1.16761136f,-1376.09058f};
  c[2]={.774996221f,-.615723491f,-.142406121f,1308.82861f};
  c[3]={.774990797f,-.61571914f,-.142405123f,1315.81934f};
  const Vec edge{574074.375f,575053.875f,578691.5f,1};
  require(evaluate(program,c,edge,texcoords[0],clip,uv) && !sameProjection(edge,clip,view,projection),"actual cancellation regression not exercised");
  require(originalPositionMatrix(program,c,view,projection),"stored main-camera DP4 matrix not proven");
  rebuilt=rebuild(program,interface,c,{edge}, {texcoords[0]},view,projection);
  require(rebuilt.valid && rebuilt.vertices[0][0]==edge[0] && rebuilt.vertices[0][2]==edge[2],"sky matrix proof did not preserve exact source vertices");
  for (unsigned j=0;j<4;++j) {
    auto warped=c;warped[j][3]+=.01f;require(!originalPositionMatrix(program,warped,view,projection),"foreign clip matrix accepted");
  }
  auto overwritten=program;overwritten.instructions.push_back(overwritten.instructions[0]);
  // Add an actual second writer to POSITION, not merely a UV instruction.
  for (const auto& ins:program.instructions) { if (type(ins.dest)==program.positionType && index(ins.dest)==program.positionIndex) { overwritten.instructions.push_back(ins);break; } }
  require(!originalPositionMatrix(overwritten,c,view,projection),"multiple position writers accepted as DP4-only proof");
  auto saturated=program;for (auto& ins:saturated.instructions) { if (type(ins.dest)==program.positionType && index(ins.dest)==program.positionIndex) { ins.dest|=1u<<20;break; } }
  require(!originalPositionMatrix(saturated,c,view,projection),"saturated clip output accepted as matrix product");
  std::cout << "RT sky matrix coefficient proof/cancelled-w/warp rejection: PASS\n";
  std::cout << "RT full sky interpolants/world-coordinate precision: PASS\n";
  std::cout << "RT original sky shader bounded replay/UV/unprojection/rejection: PASS\n";return 0;
 } catch (const std::exception& e) { std::cerr << e.what() << '\n';return 1; }
}
