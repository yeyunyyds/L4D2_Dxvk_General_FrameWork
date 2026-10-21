#pragma once
#include "rt_model_texture.h"
#include <array>
#include <iomanip>

// Exact 141352 Infected PS diffuse dependency, before vertex lighting, AO and
// fog. This is a bounded base-level bake, not execution of arbitrary shaders.
namespace ldb_rt::infected_material {
using Vec=std::array<float,4>;
inline float clamp(float x) { return std::max(0.f,std::min(1.f,x)); }
struct Image {
  std::vector<uint8_t> raw;unsigned width=0,height=0,format=0;bool srgb=false;
  uint8_t wrapU=1,wrapV=1,filter=1;
  bool load(const model_texture::Source& s) {
    const auto size=std::filesystem::file_size(s.path);
    if(size<148||size>model_texture::kMaxDdsBytes) { return false; }
    raw.resize(size_t(size));std::ifstream f(s.path,std::ios::binary);f.read(reinterpret_cast<char*>(raw.data()),std::streamsize(size));
    uint32_t h[37]{};std::memcpy(h,raw.data(),148);width=s.width;height=s.height;format=h[32];srgb=s.srgb;wrapU=s.wrapU;wrapV=s.wrapV;filter=s.filter;
    const unsigned block=(format==71||format==72)?8:(format==74||format==75||format==77||format==78)?16:0;
    return bool(f)&&h[0]==0x20534444&&h[3]==height&&h[4]==width&&width&&height&&width<=4096&&height<=4096&&
      (block||format==87||format==91)&&size>=148+(block?size_t((width+3)/4)*((height+3)/4)*block:size_t(width)*height*4);
  }
  static Vec rgb565(uint16_t a) { return {float((a>>11)&31)/31.f,float((a>>5)&63)/63.f,float(a&31)/31.f,1}; }
  static int address(int x,unsigned n,uint8_t wrap) {
    if(wrap==0) { return std::max(0,std::min(int(n)-1,x)); }
    const int period=int(n)*(wrap==2?2:1);x%=period;if(x<0) { x+=period; }
    return wrap==2&&x>=int(n)?period-1-x:x;
  }
  Vec pixel(int ix,int iy) const {
    const auto x=unsigned(address(ix,width,wrapU)),y=unsigned(address(iy,height,wrapV));Vec out{};
    if(format==87||format==91) { const auto* p=raw.data()+148+(size_t(y)*width+x)*4;out={p[2]/255.f,p[1]/255.f,p[0]/255.f,p[3]/255.f}; }
    else {
      const bool bc1=format==71||format==72,bc2=format==74||format==75;const unsigned cell=(y%4)*4+x%4;
      const auto* p=raw.data()+148+(size_t(y/4)*((width+3)/4)+x/4)*(bc1?8:16);const auto* color=p+(bc1?0:8);
      uint16_t a=0,b=0;uint32_t codes=0;std::memcpy(&a,color,2);std::memcpy(&b,color+2,2);std::memcpy(&codes,color+4,4);
      Vec palette[4]={rgb565(a),rgb565(b),{},{} };
      for(unsigned k=0;k<3;++k) {
        palette[2][k]=(bc1&&a<=b)?(palette[0][k]+palette[1][k])*.5f:(2*palette[0][k]+palette[1][k])/3;
        palette[3][k]=(bc1&&a<=b)?0.f:(palette[0][k]+2*palette[1][k])/3;
      }
      palette[2][3]=1;palette[3][3]=(bc1&&a<=b)?0.f:1.f;out=palette[(codes>>(cell*2))&3];
      if(bc2) { out[3]=float((p[cell/2]>>((cell%2)*4))&15)/15.f; }
      else if(!bc1) {
        float alpha[8]={p[0]/255.f,p[1]/255.f};
        for(unsigned k=2;k<8;++k) { alpha[k]=p[0]>p[1]?(float(8-k)*alpha[0]+float(k-1)*alpha[1])/7:
          k<6?(float(6-k)*alpha[0]+float(k-1)*alpha[1])/5:k==6?0.f:1.f; }
        uint64_t bits=0;for(unsigned k=0;k<6;++k) { bits|=uint64_t(p[2+k])<<(8*k); }out[3]=alpha[(bits>>(cell*3))&7];
      }
    }
    if(srgb) { for(unsigned k=0;k<3;++k) { const float c=out[k];out[k]=c<=.04045f?c/12.92f:std::pow((c+.055f)/1.055f,2.4f); } }
    return out;
  }
  Vec sample(float u,float v) const {
    // Inputs are bounded before baking; reducing first avoids integer overflow.
    const auto reduce=[](float t,uint8_t wrap) { return wrap?std::fmod(t,wrap==2?2.f:1.f):clamp(t); };
    u=reduce(u,wrapU)*float(width);v=reduce(v,wrapV)*float(height);
    if(!filter) { return pixel(int(std::floor(u)),int(std::floor(v))); }
    u-=.5f;v-=.5f;const int x=int(std::floor(u)),y=int(std::floor(v));const float a=u-float(x),b=v-float(y);
    const auto p=pixel(x,y),q=pixel(x+1,y),r=pixel(x,y+1),s=pixel(x+1,y+1);Vec out{};
    for(unsigned k=0;k<4;++k) { out[k]=(p[k]*(1-a)+q[k]*a)*(1-b)+(r[k]*(1-a)+s[k]*a)*b; }return out;
  }
};
struct Parameters { Vec range{},tint{},atlas{},detailU{},detailV{}; };
inline bool valid(const Parameters& p) {
  for(const auto* row:{&p.range,&p.tint,&p.atlas,&p.detailU,&p.detailV}) { for(float v:*row) { if(!std::isfinite(v)||std::abs(v)>1e4f) { return false; } } }
  return p.range[1]-p.range[0]>1e-6f;
}
inline Vec diffuse(const Vec& encoded,const Vec& second,const Vec& paletteA,const Vec& paletteB,const Vec& detail,const Parameters& p) {
  // SM3 words223/226 are MOV_SAT: channel permutation and clamping
  // are part of the material contract, not optional color correction.
  const float x=2*encoded[2]-1,y=clamp(2*second[0]-1),z=clamp(2*second[1]-1),alpha2=encoded[3]*encoded[3];
  float t=clamp((z-p.range[0])/(p.range[1]-p.range[0]));t=clamp(t*t*(3-2*t));Vec out{0,0,0,1};
  for(unsigned k=0;k<3;++k) {
    const float mixed=clamp(x)*(paletteA[k]-alpha2)+alpha2;
    const float blend=clamp(-x);const float base=blend*paletteB[k]+(1-blend)*mixed;
    out[k]=(base+y*(2*alpha2*detail[k]-base))*(1+t*(p.tint[k]-1));
  }return out;
}
inline std::vector<uint8_t> bake(const Image& base,const Image& detail,const Image& palette,const Parameters& p,unsigned width,unsigned height) {
  if(!valid(p)||!width||!height||width>256||height>256) { return {}; }
  std::vector<uint8_t> pixels(size_t(width)*height*4);
  for(unsigned y=0;y<height;++y) { for(unsigned x=0;x<width;++x) {
    const float u=(float(x)+.5f)/float(width),v=(float(y)+.5f)/float(height);const auto a=base.sample(u,v),b=base.sample(.5f*u+p.atlas[0],.5f*v+p.atlas[1]);
    const auto color=diffuse(a,b,palette.sample(a[3],p.atlas[2]),palette.sample(a[3],p.atlas[3]),
      detail.sample(u*p.detailV[0]+v*p.detailV[1],u*p.detailU[0]+v*p.detailU[1]),p);
    auto* dst=pixels.data()+(size_t(y)*width+x)*4;
    for(unsigned k=0;k<3;++k) { if(!std::isfinite(color[k])) { return {}; }dst[2-k]=uint8_t(std::lround(clamp(color[k])*255)); }dst[3]=255;
  } }
  auto out=model_texture::dds(width,height,model_texture::layout(width,height,D3DFMT_A8R8G8B8,false),pixels.data(),int(width*4));
  const unsigned levels=model_texture::fullLevels(width,height);
  while(width>1||height>1) {
    const unsigned w=std::max(1u,width/2),h=std::max(1u,height/2);std::vector<uint8_t> next(size_t(w)*h*4);
    for(unsigned y=0;y<h;++y) { for(unsigned x=0;x<w;++x) { for(unsigned k=0;k<4;++k) {
      unsigned sum=0;for(unsigned j=0;j<2;++j) { for(unsigned i=0;i<2;++i) { sum+=pixels[(size_t(std::min(height-1,y*2+j))*width+std::min(width-1,x*2+i))*4+k]; } }
      next[(size_t(y)*w+x)*4+k]=uint8_t((sum+2)/4);
    } } }out.insert(out.end(),next.begin(),next.end());pixels.swap(next);width=w;height=h;
  }
  uint32_t h[37]{};std::memcpy(h,out.data(),148);h[7]=levels;if(levels>1) { h[2]|=0x20000;h[27]|=0x400008; }std::memcpy(out.data(),h,148);return out;
}
class Cache {
  std::map<std::string,model_texture::Source> entries;uint64_t lastBake=0;
public:
  uint64_t baked=0,deferred=0,failed=0;
  model_texture::Source acquire(IDirect3DDevice9* d,model_texture::Cache& sources,const std::filesystem::path& directory,uint64_t frame,uint32_t material) {
    model_texture::Source out;out.key="infected-neutral:"+std::to_string(material);out.reason="infected_dependencies_unavailable";
    const auto base=sources.acquire(d,directory,frame,material,0,true),detail=sources.acquire(d,directory,frame,material,4,true),palette=sources.acquire(d,directory,frame,material,5,true);
    Parameters p{};float vs[8]{};
    if(FAILED(d->GetPixelShaderConstantF(3,p.range.data(),1))||FAILED(d->GetPixelShaderConstantF(4,p.tint.data(),1))||
       FAILED(d->GetPixelShaderConstantF(10,p.atlas.data(),1))||FAILED(d->GetVertexShaderConstantF(50,vs,2))) { ++failed;out.reason="infected_constant_query_failed";return out; }
    std::memcpy(p.detailU.data(),vs,16);std::memcpy(p.detailV.data(),vs+4,16);
    p.range[2]=p.range[3]=p.tint[3]=p.detailU[2]=p.detailU[3]=p.detailV[2]=p.detailV[3]=0;
    if(!valid(p)) { ++failed;out.reason="infected_material_constants_unsupported";return out; }
    if(!base.textured()||!detail.textured()||!palette.textured()) { ++deferred;return out; }
    const auto constants=ldb_rt_file::bytesSha256(&p,sizeof(p));
    const auto key=base.key+":"+detail.key+":"+palette.key+":"+constants;
    const auto found=entries.find(key);if(found!=entries.end()) { return found->second; }
    if(lastBake==frame||entries.size()>=128) { ++deferred;out.reason="infected_bake_budget";return out; }
    lastBake=frame;Image a,b,c;
    if(!a.load(base)||!b.load(detail)||!c.load(palette)) { ++failed;out.reason="infected_dependency_decode_failed";return out; }
    const unsigned width=std::min(256u,base.width),height=std::min(256u,base.height);const auto encoded=bake(a,b,c,p,width,height);
    if(encoded.empty()) { ++failed;out.reason="infected_bake_failed";return out; }
    out.width=width;out.height=height;out.format=uint32_t(D3DFMT_A8R8G8B8);out.levels=out.fullMipLevels=model_texture::fullLevels(width,height);
    out.wrapU=base.wrapU;out.wrapV=base.wrapV;out.filter=base.filter;
    if(!sources.persist(out,encoded,directory,true)) { ++deferred;return out; }
    std::ofstream proof(directory/"infected-materials.jsonl",std::ios::app);proof<<std::setprecision(9);
    proof<<"{\"event\":\"infected_diffuse_bake\",\"frame\":"<<frame<<",\"material\":"<<material<<",\"output_sha256\":\""<<out.digest<<"\",\"output_file\":\""<<out.file<<"\",\"output_evidence_saved\":"<<(out.evidenceSaved?"true":"false")<<",\"parameters\":[";
    bool comma=false;for(const auto* row:{&p.range,&p.tint,&p.atlas,&p.detailU,&p.detailV}) { for(float v:*row) { if(comma) { proof<<','; }comma=true;proof<<v; } }
    proof<<"],\"inputs\":[";comma=false;unsigned slot=0;
    for(const auto* input:{&base,&detail,&palette}) { if(comma) { proof<<','; }comma=true;
      proof<<"{\"slot\":"<<slot<<",\"sha256\":\""<<input->digest<<"\",\"file\":\""<<input->file<<"\",\"evidence_saved\":"<<(input->evidenceSaved?"true":"false")
        <<",\"srgb\":"<<(input->srgb?"true":"false")<<",\"wrap_u\":"<<unsigned(input->wrapU)<<",\"wrap_v\":"<<unsigned(input->wrapV)<<",\"filter\":"<<unsigned(input->filter)<<'}';slot=slot==0?4:5;
    }proof<<"]}\n";proof.close();if(!proof) { throw std::runtime_error("write infected bake recipe"); }
    out.key="infected-bake:"+key;out.reason="infected_palette_detail_diffuse_linear_opaque_256";entries.emplace(key,out);++baked;return out;
  }
};
} // namespace ldb_rt::infected_material
