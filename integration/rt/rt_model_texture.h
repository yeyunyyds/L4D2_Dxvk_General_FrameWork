#pragma once
#include "rt_file_hash.h"
#include "rt_static_draw.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

namespace ldb_rt::model_texture {
struct Layout { uint32_t dxgi=0,rowBytes=0,rows=0;bool compressed=false;uint64_t bytes() const { return uint64_t(rowBytes)*rows; } };
inline Layout layout(UINT width,UINT height,D3DFORMAT format,bool srgb) {
  Layout out;if(!width||!height||width>4096||height>4096) { return out; }
  unsigned block=0;
  switch(format) {
    case D3DFMT_DXT1:out.dxgi=srgb?72u:71u;block=8;break;
    case D3DFMT_DXT3:out.dxgi=srgb?75u:74u;block=16;break;
    case D3DFMT_DXT5:out.dxgi=srgb?78u:77u;block=16;break;
    case D3DFMT_A8R8G8B8:out.dxgi=srgb?91u:87u;break;
    case D3DFMT_X8R8G8B8:out.dxgi=srgb?93u:88u;break;
    default:return out;
  }
  out.compressed=block!=0;out.rowBytes=block?((width+3)/4)*block:width*4;out.rows=block?(height+3)/4:height;
  if(out.bytes()>16ull*1024*1024) { return {}; }return out;
}
inline std::vector<uint8_t> dds(UINT width,UINT height,const Layout& l,const void* data,int pitch) {
  if(!l.dxgi||!data||pitch<0||pitch>1024*1024||uint32_t(pitch)<l.rowBytes) { return {}; }
  // DDS DX10 explicitly preserves the source sampler's sRGB/linear meaning.
  std::vector<uint8_t> out(size_t(148+l.bytes()),0);uint32_t header[37]{};
  header[0]=0x20534444;header[1]=124;header[2]=0x1007|(l.compressed?0x80000:8);
  header[3]=height;header[4]=width;header[5]=l.compressed?uint32_t(l.bytes()):l.rowBytes;header[7]=1;
  header[19]=32;header[20]=4;header[21]=0x30315844;header[27]=0x1000;
  header[32]=l.dxgi;header[33]=3;header[35]=1;
  std::memcpy(out.data(),header,sizeof(header));
  for(UINT row=0;row<l.rows;++row) { std::memcpy(out.data()+148+size_t(row)*l.rowBytes,static_cast<const uint8_t*>(data)+size_t(row)*size_t(pitch),l.rowBytes); }
  return out;
}
template<class Texture> struct ReadLock {
  Texture* texture;D3DLOCKED_RECT rect{};HRESULT result;bool active;
  explicit ReadLock(Texture* value):texture(value),result(value->LockRect(0,&rect,nullptr,D3DLOCK_READONLY)),active(SUCCEEDED(result)) {}
  HRESULT finish() { if(active) { active=false;return texture->UnlockRect(0); }return result; }
  ~ReadLock() { (void)finish(); }
};
struct Source {
  std::string key="synthetic",digest,file,reason="unavailable";std::filesystem::path path;
  uint32_t width=0,height=0,format=0;bool srgb=false;uint8_t alphaTest=7,alphaRef=0,wrapU=1,wrapV=1,filter=1;
  bool textured() const { return !path.empty(); }
};
inline bool wrap(DWORD value,uint8_t& out) {
  switch(value) { case D3DTADDRESS_WRAP:out=1;return true;case D3DTADDRESS_MIRROR:out=2;return true;
    case D3DTADDRESS_CLAMP:out=0;return true;default:return false; }
}
class Cache {
  struct Entry { IDirect3DTexture9* texture=nullptr;Source source;~Entry() { if(texture) { texture->Release(); } } };
  std::map<std::pair<uintptr_t,bool>,std::unique_ptr<Entry>> entries;
  uint64_t frame=0,frameBytes=0;unsigned frameReads=0;
public:
  uint64_t bytes=0,exported=0,failed=0,deferred=0;
  static constexpr uint64_t maxBytes=64ull*1024*1024;
  void clear() { entries.clear(); }
  size_t held() const { return entries.size(); }
  Source acquire(IDirect3DDevice9* d,const std::filesystem::path& directory,uint64_t at,uint32_t material) {
    Source s;s.key="fallback:"+std::to_string(material);
    DWORD alpha=0,func=D3DCMP_ALWAYS,ref=0,srgb=0,u=0,v=0,min=0,mag=0;
    const HRESULT queries[]={d->GetRenderState(D3DRS_ALPHATESTENABLE,&alpha),d->GetRenderState(D3DRS_ALPHAFUNC,&func),
      d->GetRenderState(D3DRS_ALPHAREF,&ref),d->GetSamplerState(0,D3DSAMP_SRGBTEXTURE,&srgb),d->GetSamplerState(0,D3DSAMP_ADDRESSU,&u),
      d->GetSamplerState(0,D3DSAMP_ADDRESSV,&v),d->GetSamplerState(0,D3DSAMP_MINFILTER,&min),d->GetSamplerState(0,D3DSAMP_MAGFILTER,&mag)};
    for(auto q:queries) { if(FAILED(q)) { s.reason="texture_state_query_failed";return s; } }
    if(func<1||func>8||ref>255||!wrap(u,s.wrapU)||!wrap(v,s.wrapV)||
       (min!=D3DTEXF_POINT&&min!=D3DTEXF_LINEAR&&min!=D3DTEXF_ANISOTROPIC)||
       (mag!=D3DTEXF_POINT&&mag!=D3DTEXF_LINEAR&&mag!=D3DTEXF_ANISOTROPIC)) { s.reason="texture_sampler_unsupported";return s; }
    s.srgb=srgb!=0;s.alphaTest=alpha?uint8_t(func-1):uint8_t(7);s.alphaRef=alpha?uint8_t(ref):uint8_t(0);s.filter=uint8_t(min!=D3DTEXF_POINT||mag!=D3DTEXF_POINT);
    const auto suffix=":"+std::to_string(s.alphaTest)+":"+std::to_string(s.alphaRef)+":"+std::to_string(s.wrapU)+":"+std::to_string(s.wrapV)+":"+std::to_string(s.filter);
    static_draw::Ref<IDirect3DBaseTexture9> base;
    if(FAILED(d->GetTexture(0,&base.value))||!base.value||base.value->GetType()!=D3DRTYPE_TEXTURE) { s.reason="base_texture_unavailable";s.key+=suffix;return s; }
    auto* texture=static_cast<IDirect3DTexture9*>(base.value);const auto id=std::make_pair(reinterpret_cast<uintptr_t>(texture),s.srgb);
    const auto found=entries.find(id);
    if(found!=entries.end()) {
      const auto& cached=found->second->source;s.path=cached.path;s.file=cached.file;s.digest=cached.digest;s.width=cached.width;s.height=cached.height;s.format=cached.format;s.reason=cached.reason;
      s.key=(s.textured()?s.digest:s.key)+suffix;return s;
    }
    if(frame!=at) { frame=at;frameBytes=0;frameReads=0; }
    D3DSURFACE_DESC desc{};const auto described=texture->GetLevelDesc(0,&desc);
    const auto l=layout(desc.Width,desc.Height,desc.Format,s.srgb);
    if(FAILED(described)||!l.dxgi||(desc.Usage&(D3DUSAGE_DYNAMIC|D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL))) { s.reason="base_texture_format_or_usage_unsupported";s.key+=suffix;++failed;return s; }
    s.width=desc.Width;s.height=desc.Height;s.format=uint32_t(desc.Format);
    if(entries.size()>=128||l.bytes()+148>maxBytes-bytes) { s.reason="texture_cache_budget";s.key+=suffix;++deferred;return s; }
    if(frameReads>=4||l.bytes()+148>(16ull*1024*1024+4*148)-frameBytes) { s.reason="texture_frame_budget";s.key+=suffix;++deferred;return s; }
    ++frameReads;ReadLock<IDirect3DTexture9> lock(texture);
    std::vector<uint8_t> encoded;
    if(SUCCEEDED(lock.result)) { encoded=dds(desc.Width,desc.Height,l,lock.rect.pBits,lock.rect.Pitch);if(FAILED(lock.finish())) { encoded.clear(); } }
    auto entry=std::make_unique<Entry>();texture->AddRef();entry->texture=texture;
    if(encoded.empty()) { s.reason="base_texture_read_unavailable";++failed; }
    else {
      s.digest=ldb_rt_file::geometrySha256(encoded.data(),encoded.size());
      if(s.digest.empty()) { throw std::runtime_error("texture hash failed"); }
      std::filesystem::create_directories(directory/"model-textures");s.file="model-textures/"+s.digest+".dds";s.path=directory/s.file;
      if(!std::filesystem::exists(s.path)) { std::ofstream output(s.path,std::ios::binary);output.write(reinterpret_cast<const char*>(encoded.data()),std::streamsize(encoded.size()));output.close();if(!output) { throw std::runtime_error("write model texture"); } }
      else if(ldb_rt_file::fileSha256(s.path)!=s.digest) { throw std::runtime_error("existing model texture differs"); }
      s.reason="sampler0_base_level_dds_dx10";bytes+=encoded.size();frameBytes+=encoded.size();++exported;
    }
    entry->source=s;entries.emplace(id,std::move(entry));s.key=(s.textured()?s.digest:s.key)+suffix;return s;
  }
};
} // namespace ldb_rt::model_texture
