#pragma once
// Used only in the x64 runtime TU and its native checks, never the game ABI TU.
#include "rt_model_input.h"
#include "rt_world_remix_c.h"
#include <map>
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
  uint64_t bytes() const { return vertices.size()*sizeof(remixapi_HardcodedVertex)+indices.size()*4; }
};
inline bool build(const scene_input::Prepared& source,uint32_t kind,D3DPRIMITIVETYPE type,DWORD cull,Geometry& out) {
  out.vertices.clear();out.indices.clear();
  if(source.stride!=32 || !source.ownedDraw || source.bytes.empty() || source.bytes.size()%32 ||
     source.bytes.size()/32>32768 || (cull!=D3DCULL_NONE&&cull!=D3DCULL_CW&&cull!=D3DCULL_CCW)) { return false; }
  const size_t count=source.bytes.size()/32;std::vector<uint32_t> indices;
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
  out.vertices.resize(size_t(triangles)*3);out.indices.resize(size_t(triangles)*3);
  for(uint32_t t=0;t<triangles;++t) {
    uint32_t ix[3]{};for(unsigned j=0;j<3;++j) { ix[j]=indices[type==D3DPT_TRIANGLELIST?size_t(t)*3+j:size_t(t)+j]; }
    if(type==D3DPT_TRIANGLESTRIP && t%2) { std::swap(ix[0],ix[1]); }
    if(cull==D3DCULL_CW) { std::swap(ix[0],ix[1]); }
    float p[3][5]{};
    for(unsigned j=0;j<3;++j) { std::memcpy(p[j],source.bytes.data()+size_t(ix[j])*32,20);
      for(float x:p[j]) { if(!std::isfinite(x)||std::abs(x)>1e7f) { out={};return false; } } }
    double u[3]{},v[3]{};for(unsigned j=0;j<3;++j) { u[j]=double(p[1][j])-p[0][j];v[j]=double(p[2][j])-p[0][j]; }
    double n[]={u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
    const double length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
    if(length<1e-12) { n[0]=0;n[1]=0;n[2]=1; }else { for(double& x:n) { x/=length; } }
    for(unsigned j=0;j<3;++j) { const size_t i=size_t(t)*3+j;auto& a=out.vertices[i];a={};
      std::memcpy(a.position,p[j],12);std::memcpy(a.texcoord,p[j]+3,8);for(unsigned k=0;k<3;++k) { a.normal[k]=float(n[k]); }
      a.color=0xffffffff;out.indices[i]=uint32_t(i); }
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
struct Record { remixapi_MeshHandle mesh=nullptr;uint64_t last=0,bytes=0; };
enum class Budget : unsigned { None,Draws,FrameBytes,LiveBytes,Entries,GpuBacklog,Count };
inline const char* name(Budget reason) {
  switch(reason) { case Budget::Draws:return "draw_limit";case Budget::FrameBytes:return "frame_geometry_bytes";
    case Budget::LiveBytes:return "live_geometry_bytes";case Budget::Entries:return "mesh_entries";
    case Budget::GpuBacklog:return "gpu_backlog";default:return "none"; }
}
struct Limits { uint64_t bytes=256ull*1024*1024,frameBytes=96ull*1024*1024;unsigned draws=512,entries=512; };
struct Submission {
  bool success=false,created=false,deferred=false;Budget budget=Budget::None;
  uint64_t handle=0,triangles=0;uint32_t code=0;const char* operation="input";std::string key;
};
class Scene {
  remixapi_Interface* api=nullptr;uint64_t next=1;
  std::map<std::string,Record> meshes;
  Limits limits;
  std::array<remixapi_MaterialHandle,size_t(Material::Count)> materials{};
public:
  uint64_t bytes=0,peakBytes=0,created=0,reused=0,destroyed=0,submitted=0;
  static constexpr uint64_t maxBytes=256ull*1024*1024,maxFrameBytes=96ull*1024*1024;
  uint64_t frame=0,frameBytes=0,omitted=0;unsigned frameDraws=0,frameOmitted=0;
  std::array<uint64_t,size_t(Budget::Count)> omissionTotals{};
  explicit Scene(remixapi_Interface* table=nullptr,Limits policy={}):api(table),limits(policy) {}
  void frameAt(uint64_t at) { if(at!=frame) { frame=at;frameDraws=0;frameBytes=0;frameOmitted=0; } }
  Submission defer(Budget reason,uint64_t at) {
    frameAt(at);Submission result;result.deferred=true;result.budget=reason;result.operation="budget";
    ++omitted;++frameOmitted;++omissionTotals[size_t(reason)];return result;
  }
  uint32_t begin(uint64_t completed,uint64_t at) { frameAt(at);return retire(completed,at); }

  size_t entries() const { return meshes.size(); }
  void bind(remixapi_Interface* table) { api=table; }
  Submission draw(const scene_input::Prepared& source,uint32_t kind,D3DPRIMITIVETYPE topology,DWORD cull,Material variant,uint64_t at,size_t pending=0) {
    Submission result;
    if(!api || !api->CreateMaterial||!api->CreateMesh||!api->DrawInstance || unsigned(variant)>=unsigned(Material::Count) ||
       !source.ready || !bulk_static::affine(source.world)) { result.code=ERROR_INVALID_DATA;return result; }
    frameAt(at);
    if(pending>=8) { return defer(Budget::GpuBacklog,at); }
    if(frameDraws>=limits.draws) { return defer(Budget::Draws,at); }
    result.operation="hash";
    const auto vh=ldb_rt_file::geometrySha256(source.bytes.data(),source.bytes.size());
    const auto ih=ldb_rt_file::geometrySha256(source.indices.data(),source.indices.size());
    if(vh.empty()||ih.empty()) { result.code=ERROR_INVALID_DATA;return result; }
    result.key=vh+ih+":"+std::to_string(kind)+":"+std::to_string(unsigned(topology))+":"+std::to_string(cull)+":"+std::to_string(unsigned(variant));
    auto found=meshes.find(result.key);
    if(found==meshes.end()) {
      result.operation="geometry";Geometry geometry;if(!build(source,kind,topology,cull,geometry)) { result.code=ERROR_INVALID_DATA;return result; }
      if(geometry.bytes()>limits.frameBytes-frameBytes) { return defer(Budget::FrameBytes,at); }
      if(geometry.bytes()>limits.bytes-bytes) { return defer(Budget::LiveBytes,at); }
      if(meshes.size()>=limits.entries) { return defer(Budget::Entries,at); }
      auto& material=materials[size_t(variant)];
      if(!material) {
        auto ext=opaque(variant);remixapi_MaterialInfo info{};info.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;info.pNext=&ext;
        info.hash=0x4c444d4100000001ull+unsigned(variant);
        if(variant==Material::Emissive) { info.emissiveIntensity=3;info.emissiveColorConstant={0.8f,0.2f,0.05f}; }
        result.operation="create_material";result.code=uint32_t(api->CreateMaterial(&info,&material));if(result.code||!material) { if(!result.code) { result.code=ERROR_INVALID_HANDLE; }return result; }
      }
      remixapi_MeshInfoSurfaceTriangles surface{};surface.vertices_values=geometry.vertices.data();surface.vertices_count=geometry.vertices.size();
      surface.indices_values=geometry.indices.data();surface.indices_count=geometry.indices.size();surface.material=material;
      remixapi_MeshInfo mesh{};mesh.sType=REMIXAPI_STRUCT_TYPE_MESH_INFO;mesh.hash=0x4c444d5300000000ull+next++;
      mesh.surfaces_values=&surface;mesh.surfaces_count=1;remixapi_MeshHandle handle=nullptr;
      result.operation="create_mesh";result.code=uint32_t(api->CreateMesh(&mesh,&handle));if(result.code||!handle) { if(!result.code) { result.code=ERROR_INVALID_HANDLE; }return result; }
      // The pinned CreateMesh copies CPU arrays before returning and queues
      // Rc-owned buffers. CPU scratch can be dropped here, handles cannot.
      found=meshes.emplace(result.key,Record{handle,at,geometry.bytes()}).first;
      bytes+=geometry.bytes();peakBytes=(std::max)(peakBytes,bytes);frameBytes+=geometry.bytes();++created;result.created=true;
    } else { ++reused; }
    auto& r=found->second;r.last=at;result.handle=reinterpret_cast<uint64_t>(r.mesh);
    result.triangles=kind==2?source.drawArgs[4]:source.drawArgs[1];
    remixapi_InstanceInfo instance{};instance.sType=REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;instance.mesh=r.mesh;instance.doubleSided=cull==D3DCULL_NONE;
    for(unsigned row=0;row<3;++row) { for(unsigned col=0;col<4;++col) { instance.transform.matrix[row][col]=source.world[col*4+row]; } }
    result.operation="draw_instance";result.code=uint32_t(api->DrawInstance(&instance));if(result.code) { return result; }
    ++submitted;++frameDraws;result.success=true;return result;
  }
  uint32_t retire(uint64_t completed,uint64_t now,bool all=false) {
    for(auto it=meshes.begin();it!=meshes.end();) {
      const auto& r=it->second;
      if(r.last<=completed && (all || (now>=r.last && now-r.last>=2))) {
        const auto code=uint32_t(api->DestroyMesh(r.mesh));if(code) { return code; }
        bytes-=r.bytes;it=meshes.erase(it);++destroyed;
      } else { ++it; }
    }
    if(all&&meshes.empty()) { for(auto& material:materials) { if(material) { const auto code=uint32_t(api->DestroyMaterial(material));if(code) { return code; }material=nullptr; } } }
    return 0;
  }
};
} // namespace ldb_rt::live_mesh
