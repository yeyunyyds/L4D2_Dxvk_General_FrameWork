#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define REMIX_WINAPI_NO_LIBRARY_LOADER
#include "rt_live_mesh.h"
#include <iostream>
#include <set>
using namespace ldb_rt;
void require(bool value,const char* why) { if(!value) { throw std::runtime_error(why); } }
namespace fixture {
unsigned failureCode=1,fail=0,created=0,draws=0,materials=0,destroyed=0,materialDestroyed=0;std::set<uint64_t> live,materialLive;
uint8_t lastAlpha=7,lastRef=0;bool lastTextured=false;
std::vector<remixapi_HardcodedVertex> received;remixapi_Transform lastTransform{};
remixapi_ErrorCode result(unsigned at) { return static_cast<remixapi_ErrorCode>(fail==at?failureCode:0); }
remixapi_ErrorCode REMIXAPI_CALL material(const remixapi_MaterialInfo* info,remixapi_MaterialHandle* out) {
  if(fail==1) { return result(1); }require(info&&info->pNext,"material extension missing");
  const auto& p=*static_cast<remixapi_MaterialInfoOpaqueEXT*>(info->pNext);
  require(p.alphaTestType<=7&&!p.useDrawCallAlphaState&&p.opacityConstant>0,"invalid API material alpha");
  require(materialLive.insert(info->hash).second,"repeated registration ignored by pinned runtime; destroy first");lastAlpha=p.alphaTestType;lastRef=p.alphaReferenceValue;lastTextured=info->albedoTexture!=nullptr;
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
remixapi_ErrorCode REMIXAPI_CALL destroyMaterial(remixapi_MaterialHandle handle) { if(fail==5) { return result(5); }require(materialLive.erase(reinterpret_cast<uint64_t>(handle))==1,"double material destroy");++materialDestroyed;return static_cast<remixapi_ErrorCode>(0); }
remixapi_Interface table() { remixapi_Interface t{};t.CreateMaterial=material;t.CreateMesh=mesh;t.DrawInstance=draw;t.DestroyMesh=destroy;t.DestroyMaterial=destroyMaterial;return t; }
scene_input::Prepared source() {
  scene_input::Prepared p;p.ready=true;p.ownedDraw=true;p.indexBytes=2;p.drawArgs={0,0,4,0,2,0};
  const float v[4][8]={{0,0,0,0,0,0,0,0},{1,0,0,1,0,0,0,0},{0,1,0,0,1,0,0,0},{1,1,0,1,1,0,0,0}};
  p.bytes.resize(sizeof(v));std::memcpy(p.bytes.data(),v,sizeof(v));p.indices={0,0,1,0,2,0,3,0};return p;
}
struct TextureLock {
  unsigned locks=0,unlocks=0;HRESULT code=S_OK;
  HRESULT LockRect(UINT,D3DLOCKED_RECT*,const RECT*,DWORD flags) { require(flags==D3DLOCK_READONLY,"texture lock wrote game data");++locks;return code; }
  HRESULT UnlockRect(UINT) { ++unlocks;return S_OK; }
};
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
  auto pendingMaterial=scene.draw(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Metal,4,0,{},2);
  require(pendingMaterial.deferred&&pendingMaterial.budget==live_mesh::Budget::GpuBacklog,"material replaced while GPU still uses it");
  auto d=scene.draw(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Metal,4,0,{},3);require(d.success&&!d.created&&d.handle==c.handle&&d.materialUpdated,"material update recreated geometry or lost surface");
  require(!scene.retire(0,8)&&scene.entries()==2,"uncompleted mesh destroyed");
  require(!scene.retire(2,8)&&scene.entries()==1&&fixture::destroyed==1,"completed mesh retirement");
  require(!scene.retire(4,8,true)&&scene.entries()==0&&!scene.bytes&&fixture::live.empty()&&fixture::materialDestroyed==2,"mesh/material cleanup ordering");
  {
    live_mesh::Scene textures(&api);auto input=fixture::source();model_texture::Source first;first.key="texture-A-alpha-4-100";first.path=L"albedo.dds";first.alphaTest=4;first.alphaRef=100;
    auto textured=textures.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,10,0,first,9);
    require(textured.success&&fixture::lastTextured&&fixture::lastAlpha==4&&fixture::lastRef==100,"original texture/cutout state lost");
    auto second=first;second.key="texture-B-alpha-4-100";second.path=L"different.dds";
    auto different=textures.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,10,0,second,9);
    require(different.success&&different.handle!=textured.handle&&different.materialHandle!=textured.materialHandle,"different textures aliased material or geometry");
    fixture::fail=5;auto failed=textures.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Metal,11,0,first,10);
    require(!failed.success&&failed.code&&std::string(failed.operation)=="destroy_material_for_update","material update destroy failure hidden");fixture::fail=0;
    require(!textures.retire(11,13,true),"textured material recovery");
  }
  std::cout<<"RT live material completed-use destroy/recreate/stable-geometry/texture identity: PASS\n";
  {
    const auto bc=model_texture::layout(5,7,D3DFMT_DXT5,true);require(bc.dxgi==78&&bc.rowBytes==32&&bc.rows==2&&bc.bytes()==64,"DXT5 sRGB block layout");
    std::array<uint8_t,80> rows{};for(unsigned i=0;i<rows.size();++i) { rows[i]=uint8_t(i); }
    auto encoded=model_texture::dds(5,7,bc,rows.data(),40);require(encoded.size()==212,"DDS DX10 size");uint32_t header[37]{};std::memcpy(header,encoded.data(),148);
    require(header[0]==0x20534444&&header[19]==32&&header[21]==0x30315844&&header[32]==78&&header[33]==3&&header[35]==1,"DDS DX10 metadata");
    require(!std::memcmp(encoded.data()+148,rows.data(),32)&&!std::memcmp(encoded.data()+180,rows.data()+40,32),"texture row padding leaked into DDS");
    require(model_texture::dds(5,7,bc,rows.data(),31).empty()&&model_texture::dds(5,7,bc,rows.data(),-40).empty(),"invalid texture pitch accepted");
    require(model_texture::layout(4,4,D3DFMT_A8R8G8B8,false).dxgi==87&&model_texture::layout(4,4,D3DFMT_A8R8G8B8,true).dxgi==91,"BGRA color space lost");
    require(!model_texture::layout(4097,4,D3DFMT_DXT1,true).dxgi&&!model_texture::layout(0,4,D3DFMT_DXT1,true).dxgi&&!model_texture::layout(4,4,D3DFMT_A16B16G16R16F,true).dxgi,"unsupported texture accepted");
    uint8_t address=99;require(model_texture::wrap(D3DTADDRESS_CLAMP,address)&&address==0&&model_texture::wrap(D3DTADDRESS_WRAP,address)&&address==1&&!model_texture::wrap(D3DTADDRESS_BORDER,address),"MDL sampler address mismatch");
  }
  { fixture::TextureLock texture;
    try { model_texture::ReadLock<fixture::TextureLock> lock(&texture);throw std::runtime_error("fixture allocation failure"); }catch(const std::runtime_error&) {}
    require(texture.locks==1&&texture.unlocks==1,"exception left original texture locked");
    texture.code=E_FAIL;{ model_texture::ReadLock<fixture::TextureLock> lock(&texture); }
    require(texture.locks==2&&texture.unlocks==1,"failed lock incorrectly unlocked texture");
  }
  std::cout<<"RT model DDS DX10 row pitch/sRGB/alpha/bounds: PASS\n";
  for(unsigned failure=1;failure<=3;++failure) { fixture::fail=failure;live_mesh::Scene broken(&api);auto input=fixture::source();
   auto x=broken.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,20+failure);require(!x.success&&x.code,"API create/draw failure ignored");
   fixture::fail=0;require(!broken.retire(100,100,true),"failed submission handles leaked"); }
  for(unsigned failure=4;failure<=5;++failure) { live_mesh::Scene broken(&api);auto input=fixture::source();require(broken.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Alpha,30).success,"destruction failure setup");fixture::fail=failure;
   require(broken.retire(100,100,true)!=0,"API destroy failure hidden");fixture::fail=0;require(!broken.retire(100,100,true),"destruction retry failed"); }
  { live_mesh::Scene capped(&api);auto input=fixture::source();
    for(unsigned i=0;i<512;++i) { require(capped.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,50).success,"draw budget early failure"); }
    auto omitted=capped.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,50);
    require(omitted.deferred&&!omitted.success&&!omitted.code&&omitted.budget==live_mesh::Budget::Draws&&capped.omitted==1,"draw budget must omit, not fail API");
    require(!capped.retire(49,100,true)&&capped.entries()==1,"recovery destroyed uncompleted resource");
    require(!capped.retire(50,100,true)&&!capped.entries(),"completed recovery failed");
  }
  { auto input=fixture::source();live_mesh::Geometry g;require(live_mesh::build(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,g),"budget fixture geometry");
    for(unsigned which=0;which<3;++which) {
      live_mesh::Limits limits;if(which==0) { limits.bytes=1; }else if(which==1) { limits.frameBytes=1; }else { limits.entries=0; }
      live_mesh::Scene bounded(&api,limits);const auto before=fixture::created;
      auto x=bounded.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,60);
      require(x.deferred&&!x.code&&fixture::created==before&&!bounded.entries(),"geometry budget allocated or failed API");
      require(x.budget==(which==0?live_mesh::Budget::LiveBytes:which==1?live_mesh::Budget::FrameBytes:live_mesh::Budget::Entries),"wrong geometry budget reason");
    }
    live_mesh::Limits limits;limits.bytes=2*g.bytes();live_mesh::Scene early(&api,limits);
    require(early.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,1).success,"first pose");
    const float secondPose=0.1f,thirdPose=0.2f;std::memcpy(input.bytes.data(),&secondPose,4);
    require(early.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,2).success,"second pose");
    std::memcpy(input.bytes.data(),&thirdPose,4);require(!early.begin(0,3),"early retirement query");
    auto blocked=early.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,3);
    require(blocked.deferred&&early.entries()==2,"incomplete pose freed to make space");
    require(!early.begin(1,3)&&early.entries()==1,"completed idle pose not retired before allocation");
    require(early.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,3).success&&early.peakBytes==limits.bytes,"early retirement did not prevent transient growth");
    require(!early.retire(3,5,true),"early retirement cleanup");
    live_mesh::Scene backlog(&api);const auto before=fixture::created;
    auto x=backlog.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,10,8);
    require(x.deferred&&x.budget==live_mesh::Budget::GpuBacklog&&!x.code&&fixture::created==before,"GPU backlog must defer without allocation");
    require(backlog.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,11,7).success,"backlog did not resume");
    require(!backlog.retire(11,13,true),"backlog cleanup");
    fixture::failureCode=8;fixture::fail=2;live_mesh::Scene apiFailure(&api);
    x=apiFailure.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,20);
    require(!x.success&&!x.deferred&&x.code==8&&std::string(x.operation)=="create_mesh","API error 8 misclassified as application budget");
    fixture::fail=0;fixture::failureCode=1;require(!apiFailure.retire(20,22,true),"real API failure cleanup");
  }
  std::cout<<"RT live mesh >128 draws/budget omission/early retirement/backlog: PASS\n";
  require(fixture::live.empty()&&fixture::materialLive.empty(),"fixture left live meshes or materials");
  std::cout<<"RT live mesh CPU pose update/cache/material/fence-gated lifetime/API failures: PASS; actual GPU/game unrun\n";return 0;
 }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
