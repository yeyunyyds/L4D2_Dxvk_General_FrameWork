#pragma once
#include "rt_file_hash.h"
#include "rt_static_draw.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

namespace ldb_rt::model_texture {
constexpr size_t kMaxPayloadBytes=16u*1024*1024,kMaxMipPayloadBytes=24u*1024*1024,kDdsHeaderBytes=148,kMaxDdsBytes=kMaxMipPayloadBytes+kDdsHeaderBytes;
inline std::string ddsSha256(const std::vector<uint8_t>& encoded) {
  // Geometry's 4 MiB hash bound is smaller than supported textures, even a
  // 1024x1024 BGRA8 base level once the DDS header is included.
  return ldb_rt_file::bytesSha256(encoded.data(),encoded.size(),kMaxDdsBytes);
}
struct Layout { uint32_t dxgi=0,rowBytes=0,rows=0;bool compressed=false,opaqueAlpha=false;uint64_t bytes() const { return uint64_t(rowBytes)*rows; } };
inline Layout layout(UINT width,UINT height,D3DFORMAT format,bool srgb) {
  Layout out;if(!width||!height||width>4096||height>4096) { return out; }
  unsigned block=0;
  switch(format) {
    case D3DFMT_DXT1:out.dxgi=srgb?72u:71u;block=8;break;
    case D3DFMT_DXT3:out.dxgi=srgb?75u:74u;block=16;break;
    case D3DFMT_DXT5:out.dxgi=srgb?78u:77u;block=16;break;
    case D3DFMT_A8R8G8B8:out.dxgi=srgb?91u:87u;break;
    case D3DFMT_X8R8G8B8:out.dxgi=srgb?91u:87u;out.opaqueAlpha=true;break;
    default:return out;
  }
  out.compressed=block!=0;out.rowBytes=block?((width+3)/4)*block:width*4;out.rows=block?(height+3)/4:height;
  if(out.bytes()>kMaxPayloadBytes) { return {}; }return out;
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
  for(UINT row=0;row<l.rows;++row) { std::memcpy(out.data()+148+size_t(row)*l.rowBytes,static_cast<const uint8_t*>(data)+size_t(row)*size_t(pitch),l.rowBytes);
    if(l.opaqueAlpha) { for(UINT x=0;x<width;++x) { out[148+size_t(row)*l.rowBytes+size_t(x)*4+3]=255; } }
  }
  return out;
}
inline UINT fullLevels(UINT width,UINT height) {
  UINT levels=1;while(width>1||height>1) { width=std::max(1u,width/2);height=std::max(1u,height/2);++levels; }return levels;
}
inline uint64_t mipBytes(UINT width,UINT height,D3DFORMAT format,bool srgb,UINT levels) {
  if(!levels||levels>fullLevels(width,height)) { return 0; }uint64_t bytes=0;
  for(UINT level=0;level<levels;++level) { const auto l=layout(width,height,format,srgb);if(!l.dxgi) { return 0; }
    bytes+=l.bytes();width=std::max(1u,width/2);height=std::max(1u,height/2); }
  return bytes<=kMaxMipPayloadBytes?bytes:0;
}
template<class Texture> struct ReadLock {
  Texture* texture;UINT level;D3DLOCKED_RECT rect{};HRESULT result;bool active;
  explicit ReadLock(Texture* value,UINT at=0):texture(value),level(at),result(value->LockRect(at,&rect,nullptr,D3DLOCK_READONLY)),active(SUCCEEDED(result)) {}
  HRESULT finish() { if(active) { active=false;return texture->UnlockRect(level); }return result; }
  ~ReadLock() { (void)finish(); }
};
template<class Texture> std::vector<uint8_t> mipDds(Texture* texture,const D3DSURFACE_DESC& base,bool srgb,UINT levels) {
  const auto bytes=mipBytes(base.Width,base.Height,base.Format,srgb,levels);if(!bytes) { return {}; }
  std::vector<uint8_t> out;out.reserve(size_t(bytes)+kDdsHeaderBytes);UINT width=base.Width,height=base.Height;
  for(UINT level=0;level<levels;++level) {
    D3DSURFACE_DESC desc{};
    if(FAILED(texture->GetLevelDesc(level,&desc))||desc.Width!=width||desc.Height!=height||desc.Format!=base.Format||desc.Usage!=base.Usage) { return {}; }
    ReadLock<Texture> lock(texture,level);if(FAILED(lock.result)) { return {}; }
    auto encoded=dds(width,height,layout(width,height,base.Format,srgb),lock.rect.pBits,lock.rect.Pitch);
    if(FAILED(lock.finish())||encoded.empty()) { return {}; }
    out.insert(out.end(),encoded.begin()+(level?kDdsHeaderBytes:0),encoded.end());width=std::max(1u,width/2);height=std::max(1u,height/2);
  }
  uint32_t header[37]{};std::memcpy(header,out.data(),sizeof(header));header[7]=levels;
  if(levels>1) { header[2]|=0x20000;header[27]|=0x400008; }std::memcpy(out.data(),header,sizeof(header));return out;
}
struct Source {
  std::string key="synthetic",digest,file,reason="unavailable";std::filesystem::path path;
  uint32_t width=0,height=0,format=0,levels=0,fullMipLevels=0;bool evidenceSaved=false;bool srgb=false;uint8_t alphaTest=7,alphaRef=0,wrapU=1,wrapV=1,filter=1;
  bool textured() const { return !path.empty(); }
};
inline bool wrap(DWORD value,uint8_t& out) {
  switch(value) { case D3DTADDRESS_WRAP:out=1;return true;case D3DTADDRESS_MIRROR:out=2;return true;
    case D3DTADDRESS_CLAMP:out=0;return true;default:return false; }
}
class Cache {
  struct Entry { IDirect3DTexture9* texture=nullptr;Source source;uint64_t last=0,cost=0;~Entry() { if(texture) { texture->Release(); } } };
  std::map<std::pair<uintptr_t,bool>,std::unique_ptr<Entry>> entries;
  std::map<std::string,uint64_t> spool;
  uint64_t completed=0;
  uint64_t frame=0,frameBytes=0;unsigned frameReads=0;
public:
  uint64_t bytes=0,exported=0,failed=0,deferred=0,residentBytes=0,evidenceBytes=0,evidenceFiles=0,evicted=0;
  static constexpr uint64_t maxBytes=512ull*1024*1024,maxResidentBytes=128ull*1024*1024,maxEvidenceBytes=64ull*1024*1024;
  struct Limits { uint64_t resident=maxResidentBytes,spoolBytes=maxBytes,evidence=maxEvidenceBytes;size_t refs=256,files=1024; };
private:
  Limits limits;
public:
  Cache():limits{} {}explicit Cache(Limits policy):limits(policy) {}
  void completedThrough(uint64_t value) { completed=value; }
  void clear() { entries.clear();residentBytes=0; }
  bool reserve(uint64_t cost,uint64_t now) {
    if(cost>limits.resident) { return false; }
    while(entries.size()>=limits.refs||cost>limits.resident-residentBytes) {
      auto oldest=entries.end();
      for(auto it=entries.begin();it!=entries.end();++it) {
        const auto last=it->second->last;
        if(last<=completed&&now>=last&&now-last>=2&&(oldest==entries.end()||last<oldest->second->last)) { oldest=it; }
      }
      if(oldest==entries.end()) { return false; }
      residentBytes-=oldest->second->cost;entries.erase(oldest);++evicted;
    }return true;
  }
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
      found->second->last=at;const auto& cached=found->second->source;s.evidenceSaved=cached.evidenceSaved;s.path=cached.path;s.file=cached.file;s.digest=cached.digest;s.width=cached.width;s.height=cached.height;s.format=cached.format;s.reason=cached.reason;s.levels=cached.levels;s.fullMipLevels=cached.fullMipLevels;
      s.key=(s.textured()?s.digest:s.key)+suffix;return s;
    }
    if(frame!=at) { frame=at;frameBytes=0;frameReads=0; }
    D3DSURFACE_DESC desc{};const auto described=texture->GetLevelDesc(0,&desc);
    const auto l=layout(desc.Width,desc.Height,desc.Format,s.srgb);
    if(FAILED(described)||!l.dxgi||(desc.Usage&(D3DUSAGE_DYNAMIC|D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL))) { s.reason="base_texture_format_or_usage_unsupported";s.key+=suffix;++failed;return s; }
    s.width=desc.Width;s.height=desc.Height;s.format=uint32_t(desc.Format);s.levels=texture->GetLevelCount();s.fullMipLevels=fullLevels(desc.Width,desc.Height);
    const auto payload=mipBytes(desc.Width,desc.Height,desc.Format,s.srgb,s.levels);
    if(!payload) { s.reason="base_texture_mip_layout_unsupported";s.key+=suffix;++failed;return s; }
    if(frameReads>=4||payload+148>(kMaxMipPayloadBytes+4*148)-frameBytes) { s.reason="texture_frame_budget";s.key+=suffix;++deferred;return s; }
    if(!reserve(payload+148,at)) { s.reason="texture_cache_budget";s.key+=suffix;++deferred;return s; }
    ++frameReads;auto encoded=mipDds(texture,desc,s.srgb,s.levels);
    auto entry=std::make_unique<Entry>();texture->AddRef();entry->texture=texture;entry->last=at;
    if(encoded.empty()) { s.reason="base_texture_read_unavailable";++failed; }
    else {
      s.digest=ddsSha256(encoded);
      if(s.digest.empty()) { throw std::runtime_error("texture hash failed"); }
      const auto size=uint64_t(encoded.size());const bool exists=spool.count(s.digest)!=0;
      // Runtime paths remain until process exit: async backend loaders may still
      // read a file after a source COM reference or API material was retired.
      if(!exists&&(size>limits.spoolBytes-bytes||spool.size()>=limits.files)) {
        s.digest.clear();s.reason="texture_spool_budget";s.key+=suffix;++deferred;return s;
      }
      std::filesystem::create_directories(directory/"runtime-textures");
      s.file="model-textures/"+s.digest+".dds";s.path=directory/"runtime-textures"/(s.digest+".dds");
      if(!exists) {
        if(std::filesystem::exists(s.path)) { throw std::runtime_error("untracked runtime texture path"); }
        std::ofstream output(s.path,std::ios::binary);output.write(reinterpret_cast<const char*>(encoded.data()),std::streamsize(encoded.size()));output.close();
        if(!output) { throw std::runtime_error("write runtime texture"); }
        spool.emplace(s.digest,size);bytes+=size;++exported;
      } else if(ldb_rt_file::fileSha256(s.path)!=s.digest) { throw std::runtime_error("existing runtime texture differs"); }
      const auto evidence=directory/s.file;
      if(std::filesystem::exists(evidence)) {
        if(ldb_rt_file::fileSha256(evidence)!=s.digest) { throw std::runtime_error("existing texture evidence differs"); }s.evidenceSaved=true;
      } else if(size<=limits.evidence-evidenceBytes) {
        std::filesystem::create_directories(evidence.parent_path());
        std::filesystem::copy_file(s.path,evidence);evidenceBytes+=size;++evidenceFiles;s.evidenceSaved=true;
      }
      s.reason="sampler0_source_mips_dds_dx10";frameBytes+=size;entry->cost=size;residentBytes+=size;

    }
    entry->source=s;entries.emplace(id,std::move(entry));s.key=(s.textured()?s.digest:s.key)+suffix;return s;
  }
};
} // namespace ldb_rt::model_texture
