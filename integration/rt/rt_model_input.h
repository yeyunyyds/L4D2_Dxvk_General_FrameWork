#pragma once
#include "rt_bulk_static.h"
#include "rt_model_shader.h"
#include "rt_file_hash.h"

namespace ldb_rt::model_input {
static_assert(model_shader::kMaxVertices==scene_input::kModelLimits.vertices,"model capture/rebuild budget mismatch");
inline bool ownDraw(scene_input::Prepared& out,const scene_input::Data& input,uint32_t kind,const std::array<uint32_t,6>& args) {
  if (!input.count || out.bytes.size()!=size_t(input.first+input.count)*32) { out.reason="model_owned_vertex_range";return false; }
  std::vector<uint8_t> compact(out.bytes.begin()+size_t(input.first)*32,out.bytes.end());out.bytes.swap(compact);
  out.drawArgs={};out.indexBytes=input.indexBytes;
  if (kind==2) {
    if ((input.indexBytes!=2 && input.indexBytes!=4) || input.indices.empty() || input.indices.size()%input.indexBytes) { out.reason="model_owned_index_format";return false; }
    out.indices.resize(input.indices.size());
    for (size_t at=0;at<input.indices.size();at+=input.indexBytes) {
      uint32_t old=0;std::memcpy(&old,input.indices.data()+at,input.indexBytes);
      const int64_t local=int64_t(int32_t(args[0]))+old-input.first;
      if (local<0 || uint64_t(local)>=input.count) { out.reason="model_owned_index_range";return false; }
      const uint32_t value=uint32_t(local);std::memcpy(out.indices.data()+at,&value,input.indexBytes);
    }
    out.drawArgs={0,0,input.count,0,args[4],0};
  } else if (kind==1) { out.drawArgs={0,args[1],0,0,0,0}; }
  else { out.reason="model_owned_draw_kind";return false; }
  out.ownedDraw=true;return true;
}
inline scene_input::Prepared reconstruct(const draw_observe::Snapshot& s,const scene_input::Data& input,const std::string& digest) {
  scene_input::Prepared out;
  if (FAILED(input.result)) {
    out.reason=input.reason;out.result=input.result;
    // Budget rejection is unsupported input, not a failed state getter/setter.
    if (input.geometryResult==E_INVALIDARG && (!std::strcmp(input.reason,"vertex_budget") || !std::strcmp(input.reason,"absolute_vertex_budget") ||
        !std::strcmp(input.reason,"index_budget") || !std::strcmp(input.reason,"unsupported_draw"))) { out.result=S_FALSE; }
    return out;
  }
  const auto profile=model_shader::profile(digest);
  const bool neko=!std::strcmp(s.materialInfo.shader.data(),"NekoToon");
  if (neko!=(profile==model_shader::Profile::Neko3)) { out.reason="model_material_program_mismatch";return out; }
  if (neko) {
    const auto ps=ldb_rt_file::bytesSha256(input.ps.data(),input.ps.size()*4);
    if (ps!="f2da6f64daa0d3bad8b1ac86309ac191371a51e44733c7ec927dd0a9729ae8e9" &&
        ps!="23e61215dfa5c78e579fd1d8996a63035377ca479dca2c553991cd4736090d1a") { out.reason="unknown_neko_pixel_program";return out; }
  }
  int position=-1,uv=-1,weight=-1,indices=-1;std::vector<D3DVERTEXELEMENT9> plain;
  std::vector<D3DVERTEXELEMENT9> elements;
  const D3DVERTEXELEMENT9 end=D3DDECL_END();
  if (input.declaration.size()<3 || input.declaration.size()>MAXD3DDECLLENGTH+1 ||
      std::memcmp(&input.declaration.back(),&end,sizeof(end))) { out.reason="model_declaration_unavailable";return out; }
  for (size_t i=0;i+1<input.declaration.size();++i) {
    const auto& e=input.declaration[i];
    const bool bone=e.Usage==D3DDECLUSAGE_BLENDWEIGHT || e.Usage==D3DDECLUSAGE_BLENDINDICES;
    if (bone) {
      if (e.Stream || e.UsageIndex || e.Method || e.Offset%4 || (e.Usage==D3DDECLUSAGE_BLENDWEIGHT?
          (e.Type!=D3DDECLTYPE_SHORT2 || weight>=0):(e.Type!=D3DDECLTYPE_D3DCOLOR || indices>=0))) { out.reason="unsupported_model_bone_declaration";return out; }
      (e.Usage==D3DDECLUSAGE_BLENDWEIGHT?weight:indices)=e.Offset;
    } else { elements.push_back(e); }
  }
  elements.push_back(end);
  if (!bulk_static::layout(elements.data(),UINT(elements.size()),input.stride,plain)) { out.reason="unsupported_model_declaration";return out; }
  for (const auto& e:plain) {
    if (e.Usage==D3DDECLUSAGE_POSITION && !e.UsageIndex && !e.Stream) { position=e.Offset; }
    if (e.Usage==D3DDECLUSAGE_TEXCOORD && !e.UsageIndex && !e.Stream) { uv=e.Offset; }
  }
  if (position<0 || uv<0) { out.reason="model_position_or_uv_unavailable";return out; }
  if (input.constants[13][0]!=0 && input.morph.positions.size()!=input.count) {
    out.reason="model_morph_position_unavailable";out.result=input.morph.result;return out;
  }
  auto transformed=model_shader::reconstruct(profile,input.constants,input.vertices,input.stride,unsigned(position),unsigned(uv),weight,indices,input.morph.positions);
  out.reason=transformed.reason;if (!transformed.valid) { return out; }
  if (!model_shader::cameraCompatible(transformed,input.constants,s.view,s.projection)) { out.reason="model_clip_camera_mismatch";return out; }
  if (!input.count || input.vertices.size()!=size_t(input.count)*32 || uint64_t(input.first)+input.count>model_shader::kMaxVertices) { out.reason="model_vertex_range";return out; }
  out.stride=32;out.world=transformed.world;out.sourceDeclaration=input.declaration;
  out.elements={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
  out.bytes.resize(size_t(input.first+input.count)*32);std::memcpy(out.bytes.data()+size_t(input.first)*32,transformed.vertices.data(),transformed.vertices.size());return out;
}
inline scene_input::Prepared prepare(IDirect3DDevice9* d,const draw_observe::Snapshot& s,uint32_t kind,D3DPRIMITIVETYPE type,const std::array<uint32_t,6>& args,scene_input::Data* evidence=nullptr,bool cpuOnly=false) {
  DWORD values[7]{};scene_input::Prepared rejected;
  const D3DRENDERSTATETYPE names[]={D3DRS_ALPHABLENDENABLE,D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_STENCILENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_COLORWRITEENABLE,D3DRS_ZFUNC};
  for (unsigned i=0;i<7;++i) {
    const auto hr=d->GetRenderState(names[i],values+i);if (FAILED(hr)) { rejected.result=hr;rejected.reason="model_pipeline_query_failed";return rejected; }
  }
  if (values[0] || values[1]!=1 || values[2]!=1) { rejected.reason="model_pipeline_unsupported";return rejected; }
  if (values[3] || values[4] || (values[5]&7)!=7 || (values[5]&~15u) || (values[6]!=D3DCMP_LESS && values[6]!=D3DCMP_LESSEQUAL)) { rejected.reason="model_pipeline_special_pass";return rejected; }
  auto input=scene_input::capture(d,kind,type,args,false,scene_input::kModelLimits);
  const auto digest=ldb_rt_file::bytesSha256(input.vs.data(),input.vs.size()*4);
  if (model_shader::profile(digest)!=model_shader::Profile::Unknown && SUCCEEDED(input.result) && input.constants[13][0]!=0) { scene_input::morphPositions(d,input); }
  auto out=reconstruct(s,input,digest);if (!out.bytes.empty() && ownDraw(out,input,kind,args) && (cpuOnly || scene_input::upload(d,out))) { out.ready=true; }
  if (evidence && out.ready) { *evidence=std::move(input); }return out;
}
} // namespace ldb_rt::model_input
