#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define REMIX_WINAPI_NO_LIBRARY_LOADER
#include "rt_live_mesh.h"
#include <iostream>
#include <set>
using namespace ldb_rt;
void require(bool value,const char* why) { if(!value) { throw std::runtime_error(why); } }
namespace fixture {
unsigned fail=0,created=0,draws=0,materials=0,destroyed=0,materialDestroyed=0;std::set<uint64_t> live;
std::vector<remixapi_HardcodedVertex> received;remixapi_Transform lastTransform{};
remixapi_ErrorCode result(unsigned at) { return static_cast<remixapi_ErrorCode>(fail==at?1:0); }
remixapi_ErrorCode REMIXAPI_CALL material(const remixapi_MaterialInfo* info,remixapi_MaterialHandle* out) {
  if(fail==1) { return result(1); }require(info&&info->pNext,"material extension missing");
  const auto& p=*static_cast<remixapi_MaterialInfoOpaqueEXT*>(info->pNext);
  require(p.alphaTestType==7&&!p.useDrawCallAlphaState&&p.opacityConstant>0,"invalid API material alpha");
  ++materials;*out=reinterpret_cast<remixapi_MaterialHandle>(info->hash);return static_cast<remixapi_ErrorCode>(0);
}
remixapi_ErrorCode REMIXAPI_CALL mesh(const remixapi_MeshInfo* info,remixapi_MeshHandle* out) {
  if(fail==2) { return result(2); }require(info&&info->surfaces_count==1,"invalid mesh surface count");
  const auto& s=info->surfaces_values[0];require(s.material&&s.vertices_count==s.indices_count,"expanded mesh correspondence");
  require(live.insert(info->hash).second,"duplicate live API mesh handle");++created;
  received.assign(s.vertices_values,s.vertices_values+s.vertices_count);*out=reinterpret_cast<remixapi_MeshHandle>(info->hash);return static_cast<remixapi_ErrorCode>(0);
}
remixapi_ErrorCode REMIXAPI_CALL draw(const remixapi_InstanceInfo* info) {
  if(fail==3) { return result(3); }require(live.count(reinterpret_cast<uint64_t>(info->mesh))!=0,"draw dead mesh");lastTransform=info->transform;++draws;return static_cast<remixapi_ErrorCode>(0);
}
remixapi_ErrorCode REMIXAPI_CALL destroy(remixapi_MeshHandle handle) {
  if(fail==4) { return result(4); }require(live.erase(reinterpret_cast<uint64_t>(handle))==1,"double mesh destruction");++destroyed;return static_cast<remixapi_ErrorCode>(0);
}
remixapi_ErrorCode REMIXAPI_CALL destroyMaterial(remixapi_MaterialHandle) { if(fail==5) { return result(5); }++materialDestroyed;return static_cast<remixapi_ErrorCode>(0); }
remixapi_Interface table() { remixapi_Interface t{};t.CreateMaterial=material;t.CreateMesh=mesh;t.DrawInstance=draw;t.DestroyMesh=destroy;t.DestroyMaterial=destroyMaterial;return t; }
scene_input::Prepared source() {
  scene_input::Prepared p;p.ready=true;p.ownedDraw=true;p.indexBytes=2;p.drawArgs={0,0,4,0,2,0};
  const float v[4][8]={{0,0,0,0,0,0,0,0},{1,0,0,1,0,0,0,0},{0,1,0,0,1,0,0,0},{1,1,0,1,1,0,0,0}};
  p.bytes.resize(sizeof(v));std::memcpy(p.bytes.data(),v,sizeof(v));p.indices={0,0,1,0,2,0,3,0};return p;
}
struct CategoryDevice {
 DWORD value=live_mesh::kUnused;unsigned reads=0,writes=0,failGet=0,failSet=0;
 HRESULT GetRenderState(D3DRENDERSTATETYPE name,DWORD* out) { require(name==live_mesh::kCategory,"touched other D3D state");if(++reads==failGet) { return E_FAIL; }*out=value;return S_OK; }
 HRESULT SetRenderState(D3DRENDERSTATETYPE name,DWORD v) { require(name==live_mesh::kCategory,"touched other D3D state");if(++writes==failSet) { return E_FAIL; }value=v;return S_OK; }
};
}
int main() {
 try {
  for(DWORD category:std::array<DWORD,4>{{0,live_mesh::kUnused,4,0x100}}) {
   fixture::CategoryDevice d;d.value=category;
   { live_mesh::IgnoreScope<fixture::CategoryDevice> scope(&d);require(scope.ready()&&d.value==((category==live_mesh::kUnused?0:category)|live_mesh::kIgnore),"ignore flag not applied");scope.finish();require(SUCCEEDED(scope.restore),"ignore restoration"); }
   require(d.value==category&&d.reads==1&&d.writes==2,"original category not restored exactly");
  }
  { fixture::CategoryDevice d;d.failGet=1;live_mesh::IgnoreScope<fixture::CategoryDevice> scope(&d);require(!scope.ready()&&FAILED(scope.capture)&&!d.writes,"failed capture mutated state"); }
  { fixture::CategoryDevice d;d.failSet=1;live_mesh::IgnoreScope<fixture::CategoryDevice> scope(&d);require(!scope.ready()&&FAILED(scope.apply)&&d.value==live_mesh::kUnused,"failed apply did not roll back"); }
  { fixture::CategoryDevice d;d.failSet=2;live_mesh::IgnoreScope<fixture::CategoryDevice> scope(&d);scope.finish();require(FAILED(scope.restore),"restore error hidden"); }
  std::cout<<"RT live mesh original Draw category isolation/capture/apply/restore faults: PASS\n";
  auto p=fixture::source();live_mesh::Geometry geometry;
  require(live_mesh::build(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,geometry)&&geometry.vertices.size()==6,"strip expansion");
  const unsigned expected[]={0,1,2,2,1,3};
  for(unsigned i=0;i<6;++i) { const auto& a=geometry.vertices[i];require(!std::memcmp(a.position,p.bytes.data()+expected[i]*32,12),"strip position changed");require(a.normal[2]==1&&a.color==0xffffffff,"flat normal wrong"); }
  require(live_mesh::build(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_CW,geometry)&&geometry.vertices[0].normal[2]==-1,"CW winding not mirrored");
  p.indices[0]=9;require(!live_mesh::build(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,geometry),"out-of-range index accepted");p.indices[0]=0;
  const float nan=std::numeric_limits<float>::quiet_NaN();std::memcpy(p.bytes.data(),&nan,4);require(!live_mesh::build(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,geometry),"NaN position accepted");const float zero=0;std::memcpy(p.bytes.data(),&zero,4);
  std::cout<<"RT live mesh normalized indices/strip winding/flat normals/finite bounds: PASS\n";
  auto api=fixture::table();live_mesh::Scene scene(&api);
  auto a=scene.draw(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,1);require(a.success&&a.created&&fixture::created==1,"first API mesh not created");
  p.world[12]=10;auto b=scene.draw(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,2);require(b.success&&!b.created&&a.handle==b.handle&&scene.reused==1,"rigid transform recreated mesh");
  require(fixture::lastTransform.matrix[0][3]==10&&fixture::lastTransform.matrix[1][1]==1,"row-world to API transform mapping");
  const float moved=0.2f;std::memcpy(p.bytes.data(),&moved,4);auto c=scene.draw(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,3);require(c.success&&c.created&&c.handle!=a.handle,"live pose reused stale geometry");
  auto d=scene.draw(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Metal,4);require(d.success&&d.created&&d.handle!=c.handle,"material reused wrong surface");
  require(!scene.retire(0,8)&&scene.entries()==3,"uncompleted mesh destroyed");
  require(!scene.retire(2,8)&&scene.entries()==2&&fixture::destroyed==1,"completed mesh retirement");
  require(!scene.retire(4,8,true)&&scene.entries()==0&&!scene.bytes&&fixture::live.empty()&&fixture::materialDestroyed==2,"mesh/material cleanup ordering");
  for(unsigned failure=1;failure<=3;++failure) { fixture::fail=failure;live_mesh::Scene broken(&api);auto input=fixture::source();
   auto x=broken.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,20+failure);require(!x.success&&x.code,"API create/draw failure ignored");
   fixture::fail=0;require(!broken.retire(100,100,true),"failed submission handles leaked"); }
  for(unsigned failure=4;failure<=5;++failure) { live_mesh::Scene broken(&api);auto input=fixture::source();require(broken.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Alpha,30).success,"destruction failure setup");fixture::fail=failure;
   require(broken.retire(100,100,true)!=0,"API destroy failure hidden");fixture::fail=0;require(!broken.retire(100,100,true),"destruction retry failed"); }
  { live_mesh::Scene capped(&api);auto input=fixture::source();
    for(unsigned i=0;i<128;++i) { require(capped.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,50).success,"draw budget early failure"); }
    require(!capped.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,50).success,"draw budget not bounded");
    require(!capped.retire(49,100,true)&&capped.entries()==1,"recovery destroyed uncompleted resource");
    require(!capped.retire(50,100,true)&&!capped.entries(),"completed recovery failed");
  }
  require(fixture::live.empty(),"fixture left live meshes");
  std::cout<<"RT live mesh CPU pose update/cache/material/fence-gated lifetime/API failures: PASS; actual GPU/game unrun\n";return 0;
 }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
