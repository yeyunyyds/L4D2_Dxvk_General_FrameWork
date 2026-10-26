#pragma once
// Used only in the x64 runtime TU and its native checks, never the game ABI TU.
#include "rt_model_input.h"
#include "rt_model_texture.h"
#include "rt_world_remix_c.h"
#include <map>
#include <set>
#include <sstream>
#include <limits>

namespace ldb_rt::live_mesh {
constexpr auto kCategory=static_cast<D3DRENDERSTATETYPE>(42);
constexpr DWORD kIgnore=1u<<3,kUnused=0xfefefefe;
// Keep the physical original Draw, including its shaders and animation, but
// exclude it from the legacy RTX collector while the explicit instance is used.
template<class Device> class IgnoreScope {
  Device* device;DWORD original=0;bool dirty=false;
public:
  HRESULT capture=S_OK,apply=S_OK,restore=S_OK;
  explicit IgnoreScope(Device* d):device(d) {
    capture=device->GetRenderState(kCategory,&original);if(FAILED(capture)) { return; }
    dirty=true;apply=device->SetRenderState(kCategory,(original==kUnused?0:original)|kIgnore);
    if(FAILED(apply)) { finish(); }
  }
  bool ready() const { return SUCCEEDED(capture)&&SUCCEEDED(apply)&&SUCCEEDED(restore)&&dirty; }
  void finish() { if(dirty) { dirty=false;restore=device->SetRenderState(kCategory,original); } }
  ~IgnoreScope() { finish(); }
};

enum class Material : unsigned { Neutral,Rough,Metal,Alpha,Emissive,Count };
inline const char* name(Material v) {
  switch(v) { case Material::Rough:return "rough";case Material::Metal:return "metal";
    case Material::Alpha:return "alpha";case Material::Emissive:return "emissive";default:return "neutral"; }
}
struct Geometry {
  std::vector<remixapi_HardcodedVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<float> blendWeights;
  std::vector<uint32_t> blendIndices;
  uint32_t bonesPerVertex=0;
  uint64_t bytes() const { return vertices.size()*sizeof(remixapi_HardcodedVertex)+indices.size()*4+blendWeights.size()*4+blendIndices.size()*4; }
};
inline bool build(const scene_input::Prepared& source,uint32_t kind,D3DPRIMITIVETYPE type,DWORD cull,Geometry& out) {
  runtime_profile::Scope timing(runtime_profile::Stage::GeometryBuild);
  out={};
  if(source.stride!=32 || !source.ownedDraw || source.bytes.empty() || source.bytes.size()%32 ||
     source.bytes.size()/32>32768 || (cull!=D3DCULL_NONE&&cull!=D3DCULL_CW&&cull!=D3DCULL_CCW)) { return false; }
  const bool smooth=std::any_of(source.elements.begin(),source.elements.end(),[](const D3DVERTEXELEMENT9& e) {
    return e.Stream==0&&e.Offset==20&&e.Type==D3DDECLTYPE_FLOAT3&&e.Usage==D3DDECLUSAGE_NORMAL&&e.UsageIndex==0;
  });
  const size_t count=source.bytes.size()/32;std::vector<uint32_t> indices;
  if(source.skinning.enabled) {
    if(source.skinning.bonesPerVertex!=3||source.skinning.weights.size()!=count*3||source.skinning.indices.size()!=count*3||
       source.skinning.transforms.empty()||source.skinning.transforms.size()>REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT) { return false; }
    for(size_t i=0;i<count*3;++i) {
      if(!std::isfinite(source.skinning.weights[i])||source.skinning.weights[i]<0||source.skinning.weights[i]>1||source.skinning.indices[i]>=source.skinning.transforms.size()) { return false; }
    }
    out.bonesPerVertex=3;out.blendWeights=source.skinning.weights;out.blendIndices=source.skinning.indices;
  }
  if(kind==2) {
    if((source.indexBytes!=2&&source.indexBytes!=4)||source.indices.empty()||source.indices.size()%source.indexBytes ||
       source.drawArgs[0]||source.drawArgs[1]||source.drawArgs[2]!=count||source.drawArgs[3]) { return false; }
    indices.resize(source.indices.size()/source.indexBytes);
    for(size_t i=0;i<indices.size();++i) { std::memcpy(&indices[i],source.indices.data()+i*source.indexBytes,source.indexBytes); }
  } else if(kind==1) { if(source.drawArgs[0]) { return false; }indices.resize(count);for(size_t i=0;i<count;++i) { indices[i]=uint32_t(i); } }
  else { return false; }
  const uint32_t triangles=kind==2?source.drawArgs[4]:source.drawArgs[1];
  if(!triangles || triangles>131072 || (type!=D3DPT_TRIANGLELIST&&type!=D3DPT_TRIANGLESTRIP) ||
     indices.size()!=(type==D3DPT_TRIANGLELIST?uint64_t(triangles)*3:uint64_t(triangles)+2)) { return false; }
  for(auto i:indices) { if(i>=count) { return false; } }
  // Keep Source's indexed vertex/seam boundaries. Area-weighted geometric
  // normals are an explicit fallback; audited FLOAT3 normals take precedence.
  // This avoids allocating 3 independent 64-byte vertices for every triangle.
  out.vertices.resize(count);out.indices.resize(size_t(triangles)*3);
  std::vector<std::array<double,3>> accumulated(count);
  for(size_t i=0;i<count;++i) {
    auto& vertex=out.vertices[i];vertex={};float v[5]{};std::memcpy(v,source.bytes.data()+i*32,20);
    for(float x:v) { if(!std::isfinite(x)||std::abs(x)>1e7f) { out={};return false; } }
    std::memcpy(vertex.position,v,12);std::memcpy(vertex.texcoord,v+3,8);vertex.color=0xffffffff;
  }
  for(uint32_t t=0;t<triangles;++t) {
    uint32_t ix[3]{};for(unsigned j=0;j<3;++j) { ix[j]=indices[type==D3DPT_TRIANGLELIST?size_t(t)*3+j:size_t(t)+j]; }
    if(type==D3DPT_TRIANGLESTRIP&&t%2) { std::swap(ix[0],ix[1]); }
    if(cull==D3DCULL_CW) { std::swap(ix[0],ix[1]); }
    double u[3]{},v[3]{};
    for(unsigned k=0;k<3;++k) { u[k]=double(out.vertices[ix[1]].position[k])-out.vertices[ix[0]].position[k];v[k]=double(out.vertices[ix[2]].position[k])-out.vertices[ix[0]].position[k]; }
    const double n[]={u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
    for(unsigned j=0;j<3;++j) { out.indices[size_t(t)*3+j]=ix[j];for(unsigned k=0;k<3;++k) { accumulated[ix[j]][k]+=n[k]; } }
  }
  for(size_t i=0;i<count;++i) {
    auto n=accumulated[i];double length=0;
    if(smooth) {
      float sourceNormal[3]{};std::memcpy(sourceNormal,source.bytes.data()+i*32+20,12);
      length=std::sqrt(double(sourceNormal[0])*sourceNormal[0]+double(sourceNormal[1])*sourceNormal[1]+double(sourceNormal[2])*sourceNormal[2]);
      if(std::isfinite(length)&&length>1e-8) { for(unsigned k=0;k<3;++k) { n[k]=sourceNormal[k]; } }else { length=0; }
    }
    if(length==0) { length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]); }
    if(!std::isfinite(length)||length<1e-12) { n={0,0,1};length=1; }
    for(unsigned k=0;k<3;++k) { out.vertices[i].normal[k]=float(n[k]/length); }
  }
  return true;
}
inline remixapi_MaterialInfoOpaqueEXT opaque(Material kind) {
  remixapi_MaterialInfoOpaqueEXT p{};p.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  p.albedoConstant={0.55f,0.55f,0.55f};p.opacityConstant=1;p.roughnessConstant=kind==Material::Rough?1.0f:0.8f;
  p.useDrawCallAlphaState=FALSE;p.alphaTestType=7;
  if(kind==Material::Metal) { p.metallicConstant=1;p.roughnessConstant=0.15f; }
  if(kind==Material::Alpha) { p.opacityConstant=0.4f;p.blendType_hasvalue=TRUE;p.blendType_value=0; }
  return p;
}
struct MaterialRecord { remixapi_MaterialHandle handle=nullptr;Material variant=Material::Neutral;uint64_t last=0,hash=0; };
struct Record { remixapi_MeshHandle mesh=nullptr;uint64_t last=0,bytes=0;std::string materialKey; };
enum class Budget : unsigned { None,Draws,FrameBytes,LiveBytes,Entries,GpuBacklog,Count };
inline const char* name(Budget reason) {
  switch(reason) { case Budget::Draws:return "draw_limit";case Budget::FrameBytes:return "frame_geometry_bytes";
    case Budget::LiveBytes:return "live_geometry_bytes";case Budget::Entries:return "mesh_entries";
    case Budget::GpuBacklog:return "gpu_backlog";default:return "none"; }
}
struct Limits { uint64_t bytes=512ull*1024*1024,frameBytes=192ull*1024*1024;unsigned draws=1024,entries=3072; };
struct Submission {
  bool success=false,created=false,updated=false,deferred=false;Budget budget=Budget::None;
  uint64_t handle=0,triangles=0,materialHandle=0;bool materialUpdated=false,textured=false,skinned=false;uint32_t code=0;const char* operation="input";std::string key;
};
class Scene {
public:
  using UpdateMesh=remixapi_ErrorCode (REMIXAPI_CALL *)(const remixapi_MeshInfo*);
private:
  remixapi_Interface* api=nullptr;UpdateMesh updateMesh=nullptr;uint64_t next=1,nextMaterial=1;
  std::map<std::string,Record> meshes;
  Limits limits;
  std::map<std::string,MaterialRecord> materials;
  std::set<std::string> frameSkinnedKeys;
  // A skinned Remix mesh owns one mutable deformed vertex buffer. Source may
  // draw the same bind-pose geometry several times in a frame for different
  // characters or model parts, so those simultaneous poses must not share a
  // mesh handle. Draw order is stable for Source studio models and gives each
  // occurrence a persistent slot that is reused on the following frame.
  std::map<std::string,unsigned> frameSkinnedSlots;
public:
  uint64_t bytes=0,peakBytes=0,created=0,reused=0,updated=0,destroyed=0,submitted=0,materialUpdates=0,skinnedCreated=0,skinnedReused=0,skinnedSubmitted=0;
  static constexpr uint64_t maxBytes=512ull*1024*1024,maxFrameBytes=192ull*1024*1024;
  uint64_t frame=0,frameBytes=0,omitted=0;unsigned frameDraws=0,frameSkinnedDraws=0,frameSkinnedUniqueMeshes=0,frameSkinnedRepeatDraws=0,frameOmitted=0;
  std::array<uint64_t,size_t(Budget::Count)> omissionTotals{};
  explicit Scene(remixapi_Interface* table=nullptr,Limits policy={},UpdateMesh update=nullptr):api(table),updateMesh(update),limits(policy) {}
  void frameAt(uint64_t at) { if(at!=frame) { frame=at;frameDraws=0;frameSkinnedDraws=0;frameSkinnedUniqueMeshes=0;frameSkinnedRepeatDraws=0;frameBytes=0;frameOmitted=0;frameSkinnedKeys.clear();frameSkinnedSlots.clear(); } }
  Submission defer(Budget reason,uint64_t at) {
    frameAt(at);Submission result;result.deferred=true;result.budget=reason;result.operation="budget";
    ++omitted;++frameOmitted;++omissionTotals[size_t(reason)];return result;
  }
  uint32_t begin(uint64_t completed,uint64_t at) { frameAt(at);return retire(completed,at); }

  size_t entries() const { return meshes.size(); }
  void bind(remixapi_Interface* table,UpdateMesh update=nullptr) { api=table;updateMesh=update; }
  Submission draw(const scene_input::Prepared& source,uint32_t kind,D3DPRIMITIVETYPE topology,DWORD cull,Material variant,uint64_t at,size_t pending=0,const model_texture::Source& sourceMaterial={},uint64_t completed=0) {
    Submission result;
    if(!api || !api->CreateMaterial||!api->DestroyMaterial||!api->CreateMesh||!api->DrawInstance || unsigned(variant)>=unsigned(Material::Count) ||
       !source.ready || !bulk_static::affine(source.world)) { result.code=ERROR_INVALID_DATA;return result; }
    frameAt(at);
    if(pending>=8) { return defer(Budget::GpuBacklog,at); }
    if(frameDraws>=limits.draws) { return defer(Budget::Draws,at); }
    const auto materialKey=sourceMaterial.key;
    auto materialFound=materials.find(materialKey);
    if(materialFound==materials.end()) {
      if(materials.size()>=512) { return defer(Budget::Entries,at); }
      MaterialRecord record;record.hash=0x4c444d4100000000ull+nextMaterial++;
      materialFound=materials.emplace(materialKey,record).first;
    }
    auto& material=materialFound->second;
    if(material.handle&&material.variant!=variant&&material.last>completed) { return defer(Budget::GpuBacklog,at); }
    result.operation="hash";std::string vh,ih;
    if(source.dynamicPose) {
      // CPU-skinned positions normally change every frame. v40 observed only
      // 2.1% reuse, so two SHA256 passes per pose cost far more than the rare
      // hit. Frame/draw identity is exact and retirement remains fence-gated.
      vh="dynamic-pose-frame-"+std::to_string(at)+"-draw-"+std::to_string(frameDraws);
    } else {
      vh=ldb_rt_file::geometrySha256(source.bytes.data(),source.bytes.size());
      ih=ldb_rt_file::geometrySha256(source.indices.data(),source.indices.size());
      if(vh.empty()||ih.empty()) { result.code=ERROR_INVALID_DATA;return result; }
    }
    std::string skinKey;
    if(source.skinning.enabled) {
      const auto wh=ldb_rt_file::geometrySha256(source.skinning.weights.data(),source.skinning.weights.size()*sizeof(float));
      const auto bh=ldb_rt_file::geometrySha256(source.skinning.indices.data(),source.skinning.indices.size()*sizeof(uint32_t));
      if(wh.empty()||bh.empty()) { result.code=ERROR_INVALID_DATA;return result; }skinKey=":skin3:"+wh+bh;result.skinned=true;
    }
    result.key=vh+ih+skinKey+":"+std::to_string(kind)+":"+std::to_string(unsigned(topology))+":"+std::to_string(cull)+":"+materialKey;
    if(source.skinning.enabled) {
      const auto baseKey=result.key;
      result.key+=":pose-slot:"+std::to_string(frameSkinnedSlots[baseKey]++);
    }
    auto found=meshes.find(result.key);
    Geometry geometry;
    if(found==meshes.end()) {
      result.operation="geometry";if(!build(source,kind,topology,cull,geometry)) { result.code=ERROR_INVALID_DATA;return result; }
      if(geometry.bytes()>limits.frameBytes-frameBytes) { return defer(Budget::FrameBytes,at); }
      if(geometry.bytes()>limits.bytes-bytes) { return defer(Budget::LiveBytes,at); }
      if(meshes.size()>=limits.entries) { return defer(Budget::Entries,at); }
    }
    if(!material.handle||material.variant!=variant) {
      runtime_profile::Scope timing(runtime_profile::Stage::Material);
      if(material.handle) {
        result.operation="destroy_material_for_update";result.code=uint32_t(api->DestroyMaterial(material.handle));if(result.code) { return result; }material.handle=nullptr;
      }
      auto ext=opaque(variant);ext.albedoConstant=sourceMaterial.textured()?remixapi_Float3D{1,1,1}:remixapi_Float3D{0.55f,0.55f,0.55f};
      if(sourceMaterial.key=="infected-bright-opaque-control") { ext.albedoConstant={0.1f,1.f,0.2f}; }
      if(sourceMaterial.alphaBlend) { ext.blendType_hasvalue=TRUE;ext.blendType_value=0;ext.opacityConstant=1; }
      ext.alphaTestType=sourceMaterial.alphaTest;ext.alphaReferenceValue=sourceMaterial.alphaRef;
      remixapi_MaterialInfo info{};info.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;info.pNext=&ext;info.hash=material.hash;
      info.albedoTexture=sourceMaterial.textured()?sourceMaterial.path.c_str():nullptr;
      if(sourceMaterial.key=="infected-bright-opaque-control") { info.emissiveIntensity=2;info.emissiveColorConstant={0.1f,1.f,0.2f}; }
      info.filterMode=sourceMaterial.filter;info.wrapModeU=sourceMaterial.wrapU;info.wrapModeV=sourceMaterial.wrapV;
      if(variant==Material::Emissive) { info.emissiveIntensity=3;info.emissiveColorConstant={0.8f,0.2f,0.05f}; }
      result.operation="create_material";result.code=uint32_t(api->CreateMaterial(&info,&material.handle));if(result.code||!material.handle) { if(!result.code) { result.code=ERROR_INVALID_HANDLE; }return result; }
      material.variant=variant;++materialUpdates;result.materialUpdated=true;
    }
    result.materialHandle=material.hash;result.textured=sourceMaterial.textured();
    if(found==meshes.end()) {
      remixapi_MeshInfoSurfaceTriangles surface{};surface.vertices_values=geometry.vertices.data();surface.vertices_count=geometry.vertices.size();
      surface.indices_values=geometry.indices.data();surface.indices_count=geometry.indices.size();surface.material=material.handle;
      if(geometry.bonesPerVertex) {
        surface.skinning_hasvalue=TRUE;surface.skinning_value.bonesPerVertex=geometry.bonesPerVertex;
        surface.skinning_value.blendWeights_values=geometry.blendWeights.data();surface.skinning_value.blendWeights_count=uint32_t(geometry.blendWeights.size());
        surface.skinning_value.blendIndices_values=geometry.blendIndices.data();surface.skinning_value.blendIndices_count=uint32_t(geometry.blendIndices.size());
      }
      remixapi_MeshInfo mesh{};mesh.sType=REMIXAPI_STRUCT_TYPE_MESH_INFO;mesh.hash=0x4c444d5300000000ull+next++;
      mesh.surfaces_values=&surface;mesh.surfaces_count=1;remixapi_MeshHandle handle=nullptr;
      runtime_profile::Scope timing(runtime_profile::Stage::MeshCreate);
      result.operation="create_mesh";result.code=uint32_t(api->CreateMesh(&mesh,&handle));if(result.code||!handle) { if(!result.code) { result.code=ERROR_INVALID_HANDLE; }return result; }
      // The pinned CreateMesh copies CPU arrays before returning and queues
      // Rc-owned buffers. CPU scratch can be dropped here, handles cannot.
      found=meshes.emplace(result.key,Record{handle,at,geometry.bytes(),materialKey}).first;
      bytes+=geometry.bytes();peakBytes=(std::max)(peakBytes,bytes);frameBytes+=geometry.bytes();++created;if(result.skinned) { ++skinnedCreated; }result.created=true;
    } else { ++reused;if(result.skinned) { ++skinnedReused; } }
    auto& r=found->second;r.last=at;result.handle=reinterpret_cast<uint64_t>(r.mesh);
    result.triangles=kind==2?source.drawArgs[4]:source.drawArgs[1];
    remixapi_InstanceInfo instance{};instance.sType=REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;instance.mesh=r.mesh;instance.doubleSided=cull==D3DCULL_NONE;
    for(unsigned row=0;row<3;++row) { for(unsigned col=0;col<4;++col) { instance.transform.matrix[row][col]=source.world[col*4+row]; } }
    std::vector<remixapi_Transform> transforms;remixapi_InstanceInfoBoneTransformsEXT bones{};
    if(source.skinning.enabled) {
      transforms.resize(source.skinning.transforms.size());
      for(size_t i=0;i<transforms.size();++i) { std::memcpy(transforms[i].matrix,source.skinning.transforms[i].data(),sizeof(transforms[i].matrix)); }
      bones.sType=REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;bones.boneTransforms_values=transforms.data();bones.boneTransforms_count=uint32_t(transforms.size());instance.pNext=&bones;
    }
    result.operation="draw_instance";{ runtime_profile::Scope timing(runtime_profile::Stage::Instance);result.code=uint32_t(api->DrawInstance(&instance)); }if(result.code) { return result; }
    material.last=at;++submitted;if(result.skinned) {
      ++skinnedSubmitted;++frameSkinnedDraws;
      if(frameSkinnedKeys.insert(result.key).second) { ++frameSkinnedUniqueMeshes; }else { ++frameSkinnedRepeatDraws; }
    }++frameDraws;result.success=true;return result;
  }
  // A frame batch is already canonical triangle-list geometry. Unlike draw(),
  // it represents many Source surfaces that share one material, transform and
  // raster class. The API still copies the arrays synchronously, but the Host
  // registers one RT geometry instead of one mesh for every Source Draw.
  Submission drawBatch(const Geometry& geometry,const std::array<float,16>& world,DWORD cull,Material variant,
      uint64_t at,size_t pending=0,const model_texture::Source& sourceMaterial={},uint64_t completed=0,
      const std::string& persistentKey={}) {
    Submission result;
    if(!api||!api->CreateMaterial||!api->DestroyMaterial||!api->CreateMesh||!api->DrawInstance||
       unsigned(variant)>=unsigned(Material::Count)||geometry.vertices.empty()||geometry.indices.empty()||
       geometry.indices.size()%3||geometry.bonesPerVertex||!bulk_static::affine(world)||
       (cull!=D3DCULL_NONE&&cull!=D3DCULL_CW&&cull!=D3DCULL_CCW)||(!persistentKey.empty()&&!updateMesh)) { result.code=ERROR_INVALID_DATA;return result; }
    frameAt(at);
    if(pending>=8) { return defer(Budget::GpuBacklog,at); }
    if(frameDraws>=limits.draws) { return defer(Budget::Draws,at); }
    const auto cost=geometry.bytes();
    if(cost>limits.frameBytes-frameBytes) { return defer(Budget::FrameBytes,at); }
    const auto key=persistentKey.empty()?"frame-batch-"+std::to_string(at)+"-"+std::to_string(frameDraws):"persistent-batch-"+persistentKey;
    auto found=meshes.find(key);
    const auto previousCost=found==meshes.end()?0:found->second.bytes;
    if(cost>previousCost&&cost-previousCost>limits.bytes-bytes) { return defer(Budget::LiveBytes,at); }
    if(found==meshes.end()&&meshes.size()>=limits.entries) { return defer(Budget::Entries,at); }
    const auto materialKey=sourceMaterial.key;
    auto materialFound=materials.find(materialKey);
    if(materialFound==materials.end()) {
      if(materials.size()>=512) { return defer(Budget::Entries,at); }
      MaterialRecord record;record.hash=0x4c444d4100000000ull+nextMaterial++;
      materialFound=materials.emplace(materialKey,record).first;
    }
    auto& material=materialFound->second;
    if(material.handle&&material.variant!=variant&&material.last>completed) { return defer(Budget::GpuBacklog,at); }
    if(!material.handle||material.variant!=variant) {
      runtime_profile::Scope timing(runtime_profile::Stage::Material);
      if(material.handle) {
        result.operation="destroy_material_for_batch_update";result.code=uint32_t(api->DestroyMaterial(material.handle));
        if(result.code) { return result; }material.handle=nullptr;
      }
      auto ext=opaque(variant);ext.albedoConstant=sourceMaterial.textured()?remixapi_Float3D{1,1,1}:remixapi_Float3D{0.55f,0.55f,0.55f};
      if(sourceMaterial.alphaBlend) { ext.blendType_hasvalue=TRUE;ext.blendType_value=0;ext.opacityConstant=1; }
      ext.alphaTestType=sourceMaterial.alphaTest;ext.alphaReferenceValue=sourceMaterial.alphaRef;
      remixapi_MaterialInfo info{};info.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;info.pNext=&ext;info.hash=material.hash;
      info.albedoTexture=sourceMaterial.textured()?sourceMaterial.path.c_str():nullptr;
      info.filterMode=sourceMaterial.filter;info.wrapModeU=sourceMaterial.wrapU;info.wrapModeV=sourceMaterial.wrapV;
      if(variant==Material::Emissive) { info.emissiveIntensity=3;info.emissiveColorConstant={0.8f,0.2f,0.05f}; }
      result.operation="create_batch_material";result.code=uint32_t(api->CreateMaterial(&info,&material.handle));
      if(result.code||!material.handle) { if(!result.code) { result.code=ERROR_INVALID_HANDLE; }return result; }
      material.variant=variant;++materialUpdates;result.materialUpdated=true;
    }
    result.materialHandle=material.hash;result.textured=sourceMaterial.textured();
    remixapi_MeshInfoSurfaceTriangles surface{};surface.vertices_values=geometry.vertices.data();surface.vertices_count=geometry.vertices.size();
    surface.indices_values=geometry.indices.data();surface.indices_count=geometry.indices.size();surface.material=material.handle;
    remixapi_MeshInfo mesh{};mesh.sType=REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.surfaces_values=&surface;mesh.surfaces_count=1;
    if(found==meshes.end()) {
      mesh.hash=0x4c444d5300000000ull+next++;remixapi_MeshHandle handle=nullptr;
      { runtime_profile::Scope timing(runtime_profile::Stage::MeshCreate);
        result.operation=persistentKey.empty()?"create_batch_mesh":"upsert_persistent_batch_mesh";
        if(persistentKey.empty()) { result.code=uint32_t(api->CreateMesh(&mesh,&handle)); }
        else { result.code=uint32_t(updateMesh(&mesh));handle=reinterpret_cast<remixapi_MeshHandle>(mesh.hash); }
      }
      if(result.code||!handle) { if(!result.code) { result.code=ERROR_INVALID_HANDLE; }return result; }
      found=meshes.emplace(key,Record{handle,at,cost,materialKey}).first;
      bytes+=cost;++created;result.created=true;
    } else {
      mesh.hash=reinterpret_cast<uint64_t>(found->second.mesh);
      { runtime_profile::Scope timing(runtime_profile::Stage::MeshCreate);
        result.operation="update_persistent_batch_mesh";result.code=uint32_t(updateMesh(&mesh)); }
      if(result.code) { return result; }
      bytes=bytes-previousCost+cost;found->second.bytes=cost;found->second.materialKey=materialKey;++updated;result.updated=true;
    }
    peakBytes=(std::max)(peakBytes,bytes);frameBytes+=cost;result.key=key;
    auto handle=found->second.mesh;result.handle=reinterpret_cast<uint64_t>(handle);result.triangles=geometry.indices.size()/3;
    remixapi_InstanceInfo instance{};instance.sType=REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;instance.mesh=handle;instance.doubleSided=cull==D3DCULL_NONE;
    for(unsigned row=0;row<3;++row) { for(unsigned col=0;col<4;++col) { instance.transform.matrix[row][col]=world[col*4+row]; } }
    result.operation="draw_batch_instance";{ runtime_profile::Scope timing(runtime_profile::Stage::Instance);result.code=uint32_t(api->DrawInstance(&instance)); }
    if(result.code) { return result; }
    material.last=at;found->second.last=at;++submitted;++frameDraws;result.success=true;return result;
  }
  uint32_t retire(uint64_t completed,uint64_t now,bool all=false) {
    runtime_profile::Scope timing(runtime_profile::Stage::Retire);
    for(auto it=meshes.begin();it!=meshes.end();) {
      const auto& r=it->second;
      if(r.last<=completed && (all || (now>=r.last && now-r.last>=2))) {
        const auto code=uint32_t(api->DestroyMesh(r.mesh));if(code) { return code; }
        bytes-=r.bytes;it=meshes.erase(it);++destroyed;
      } else { ++it; }
    }
    std::set<std::string> referenced;for(const auto& mesh:meshes) { referenced.insert(mesh.second.materialKey); }
    for(auto it=materials.begin();it!=materials.end();) {
      auto& material=it->second;
      const bool used=referenced.count(it->first)!=0;
      if(!used&&material.last<=completed&&(all||(now>=material.last&&now-material.last>=2))) {
        if(material.handle) { const auto code=uint32_t(api->DestroyMaterial(material.handle));if(code) { return code; } }
        it=materials.erase(it);
      } else { ++it; }
    }
    return 0;
  }
};
} // namespace ldb_rt::live_mesh
