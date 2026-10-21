#pragma once
#include "rt_bulk_static.h"
#include "rt_model_shader.h"
#include "rt_file_hash.h"

namespace ldb_rt::model_input {
static_assert(model_shader::kMaxVertices==scene_input::kModelLimits.vertices,"model capture/rebuild budget mismatch");
inline bool ownDraw(scene_input::Prepared& out,const scene_input::Data& input,uint32_t kind,const std::array<uint32_t,6>& args) {
  if (!input.count || out.bytes.size()!=size_t(input.first+input.count)*32) { out.reason="model_owned_vertex_range";return false; }
  std::vector<uint8_t> compact(out.bytes.begin()+size_t(input.first)*32,out.bytes.end());out.bytes.swap(compact);
  if(out.skinning.enabled) {
    const auto bones=out.skinning.bonesPerVertex;const size_t begin=size_t(input.first)*bones,total=size_t(input.first+input.count)*bones;
    if(!bones||out.skinning.weights.size()!=total||out.skinning.indices.size()!=total) { out.reason="model_owned_skin_range";return false; }
    std::vector<float> weights(out.skinning.weights.begin()+begin,out.skinning.weights.end());out.skinning.weights.swap(weights);
    std::vector<uint32_t> indices(out.skinning.indices.begin()+begin,out.skinning.indices.end());out.skinning.indices.swap(indices);
  }
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
  runtime_profile::Scope timing(runtime_profile::Stage::Skin);
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
  const bool infected=!std::strcmp(s.materialInfo.shader.data(),"Infected");
  if(infected!=(profile==model_shader::Profile::InfectedRigid)) { out.reason="infected_material_program_mismatch";return out; }
  if(infected) {
    const D3DVERTEXELEMENT9 exact[]={
      {0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,0},
      {0,24,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},{0,32,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_TANGENT,0},
      {1,0,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_COLOR,1},{2,0,D3DDECLTYPE_FLOAT4,0,D3DDECLUSAGE_POSITION,1},
      {2,16,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,1},D3DDECL_END()};
    if(input.stride!=48||input.declaration.size()!=8||std::memcmp(input.declaration.data(),exact,sizeof(exact))||
       ldb_rt_file::bytesSha256(input.ps.data(),input.ps.size()*4)!="3bac21dc89466c265f1c1411bd54bcdfeec8c0a9fafceab1b1891739384e7de7") { out.reason="infected_layout_or_pixel_program";return out; }
    const auto rebuilt=model_shader::infected(input.constants,input.vertices);out.reason=rebuilt.reason;
    if(!rebuilt.valid) { return out; }
    if(!model_shader::cameraCompatible(rebuilt,input.constants,s.view,s.projection)) { out.reason="model_clip_camera_mismatch";return out; }
    if(!input.count||input.vertices.size()!=size_t(input.count)*48||uint64_t(input.first)+input.count>model_shader::kMaxVertices) { out.reason="model_vertex_range";return out; }
    out.stride=32;out.world=rebuilt.world;out.sourceDeclaration=input.declaration;
    out.elements={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},
      {0,20,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,0},D3DDECL_END()};
    out.bytes.resize(size_t(input.first+input.count)*32);std::memcpy(out.bytes.data()+size_t(input.first)*32,rebuilt.vertices.data(),rebuilt.vertices.size());return out;
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
  // Preserve immutable bind-pose geometry and move only the audited three-
  // weight palette each Draw. Morph targets retain the exact CPU path below.
  const bool gpuSkin=(profile==model_shader::Profile::Skin3||profile==model_shader::Profile::Neko3)&&input.constants[13][0]==0;
  if(gpuSkin) {
    constexpr uint32_t maxBones=256;
    if(input.stride!=32||input.vertices.size()!=size_t(input.count)*32||!input.count||uint64_t(input.first)+input.count>model_shader::kMaxVertices||
       weight<0||indices<0||unsigned(weight)>28||unsigned(indices)>28||input.constants[0][1]!=1||
       (profile==model_shader::Profile::Neko3&&input.constants[12][3]!=0)) { out.reason="gpu_skin_layout_or_control";return out; }
    for(unsigned r:{48u,49u}) { for(float v:input.constants[r]) { if(!std::isfinite(v)) { out.reason="model_uv_nonfinite";return out; } } }
    out.stride=32;out.world={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};out.sourceDeclaration=input.declaration;
    out.elements={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
    out.bytes.resize(size_t(input.first+input.count)*32);out.skinning.enabled=true;out.skinning.bonesPerVertex=3;
    out.skinning.weights.resize(size_t(input.first+input.count)*3);out.skinning.indices.resize(size_t(input.first+input.count)*3);
    std::array<bool,maxBones> used{};uint32_t highest=0;
    for(uint32_t n=0;n<input.count;++n) {
      const auto* src=input.vertices.data()+size_t(n)*32;auto* dst=out.bytes.data()+size_t(input.first+n)*32;
      float v[3]{},t[2]{};std::memcpy(v,src+position,12);std::memcpy(t,src+uv,8);
      for(float x:v) { if(!std::isfinite(x)||std::abs(x)>1e7f) { out.reason="model_position_nonfinite";return out; } }
      int16_t packed[2]{};std::memcpy(packed,src+weight,4);
      const float weights[3]={(float(packed[0])+1.f)/32768.f,(float(packed[1])+1.f)/32768.f,
        1.f-((float(packed[0])+1.f)/32768.f+(float(packed[1])+1.f)/32768.f)};
      const auto* bones=src+indices;const size_t at=size_t(input.first+n)*3;
      for(unsigned k=0;k<3;++k) {
        if(weights[k]<0||weights[k]>1||unsigned(bones[k])*3+60>=input.constants.size()) { out.reason="model_bone_weight_or_palette_range";return out; }
        out.skinning.weights[at+k]=weights[k];out.skinning.indices[at+k]=bones[k];used[bones[k]]=true;highest=(std::max)(highest,uint32_t(bones[k]));
      }
      float mapped[2]{};for(unsigned j=0;j<2;++j) {
        mapped[j]=t[0]*input.constants[48+j][0]+t[1]*input.constants[48+j][1]+input.constants[48+j][3];
        if(!std::isfinite(mapped[j])||std::abs(mapped[j])>1e8f) { out.reason="model_uv_nonfinite";return out; }
      }
      std::memcpy(dst,v,12);std::memcpy(dst+12,mapped,8);
    }
    out.skinning.transforms.resize(highest+1);
    for(uint32_t bone=0;bone<=highest;++bone) {
      auto& matrix=out.skinning.transforms[bone];matrix={1,0,0,0,0,1,0,0,0,0,1,0};
      if(!used[bone]) { continue; }
      for(unsigned row=0;row<3;++row) { for(unsigned col=0;col<4;++col) {
        const float value=input.constants[58+bone*3+row][col];
        if(!std::isfinite(value)||std::abs(value)>1e7f) { out.reason="model_bone_matrix_nonfinite";return out; }
        matrix[row*4+col]=value;
      } }
    }
    // Keep malformed palettes away from the runtime/BLAS boundary. This is a
    // temporary full CPU oracle for the correctness pilot; it can become a
    // sampled check after the runtime skinning layout is proven in game.
    for(uint32_t n=0;n<input.count;++n) {
      float positionValue[3]{},skinned[3]{};std::memcpy(positionValue,out.bytes.data()+size_t(input.first+n)*32,12);
      const size_t base=size_t(input.first+n)*3;
      for(unsigned k=0;k<3;++k) {
        const auto& matrix=out.skinning.transforms[out.skinning.indices[base+k]];const float weightValue=out.skinning.weights[base+k];
        for(unsigned row=0;row<3;++row) { skinned[row]+=(positionValue[0]*matrix[row*4]+positionValue[1]*matrix[row*4+1]+positionValue[2]*matrix[row*4+2]+matrix[row*4+3])*weightValue; }
      }
      for(float value:skinned) { if(!std::isfinite(value)||std::abs(value)>1e7f) { out.reason="gpu_skin_output_nonfinite";return out; } }
    }
    model_shader::Result camera;camera.valid=true;camera.world=out.world;
    if(!model_shader::cameraCompatible(camera,input.constants,s.view,s.projection)) { out.reason="model_clip_camera_mismatch";return out; }
    out.reason=profile==model_shader::Profile::Neko3?"original_vs_gpu_neko_three_weight":"original_vs_gpu_three_weight";return out;
  }
  auto transformed=model_shader::reconstruct(profile,input.constants,input.vertices,input.stride,unsigned(position),unsigned(uv),weight,indices,input.morph.positions);
  out.reason=transformed.reason;if (!transformed.valid) { return out; }
  if (!model_shader::cameraCompatible(transformed,input.constants,s.view,s.projection)) { out.reason="model_clip_camera_mismatch";return out; }
  if (!input.count || input.vertices.size()!=size_t(input.count)*32 || uint64_t(input.first)+input.count>model_shader::kMaxVertices) { out.reason="model_vertex_range";return out; }
  out.stride=32;out.world=transformed.world;out.sourceDeclaration=input.declaration;
  out.elements={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
  out.bytes.resize(size_t(input.first+input.count)*32);std::memcpy(out.bytes.data()+size_t(input.first)*32,transformed.vertices.data(),transformed.vertices.size());return out;
}
template<class Device> bool standardAlpha(Device* d) {
  DWORD src=0,dst=0,op=0,separate=0;
  return SUCCEEDED(d->GetRenderState(D3DRS_SRCBLEND,&src))&&SUCCEEDED(d->GetRenderState(D3DRS_DESTBLEND,&dst))&&
    SUCCEEDED(d->GetRenderState(D3DRS_BLENDOP,&op))&&SUCCEEDED(d->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,&separate))&&
    src==D3DBLEND_SRCALPHA&&dst==D3DBLEND_INVSRCALPHA&&op==D3DBLENDOP_ADD&&!separate;
}
inline scene_input::Prepared prepare(IDirect3DDevice9* d,const draw_observe::Snapshot& s,uint32_t kind,D3DPRIMITIVETYPE type,const std::array<uint32_t,6>& args,scene_input::Data* evidence=nullptr,bool cpuOnly=false) {
  runtime_profile::Scope timing(runtime_profile::Stage::Prepare);
  DWORD values[7]{};scene_input::Prepared rejected;
  const D3DRENDERSTATETYPE names[]={D3DRS_ALPHABLENDENABLE,D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_STENCILENABLE,D3DRS_CLIPPLANEENABLE,D3DRS_COLORWRITEENABLE,D3DRS_ZFUNC};
  for (unsigned i=0;i<7;++i) {
    const auto hr=d->GetRenderState(names[i],values+i);if (FAILED(hr)) { rejected.result=hr;rejected.reason="model_pipeline_query_failed";return rejected; }
  }
  if (values[1]!=1 || values[2]>1 || (!values[0]&&!values[2]) || (values[0]&&(!standardAlpha(d)||!std::strcmp(s.materialInfo.shader.data(),"Infected")))) { rejected.reason="model_pipeline_unsupported";return rejected; }
  if (values[3] || values[4] || (values[5]&7)!=7 || (values[5]&~15u) || (values[6]!=D3DCMP_LESS && values[6]!=D3DCMP_LESSEQUAL)) { rejected.reason="model_pipeline_special_pass";return rejected; }
  auto input=scene_input::capture(d,kind,type,args,false,scene_input::kModelLimits);
  const auto digest=ldb_rt_file::bytesSha256(input.vs.data(),input.vs.size()*4);
  if (model_shader::profile(digest)!=model_shader::Profile::Unknown && model_shader::profile(digest)!=model_shader::Profile::InfectedRigid && SUCCEEDED(input.result) && input.constants[13][0]!=0) { scene_input::morphPositions(d,input); }
  auto out=reconstruct(s,input,digest);if (!out.bytes.empty() && ownDraw(out,input,kind,args) && (cpuOnly || scene_input::upload(d,out))) { out.ready=true; }
  if (evidence && out.ready) { *evidence=std::move(input); }return out;
}
} // namespace ldb_rt::model_input
