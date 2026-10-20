#pragma once
#include "rt_sky_shader.h"
#include "rt_scene_draw.h"
#include <string>

namespace ldb_rt::model_shader {
constexpr unsigned kMaxVertices=32768;
// Complete SM3 programs audited in the 191200, 204924, 004208 and 141352 captures. A material name or
// stride does not establish their position/UV contract. Unknown code falls back.
enum class Profile { Unknown, Rigid, Bone0, Skin3, Neko3, InfectedRigid };
inline Profile profile(const std::string& sha) {
  if(sha=="e9f1650bedd1575e46c486f522d3d0bb8a40e1abb8d78eb0d281daf93a267d72") { return Profile::InfectedRigid; }
  if (sha=="b9081b1aa48487b133f168edcf34a302ad06d9963e1bedab8d9dd62d98fbc606" ||
      sha=="803be4995dac57ebc289dbcc97004a6b1f4c8ad616c7db8dc39f41c200663706" ||
      sha=="1028c5ef031a8f9763c878ac9514dba6e628f87d3746ec373f4fc3b3e4486775" ||
      sha=="3a9ebc14b4895b0614690472b2500c46c505ff282e6868472849900975493e41" ||
      sha=="3566b4c8bed75da732f7c8cdb1bce1bb1b5025c932916b2d739ec748f9b8526b" ||
      sha=="dbb52fd1bc05cfef85a1ddf3a4e1943b3e9001182934ee4df94289cd7e27b91e") { return Profile::Rigid; }
  if (sha=="f29d14d0f408c16686dbba5b391c48a8e7445de0d47162b7118d0baaca5519a3") { return Profile::Neko3; }
  for (const char* known:{"d7f9daf0698882776cffeb478a6bfdf2a480ade55a7bc659bd76b36c589d5174",
      "3c653c42a2ce6a7b2201a5b26af6cee4b501625b2d32f1063caafa78d9261d2b","84e1a3321dd4b0e7630372d7c229e37cadea38db2833da73cbfdd5bc8b7dce0d",
      "8eb1c8546fb7d3536f008f1e09998cf034849f5f67f23fc1d2fa61cea69774f9",
      "149c60569507ca934d0374759c8f998fa1ce0794937375a7a21b55549f8c7089",
      "76ed20af53d3a4e9c4bf430e99390c109159189292c0c91bd5abe026a0ecbb4b"}) {
    if (sha==known) { return Profile::Skin3; }
  }return Profile::Unknown;
}
struct Result {
  bool valid=false;const char* reason="unknown_model_program";
  std::array<float,16> world{};
  std::vector<uint8_t> vertices;
};
inline bool positiveAffine(const std::array<float,16>& m) {
  for (float v:m) { if (!std::isfinite(v) || std::abs(v)>1e7f) { return false; } }
  const double det=double(m[0])*(double(m[5])*m[10]-double(m[6])*m[9])-
    double(m[1])*(double(m[4])*m[10]-double(m[6])*m[8])+double(m[2])*(double(m[4])*m[9]-double(m[5])*m[8]);
  return det>1e-8 && det<1e12;
}
inline Result reconstruct(Profile p,const sky_shader::Constants& c,const std::vector<uint8_t>& bytes,unsigned stride,
    unsigned position,unsigned uv,int weight,int indices,const std::vector<sky_shader::Vec>& morph={}) {
  Result out;if (p==Profile::Unknown) { return out; }
  const bool skin=p==Profile::Skin3 || p==Profile::Neko3;
  if (stride!=32 || bytes.empty() || bytes.size()%stride || bytes.size()/stride>kMaxVertices || position>stride-12 || uv>stride-8 ||
      (p!=Profile::Rigid?(weight<0 || indices<0 || unsigned(weight)>stride-4 || unsigned(indices)>stride-4):(weight>=0 || indices>=0))) { out.reason="model_layout_or_range";return out; }
  if (c[0][1]!=1 || !std::isfinite(c[13][0]) || std::abs(c[13][0])>1e4f) { out.reason="model_morph_or_homogeneous_control";return out; }
  if (p==Profile::Neko3 && c[12][3]!=0) { out.reason="neko_clip_depth_bias_unsupported";return out; }
  if (c[13][0]!=0 && morph.size()!=bytes.size()/stride) { out.reason="model_morph_position_unavailable";return out; }
  out.world[15]=1;
  for (unsigned j=0;j<3;++j) { for (unsigned i=0;i<4;++i) { out.world[i*4+j]=c[58+j][i]; } }
  if (skin) { out.world={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
  else if (!positiveAffine(out.world)) { out.reason="model_matrix_nonfinite_or_mirrored";return out; }
  for (unsigned r:{48u,49u}) { for (float v:c[r]) { if (!std::isfinite(v)) { out.reason="model_uv_nonfinite";return out; } } }
  out.vertices.resize(bytes.size());
  for (size_t at=0;at<bytes.size();at+=stride) {
    if (p==Profile::Bone0) {
      int16_t w[2]{};uint32_t bone=0;std::memcpy(w,bytes.data()+at+weight,4);std::memcpy(&bone,bytes.data()+at+indices,4);
      if (w[0]!=32767 || w[1]!=-1 || bone) { out.reason="model_not_rigid_bone_zero";out.vertices.clear();return out; }
    }
    float v[3]{},t[2]{};std::memcpy(v,bytes.data()+at+position,12);std::memcpy(t,bytes.data()+at+uv,8);
    for (float x:v) { if (!std::isfinite(x) || std::abs(x)>1e7f) { out.reason="model_position_nonfinite";out.vertices.clear();return out; } }
    if (c[13][0]!=0) {
      for (unsigned j=0;j<3;++j) {
        const float delta=morph[at/stride][j];
        if (!std::isfinite(delta) || std::abs(delta)>1e7f) { out.reason="model_morph_position_nonfinite";out.vertices.clear();return out; }
        v[j]=delta*c[13][0]+v[j];
      }
    }
    if (skin) {
      int16_t packed[2]{};std::memcpy(packed,bytes.data()+at+weight,4);
      const float w[3]={(float(packed[0])+1.f)/32768.f,(float(packed[1])+1.f)/32768.f,
        1.f-((float(packed[0])+1.f)/32768.f+(float(packed[1])+1.f)/32768.f)};
      const auto* bone=bytes.data()+at+indices;
      for (unsigned k=0;k<3;++k) {
        // All three addresses are read by the original VS, even for zero weight.
        if (w[k]<0 || w[k]>1 || unsigned(bone[k])*3+60>=c.size()) { out.reason="model_bone_weight_or_palette_range";out.vertices.clear();return out; }
      }
      float world[3]{};
      for (unsigned j=0;j<3;++j) {
        float row[4]{};
        for (unsigned i=0;i<4;++i) {
          const float a=c[58+unsigned(bone[0])*3+j][i],b=c[58+unsigned(bone[1])*3+j][i],z=c[58+unsigned(bone[2])*3+j][i];
          if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(z)) { out.reason="model_bone_matrix_nonfinite";out.vertices.clear();return out; }
          row[i]=a*w[0]+b*w[1];row[i]=z*w[2]+row[i];
        }
        world[j]=v[0]*row[0]+v[1]*row[1]+v[2]*row[2]+row[3];
      }
      std::memcpy(v,world,sizeof(v));
    }
    for (float x:v) { if (!std::isfinite(x) || std::abs(x)>1e7f) { out.reason="model_position_nonfinite";out.vertices.clear();return out; } }
    float mapped[2]{};
    for (unsigned j=0;j<2;++j) {
      mapped[j]=t[0]*c[48+j][0]+t[1]*c[48+j][1]+c[48+j][3];
      if (!std::isfinite(mapped[j]) || std::abs(mapped[j])>1e8f) { out.reason="model_uv_nonfinite";out.vertices.clear();return out; }
    }
    std::memcpy(out.vertices.data()+at,v,12);std::memcpy(out.vertices.data()+at+12,mapped,8);
  }
  out.valid=true;out.reason=p==Profile::Neko3?"original_vs_neko_three_weight":p==Profile::Skin3?"original_vs_three_weight":
    p==Profile::Bone0?"original_vs_rigid_bone_zero":"original_vs_model_matrix";return out;
}
// Exact v19 Infected VS reads FLOAT3 position/normal and FLOAT2 UV, no
// palette or morph input. Use shader c58..60, never stale D3DTS_WORLD.
inline Result infected(const sky_shader::Constants& c,const std::vector<uint8_t>& bytes) {
  Result out;out.reason="infected_layout_or_control";
  if(bytes.empty()||bytes.size()%48||bytes.size()/48>kMaxVertices||c[0][1]!=1) { return out; }
  out.world[15]=1;
  for(unsigned j=0;j<3;++j) { for(unsigned i=0;i<4;++i) { out.world[i*4+j]=c[58+j][i]; } }
  // Audited captures are rigid. Normal vectors can be imported without an
  // inverse-transpose only for this stricter contract; unknown transforms fall back.
  if(!scene_draw::rigid(out.world)) { out.reason="infected_nonrigid_matrix";return out; }
  out.vertices.resize(bytes.size()/48*32);
  for(size_t n=0;n<bytes.size()/48;++n) {
    float v[12]{};std::memcpy(v,bytes.data()+n*48,48);
    for(unsigned j=0;j<8;++j) { if(!std::isfinite(v[j])||std::abs(v[j])>1e7f) { out.reason="infected_nonfinite_input";out.vertices.clear();return out; } }
    auto* dst=out.vertices.data()+n*32;std::memcpy(dst,v,12);std::memcpy(dst+12,v+6,8);
    // Canonical bytes20..31 hold local smooth normals. Zero remains explicit
    // and the API builder falls back to that triangle's geometric normal.
    const double length=std::sqrt(double(v[3])*v[3]+double(v[4])*v[4]+double(v[5])*v[5]);
    if(length>1e-8) { float normal[3]{};for(unsigned j=0;j<3;++j) { normal[j]=float(v[3+j]/length); }std::memcpy(dst+20,normal,12); }
  }
  out.valid=true;out.reason="original_vs_infected_rigid48";return out;
}
inline bool cameraCompatible(const Result& result,const sky_shader::Constants& c,const std::array<float,16>& view,const std::array<float,16>& projection) {
  if (!result.valid) { return false; }
  // The full-program profiles establish worldW=1 and clip DP4(c8..11).
  // Compare their matrix before large world-coordinate cancellation. A foreign
  // camera still rejects, including a nonzero coefficient at a structural zero.
  for (unsigned j=0;j<4;++j) { for (unsigned i=0;i<4;++i) {
    double expected=0,magnitude=0;
    for (unsigned k=0;k<4;++k) {
      const double term=double(view[i*4+k])*projection[k*4+j];expected+=term;magnitude+=std::abs(term);
    }
    if (!std::isfinite(expected) || !std::isfinite(c[8+j][i]) ||
        std::abs(double(c[8+j][i])-expected)>8.0*1.1920928955078125e-7*magnitude) { return false; }
  }
  }return true;
}
} // namespace ldb_rt::model_shader
