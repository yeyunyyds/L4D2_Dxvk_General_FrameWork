#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define REMIX_WINAPI_NO_LIBRARY_LOADER
#include "rt_model_batch.h"
#include <iostream>
#include <set>
#include <thread>
using namespace ldb_rt;
void require(bool value,const char* why) { if(!value) { throw std::runtime_error(why); } }
#include "rt_infected_material_cases.h"
namespace fixture {
unsigned failureCode=1,fail=0,created=0,updates=0,draws=0,materials=0,destroyed=0,materialDestroyed=0;std::set<uint64_t> live,materialLive;
int lastAlpha=7;uint8_t lastRef=0;bool lastTextured=false,lastBlend=false;float lastOpacity=0;
bool lastSkinned=false;uint32_t lastSkinWeights=0,lastSkinIndices=0,lastBoneCount=0,lastStableInstance=0;float lastBoneX=0;
std::vector<remixapi_HardcodedVertex> received;remixapi_Transform lastTransform{};
remixapi_ErrorCode result(unsigned at) { return static_cast<remixapi_ErrorCode>(fail==at?failureCode:0); }
remixapi_ErrorCode REMIXAPI_CALL material(const remixapi_MaterialInfo* info,remixapi_MaterialHandle* out) {
  if(fail==1) { return result(1); }require(info&&info->pNext,"material extension missing");
  const auto& p=*static_cast<remixapi_MaterialInfoOpaqueEXT*>(info->pNext);
  require(p.alphaTestType>=0&&p.alphaTestType<=7&&!p.useDrawCallAlphaState&&p.opacityConstant>0,"invalid API material alpha");
  require(materialLive.insert(info->hash).second,"repeated registration ignored by pinned runtime; destroy first");lastAlpha=p.alphaTestType;lastRef=p.alphaReferenceValue;lastTextured=info->albedoTexture!=nullptr;
  lastBlend=p.blendType_hasvalue!=FALSE;lastOpacity=p.opacityConstant;
  ++materials;*out=reinterpret_cast<remixapi_MaterialHandle>(info->hash);return static_cast<remixapi_ErrorCode>(0);
}
remixapi_ErrorCode REMIXAPI_CALL mesh(const remixapi_MeshInfo* info,remixapi_MeshHandle* out) {
  if(fail==2) { return result(2); }require(info&&info->surfaces_count==1,"invalid mesh surface count");
  const auto& s=info->surfaces_values[0];require(s.material&&s.vertices_count&&s.indices_count&&s.indices_count%3==0,"indexed mesh correspondence");
  for(size_t i=0;i<s.indices_count;++i) { require(s.indices_values[i]<s.vertices_count,"API index exceeds retained vertex range"); }
  lastSkinned=s.skinning_hasvalue!=FALSE;lastSkinWeights=s.skinning_value.blendWeights_count;lastSkinIndices=s.skinning_value.blendIndices_count;
  if(lastSkinned) { require(s.skinning_value.bonesPerVertex==3&&lastSkinWeights==s.vertices_count*3&&lastSkinIndices==s.vertices_count*3,"API skinning scalar counts"); }
  require(live.insert(info->hash).second,"duplicate live API mesh handle");++created;
  received.assign(s.vertices_values,s.vertices_values+s.vertices_count);*out=reinterpret_cast<remixapi_MeshHandle>(info->hash);return static_cast<remixapi_ErrorCode>(0);
}
remixapi_ErrorCode REMIXAPI_CALL updateMesh(const remixapi_MeshInfo* info) {
  require(info&&info->surfaces_count==1,"invalid persistent mesh upsert");
  const auto& s=info->surfaces_values[0];require(s.vertices_count&&s.indices_count,"empty persistent update");
  live.insert(info->hash);received.assign(s.vertices_values,s.vertices_values+s.vertices_count);++updates;return static_cast<remixapi_ErrorCode>(0);
}
remixapi_ErrorCode REMIXAPI_CALL draw(const remixapi_InstanceInfo* info) {
  if(fail==3) { return result(3); }require(live.count(reinterpret_cast<uint64_t>(info->mesh))!=0,"draw dead mesh");lastTransform=info->transform;
  struct Extension { remixapi_StructType type;void* next; };
  lastBoneCount=0;lastStableInstance=0;lastBoneX=0;for(auto* next=info->pNext;next;) {
    const auto* extension=static_cast<const Extension*>(next);const auto type=extension->type;const auto linked=extension->next;
    if(type==REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT) { const auto* b=static_cast<const remixapi_InstanceInfoBoneTransformsEXT*>(next);lastBoneCount=b->boneTransforms_count;if(lastBoneCount>1) { lastBoneX=b->boneTransforms_values[1].matrix[0][3]; } }
    else if(type==REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_OBJECT_PICKING_EXT) { lastStableInstance=static_cast<const remixapi_InstanceInfoObjectPickingEXT*>(next)->objectPickingValue; }
    else { require(false,"unknown instance extension"); }next=linked;
  }
  ++draws;return static_cast<remixapi_ErrorCode>(0);
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
#include "rt_model_texture_cases.h"
int main() {
 try {
  infectedMaterialCases();textureHashCases();systemTextureExport();
  {
    model_input::Captured captured;captured.reason="model_pipeline_unsupported";
    require(model_batch::captureDisposition(captured)==model_batch::CaptureDisposition::Fallback,
      "supported non-ready batch exclusion became fatal");
    captured.ready=true;
    require(model_batch::captureDisposition(captured)==model_batch::CaptureDisposition::Queue,
      "ready batch capture was not queued");
    captured.ready=false;captured.result=E_FAIL;
    require(model_batch::captureDisposition(captured)==model_batch::CaptureDisposition::Fatal,
      "failed D3D batch capture was silently downgraded");
    std::cout<<"RT frame batch recoverable model exclusion versus fatal capture: PASS\n";
  }
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
  require(live_mesh::build(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,geometry)&&geometry.vertices.size()==4,"indexed strip vertex retention");
  const unsigned expected[]={0,1,2,2,1,3};
  for(unsigned i=0;i<6;++i) { require(geometry.indices[i]==expected[i],"strip triangle indices changed");const auto& a=geometry.vertices[geometry.indices[i]];require(!std::memcmp(a.position,p.bytes.data()+expected[i]*32,12),"strip position changed");require(a.normal[2]==1&&a.color==0xffffffff,"flat normal wrong"); }
  require(live_mesh::build(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_CW,geometry)&&geometry.vertices[0].normal[2]==-1,"CW winding not mirrored");
  p.indices[0]=9;require(!live_mesh::build(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,geometry),"out-of-range index accepted");p.indices[0]=0;
  const float nan=std::numeric_limits<float>::quiet_NaN();std::memcpy(p.bytes.data(),&nan,4);require(!live_mesh::build(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,geometry),"NaN position accepted");const float zero=0;std::memcpy(p.bytes.data(),&zero,4);
  std::cout<<"RT live mesh normalized indices/strip winding/flat normals/finite bounds: PASS\n";
  {
    draw_observe::Snapshot snapshot;snapshot.materialInfo.shader={};std::memcpy(snapshot.materialInfo.shader.data(),"VertexLitGeneric",16);
    snapshot.view={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};snapshot.projection=snapshot.view;
    scene_input::Data input;input.first=0;input.count=4;input.stride=32;input.indexBytes=2;input.vertices.resize(128);input.indices={0,0,1,0,2,0,3,0};
    input.declaration={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_SHORT2,0,D3DDECLUSAGE_BLENDWEIGHT,0},
      {0,16,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_BLENDINDICES,0},{0,24,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
    input.constants[0][1]=1;input.constants[48]={1,0,0,0};input.constants[49]={0,1,0,0};
    for(unsigned i=0;i<4;++i) { input.constants[8+i][i]=1;const float vertex[]={float(i&1),float(i>>1),0};const float uv[]={float(i&1),float(i>>1)};const int16_t weights[]={8191,16383};const uint8_t bones[]={2,0,1,255};
      std::memcpy(input.vertices.data()+i*32,vertex,12);std::memcpy(input.vertices.data()+i*32+12,weights,4);std::memcpy(input.vertices.data()+i*32+16,bones,4);std::memcpy(input.vertices.data()+i*32+24,uv,8); }
    for(unsigned bone=0;bone<3;++bone) { input.constants[58+bone*3]={1,0,0,float(bone*10)};input.constants[59+bone*3]={0,1,0,0};input.constants[60+bone*3]={0,0,1,0}; }
    const auto digest="21a836464d57307495a547b937729a9a197b4a210864605bf7acdb16aa03931b";
    auto cpuPrepared=model_input::reconstruct(snapshot,input,digest);
    require(!cpuPrepared.skinning.enabled&&cpuPrepared.dynamicPose&&std::string(cpuPrepared.reason)=="original_vs_three_weight","default path must identify dynamic CPU skinning after device loss");
    require(model_input::ownDraw(cpuPrepared,input,2,{0,0,4,0,2,0}),"CPU pose owned Draw compaction");cpuPrepared.ready=true;
    auto dynamicApi=fixture::table();live_mesh::Scene dynamic(&dynamicApi);
    const auto dynamicFirst=dynamic.draw(cpuPrepared,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,1);
    const auto dynamicSecond=dynamic.draw(cpuPrepared,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,1);
    require(dynamicFirst.success&&dynamicSecond.success&&dynamicFirst.created&&dynamicSecond.created&&dynamicFirst.handle!=dynamicSecond.handle&&
      dynamicFirst.key.find("dynamic-pose-frame-1-draw-0")==0&&dynamicSecond.key.find("dynamic-pose-frame-1-draw-1")==0,
      "dynamic CPU poses performed hash reuse or shared a simultaneous mesh");
    require(!dynamic.retire(1,3,true)&&dynamic.entries()==0,"dynamic CPU pose cleanup");
    auto isolatedLimit=model_input::reconstruct(snapshot,input,digest,false,true,true);
    require(isolatedLimit.bytes.empty()&&isolatedLimit.result==S_FALSE&&std::string(isolatedLimit.reason)=="performance_gpu_skin_only_omitted","performance limit unexpectedly built a CPU-pose mesh");
    auto prepared=model_input::reconstruct(snapshot,input,digest,true,true,true);
    require(prepared.skinning.enabled&&!prepared.dynamicPose&&prepared.skinning.weights.size()==12&&prepared.skinning.transforms.size()==3,"audited three-weight bind pose did not select GPU skinning");
    for(size_t vertex=0;vertex<4;++vertex) {
      float bind[3]{},skinned[3]{};std::memcpy(bind,prepared.bytes.data()+vertex*32,12);const size_t base=vertex*prepared.skinning.bonesPerVertex;
      for(uint32_t k=0;k<prepared.skinning.bonesPerVertex;++k) { const auto& m=prepared.skinning.transforms[prepared.skinning.indices[base+k]];const float w=prepared.skinning.weights[base+k];
        for(unsigned row=0;row<3;++row) { skinned[row]+=(bind[0]*m[row*4]+bind[1]*m[row*4+1]+bind[2]*m[row*4+2]+m[row*4+3])*w; } }
      require(std::abs(skinned[0]-(bind[0]+7.5f))<1e-5f&&std::abs(skinned[1]-bind[1])<1e-5f,"per-vertex three-weight stride oracle mismatch");
    }
    require(model_input::ownDraw(prepared,input,2,{0,0,4,0,2,0}),"GPU skinning owned Draw compaction");prepared.ready=true;
    auto skinApi=fixture::table();live_mesh::Scene skins(&skinApi);const auto first=skins.draw(prepared,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,1);
    require(first.success&&first.created&&first.skinned&&fixture::lastSkinned&&fixture::lastSkinWeights==12&&fixture::lastBoneCount==3&&fixture::lastBoneX==10&&skins.frameSkinnedDraws==1&&skins.frameSkinnedUniqueMeshes==1&&!skins.frameSkinnedRepeatDraws,"first persistent skinned mesh payload/frame accounting");
    prepared.skinning.transforms[1][3]=20;const auto second=skins.draw(prepared,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,2);
    require(second.success&&!second.created&&second.handle==first.handle&&fixture::lastBoneX==20&&skins.skinnedCreated==1&&skins.skinnedReused==1&&skins.frameSkinnedDraws==1&&skins.frameSkinnedUniqueMeshes==1&&!skins.frameSkinnedRepeatDraws,"bone update recreated persistent mesh or frame counter did not reset");
    const auto repeated=skins.draw(prepared,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,2);
    require(repeated.success&&repeated.created&&repeated.handle!=first.handle&&skins.skinnedCreated==2&&skins.skinnedReused==1&&skins.frameSkinnedDraws==2&&skins.frameSkinnedUniqueMeshes==2&&!skins.frameSkinnedRepeatDraws,"simultaneous skinned poses shared a mutable mesh handle");
    const auto firstNextFrame=skins.draw(prepared,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,3);
    const auto secondNextFrame=skins.draw(prepared,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,3);
    require(firstNextFrame.success&&secondNextFrame.success&&!firstNextFrame.created&&!secondNextFrame.created&&firstNextFrame.handle==first.handle&&secondNextFrame.handle==repeated.handle&&skins.skinnedCreated==2&&skins.skinnedReused==3&&skins.frameSkinnedUniqueMeshes==2,"skinned pose slots were not persistent across frames");
    require(!skins.retire(3,5,true)&&skins.entries()==0,"persistent skin cleanup");
    auto sharedApi=fixture::table();live_mesh::Scene shared(&sharedApi);const auto sharedA=shared.draw(prepared,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,4,0,{},3,101,7,0x1234567800000042ull);
    const auto firstStable=fixture::lastStableInstance;prepared.skinning.transforms[1][3]=30;
    const auto sharedB=shared.draw(prepared,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,4,0,{},3,101,7,0x8765432100000011ull);
    require(sharedA.success&&sharedB.success&&sharedA.created&&!sharedB.created&&sharedA.handle==sharedB.handle&&shared.entries()==1&&
      shared.frameSkinnedUniqueMeshes==1&&shared.frameSkinnedRepeatDraws==1&&firstStable==(0x12345678u^0x42u)&&
      fixture::lastStableInstance==(0x87654321u^0x11u),"stable model instances duplicated topology or lost explicit identity");
    require(!shared.retire(4,6,true),"stable shared skin cleanup");
    input.constants[13][0]=1;input.morph.positions.resize(4);auto morphed=model_input::reconstruct(snapshot,input,digest,true);
    require(!morphed.skinning.enabled&&std::string(morphed.reason)=="original_vs_three_weight","morph must retain CPU skin fallback");
    auto isolatedMorph=model_input::reconstruct(snapshot,input,digest,true,true,true);
    require(isolatedMorph.bytes.empty()&&isolatedMorph.result==S_FALSE&&std::string(isolatedMorph.reason)=="performance_gpu_skin_only_omitted","performance mode reconstructed a morphed CPU-pose mesh");
    std::cout<<"RT default CPU skin safety path/isolated GPU skin fixture/persistent mesh/bone update/morph fallback: PASS\n";
  }
  {
    auto smooth=fixture::source();smooth.elements={{0,20,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,0},D3DDECL_END()};
    const float normal[]={0,1,0};for(unsigned i=0;i<4;++i) { std::memcpy(smooth.bytes.data()+i*32+20,normal,12); }
    require(live_mesh::build(smooth,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,geometry)&&geometry.vertices[0].normal[1]==1,"source smooth normal lost");
    std::memset(smooth.bytes.data()+20,0,12);
    require(live_mesh::build(smooth,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,geometry)&&geometry.vertices[0].normal[2]==1&&geometry.vertices[1].normal[1]==1,"invalid normal failed per-vertex geometric fallback");
    std::cout<<"RT audited smooth normal import/per-vertex geometric fallback: PASS\n";
  }
  {
    auto bent=fixture::source();const float z=1;std::memcpy(bent.bytes.data()+3*32+8,&z,4);
    require(live_mesh::build(bent,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,geometry),"bent indexed geometry rejected");
    require(geometry.vertices.size()==4&&geometry.indices.size()==6&&geometry.bytes()==4*64+6*4,"indexed byte budget differs");
    const auto& n=geometry.vertices[1].normal;require(n[0]<0&&n[1]<0&&n[2]>0&&std::abs(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]-1)<1e-5,"area-weighted shared normal incorrect");
    std::cout<<"RT compact indexed topology/source normals/area-weighted fallback/bytes: PASS\n";
  }
  {
    struct AlphaDevice {
      DWORD src=D3DBLEND_SRCALPHA,dst=D3DBLEND_INVSRCALPHA,op=D3DBLENDOP_ADD,separate=0;bool failed=false;
      HRESULT GetRenderState(D3DRENDERSTATETYPE state,DWORD* out) {
        if(failed) { return E_FAIL; }*out=state==D3DRS_SRCBLEND?src:state==D3DRS_DESTBLEND?dst:state==D3DRS_BLENDOP?op:separate;return S_OK;
      }
    } device;
    require(model_input::standardAlpha(&device),"standard texture alpha rejected");device.dst=D3DBLEND_ONE;require(!model_input::standardAlpha(&device),"additive pass treated as ordinary alpha");device.dst=D3DBLEND_INVSRCALPHA;
    device.src=D3DBLEND_ONE;require(!model_input::standardAlpha(&device),"premultiplied pass silently changed");device.src=D3DBLEND_SRCALPHA;
    device.separate=1;require(!model_input::standardAlpha(&device),"separate alpha pass silently changed");device.separate=0;device.failed=true;require(!model_input::standardAlpha(&device),"failed blend getter admitted");
    auto api=fixture::table();live_mesh::Scene alphaScene(&api);model_texture::Source source;source.key="source-alpha";source.alphaBlend=true;source.path=L"alpha.dds";source.alphaTest=4;source.alphaRef=100;
    require(alphaScene.draw(p,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,1,0,source).success&&fixture::lastBlend&&fixture::lastOpacity==1&&fixture::lastAlpha==4&&fixture::lastRef==100,"source alpha material not preserved");
    require(!alphaScene.retire(1,3,true),"alpha resource cleanup");fixture::created=fixture::destroyed=fixture::materials=fixture::materialDestroyed=fixture::draws=0;
    std::cout<<"RT standard alpha admission/rejection/DDS opacity/material cleanup: PASS\n";
  }
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
    const auto xrgb=model_texture::layout(4,4,D3DFMT_X8R8G8B8,true);auto opaque=model_texture::dds(4,4,xrgb,rows.data(),16);
    require(xrgb.dxgi==91&&opaque.size()==212,"X8 mapped to unsupported Vulkan format through GLI");
    for(unsigned pixel=0;pixel<16;++pixel) { require(opaque[148+pixel*4+3]==255&&!std::memcmp(opaque.data()+148+pixel*4,rows.data()+pixel*4,3),"X8 conversion changed RGB or kept undefined alpha"); }
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
    for(unsigned i=0;i<1024;++i) { require(capped.draw(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,live_mesh::Material::Neutral,50).success,"draw budget early failure"); }
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
  {
    auto input=fixture::source();live_mesh::Geometry sourceGeometry;
    require(live_mesh::build(input,2,D3DPT_TRIANGLESTRIP,D3DCULL_NONE,sourceGeometry),"batch source geometry");
    auto result=[&](float x) {
      model_batch::Result value;value.success=true;value.dynamicPose=true;value.geometry=sourceGeometry;value.cull=D3DCULL_NONE;
      value.world={1,0,0,0,0,1,0,0,0,0,1,0,x,0,0,1};value.material.key="shared";return value;
    };
    std::vector<model_batch::Result> parts;parts.push_back(result(0));parts.push_back(result(0));parts.push_back(result(10));
    auto assembled=model_batch::assemble(std::move(parts));
    require(assembled.sourceDraws==3&&assembled.sourceVertices==12&&assembled.sourceIndices==18&&assembled.batches.size()==2,
      "frame batch did not group exact material/transform classes");
    const auto grouped=std::find_if(assembled.batches.begin(),assembled.batches.end(),[](const auto& batch) { return batch.sourceDraws==2; });
    require(grouped!=assembled.batches.end()&&grouped->geometry.vertices.size()==8&&grouped->geometry.indices.size()==12&&
      grouped->geometry.indices[6]>=4,"frame batch did not rebase merged indices");
    auto batchApi=fixture::table();live_mesh::Scene batched(&batchApi);const auto before=fixture::created;
    auto submission=batched.drawBatch(grouped->geometry,grouped->world,grouped->cull,grouped->variant,70,0,grouped->material,69);
    require(submission.success&&submission.created&&fixture::created==before+1&&batched.submitted==1&&batched.frameDraws==1,
      "combined Source draws were not one Remix mesh/instance");
    require(!batched.retire(70,72,true),"frame batch cleanup");
    require(grouped->persistent&&!grouped->persistentKey.empty(),"dynamic batch has no persistent slot identity");
    live_mesh::Scene persistent(&batchApi,{},fixture::updateMesh);const auto updatesBefore=fixture::updates,createsBefore=fixture::created;
    auto warm=persistent.drawBatch(grouped->geometry,grouped->world,grouped->cull,grouped->variant,80,0,grouped->material,79,grouped->persistentKey);
    auto changed=grouped->geometry;changed.vertices[0].position[0]+=0.25f;
    auto steady=persistent.drawBatch(changed,grouped->world,grouped->cull,grouped->variant,81,0,grouped->material,80,grouped->persistentKey);
    require(warm.success&&warm.created&&!warm.updated&&steady.success&&!steady.created&&steady.updated&&steady.handle==warm.handle&&
      fixture::updates==updatesBefore+2&&fixture::created==createsBefore&&persistent.created==1&&persistent.updated==1&&persistent.entries()==1,
      "persistent frame batch bypassed the Runtime upsert path, recreated its Remix mesh, or skipped geometry update");
    require(!persistent.retire(81,83,true),"persistent frame batch cleanup");
    std::cout<<"RT frame batch exact grouping plus persistent first-frame upsert/same-handle update: PASS\n";
  }
  require(fixture::live.empty()&&fixture::materialLive.empty(),"fixture left live meshes or materials");
  std::cout<<"RT live mesh CPU pose update/cache/material/fence-gated lifetime/API failures: PASS; actual GPU/game unrun\n";return 0;
 }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
