#pragma once
#include "rt_bulk_static.h"
#include "rt_model_shader.h"
#include "rt_file_hash.h"

namespace ldb_rt::model_input {
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
  int position=-1,uv=-1,weight=-1,indices=-1;std::vector<D3DVERTEXELEMENT9> plain;
  auto elements=input.declaration;
  const D3DVERTEXELEMENT9 end=D3DDECL_END();
  if (elements.empty() || std::memcmp(&elements.back(),&end,sizeof(end))) { out.reason="model_declaration_unavailable";return out; }
  for (auto it=elements.begin();it!=elements.end()-1;) {
    const bool bone=it->Usage==D3DDECLUSAGE_BLENDWEIGHT || it->Usage==D3DDECLUSAGE_BLENDINDICES;
    if (bone) {
      if (it->Stream || it->UsageIndex || it->Method || it->Offset%4 || (it->Usage==D3DDECLUSAGE_BLENDWEIGHT?
          (it->Type!=D3DDECLTYPE_SHORT2 || weight>=0):(it->Type!=D3DDECLTYPE_D3DCOLOR || indices>=0))) { out.reason="unsupported_model_bone_declaration";return out; }
      (it->Usage==D3DDECLUSAGE_BLENDWEIGHT?weight:indices)=it->Offset;it=elements.erase(it);
    } else { ++it; }
  }
  if (!bulk_static::layout(elements.data(),UINT(elements.size()),input.stride,plain)) { out.reason="unsupported_model_declaration";return out; }
  for (const auto& e:plain) {
    if (e.Usage==D3DDECLUSAGE_POSITION && !e.UsageIndex && !e.Stream) { position=e.Offset; }
    if (e.Usage==D3DDECLUSAGE_TEXCOORD && !e.UsageIndex && !e.Stream) { uv=e.Offset; }
  }
  if (position<0 || uv<0) { out.reason="model_position_or_uv_unavailable";return out; }
  auto transformed=model_shader::reconstruct(profile,input.constants,input.vertices,input.stride,unsigned(position),unsigned(uv),weight,indices);
  out.reason=transformed.reason;if (!transformed.valid) { return out; }
  if (!model_shader::cameraCompatible(transformed,input.constants,s.view,s.projection)) { out.reason="model_clip_camera_mismatch";return out; }
  if (!input.count || input.vertices.size()!=size_t(input.count)*32 || uint64_t(input.first)+input.count>scene_input::kMaxVertices) { out.reason="model_vertex_range";return out; }
  out.stride=32;out.world=transformed.world;out.sourceDeclaration=input.declaration;
  out.elements={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
  out.bytes.resize(size_t(input.first+input.count)*32);std::memcpy(out.bytes.data()+size_t(input.first)*32,transformed.vertices.data(),transformed.vertices.size());return out;
}
inline scene_input::Prepared prepare(IDirect3DDevice9* d,const draw_observe::Snapshot& s,uint32_t kind,D3DPRIMITIVETYPE type,const std::array<uint32_t,6>& args) {
  DWORD blend=0,depth=0,write=0;scene_input::Prepared rejected;
  const D3DRENDERSTATETYPE names[]={D3DRS_ALPHABLENDENABLE,D3DRS_ZENABLE,D3DRS_ZWRITEENABLE};DWORD* values[]={&blend,&depth,&write};
  for (unsigned i=0;i<3;++i) {
    const auto hr=d->GetRenderState(names[i],values[i]);if (FAILED(hr)) { rejected.result=hr;rejected.reason="model_pipeline_query_failed";return rejected; }
  }
  if (blend || depth!=1 || write!=1) { rejected.reason="model_pipeline_unsupported";return rejected; }
  const auto input=scene_input::capture(d,kind,type,args,false);
  const auto digest=ldb_rt_file::bytesSha256(input.vs.data(),input.vs.size()*4);
  auto out=reconstruct(s,input,digest);if (!out.bytes.empty() && scene_input::upload(d,out)) { out.ready=true; }return out;
}
} // namespace ldb_rt::model_input
