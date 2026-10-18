#pragma once
#include "rt_sky_shader.h"
#include <string>

namespace ldb_rt::model_shader {
constexpr unsigned kMaxVertices=16384;
// Complete SM3 programs audited in the 191200 and 204924 captures. A material name or
// stride does not establish their position/UV contract. Unknown code falls back.
enum class Profile { Unknown, Rigid, Bone0 };
inline Profile profile(const std::string& sha) {
  if (sha=="3a9ebc14b4895b0614690472b2500c46c505ff282e6868472849900975493e41" ||
      sha=="3566b4c8bed75da732f7c8cdb1bce1bb1b5025c932916b2d739ec748f9b8526b") { return Profile::Rigid; }
  for (const char* known:{"84e1a3321dd4b0e7630372d7c229e37cadea38db2833da73cbfdd5bc8b7dce0d",
      "8eb1c8546fb7d3536f008f1e09998cf034849f5f67f23fc1d2fa61cea69774f9",
      "149c60569507ca934d0374759c8f998fa1ce0794937375a7a21b55549f8c7089"}) {
    if (sha==known) { return Profile::Bone0; }
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
    unsigned position,unsigned uv,int weight,int indices) {
  Result out;if (p==Profile::Unknown) { return out; }
  if (stride!=32 || bytes.empty() || bytes.size()%stride || bytes.size()/stride>kMaxVertices || position>stride-12 || uv>stride-8 ||
      (p==Profile::Bone0?(weight<0 || indices<0 || unsigned(weight)>stride-4 || unsigned(indices)>stride-4):(weight>=0 || indices>=0))) { out.reason="model_layout_or_range";return out; }
  if (c[0][1]!=1 || c[13][0]!=0) { out.reason="model_morph_or_homogeneous_control";return out; }
  out.world[15]=1;
  for (unsigned j=0;j<3;++j) { for (unsigned i=0;i<4;++i) { out.world[i*4+j]=c[58+j][i]; } }
  if (!positiveAffine(out.world)) { out.reason="model_matrix_nonfinite_or_mirrored";return out; }
  for (unsigned r:{48u,49u}) { for (float v:c[r]) { if (!std::isfinite(v)) { out.reason="model_uv_nonfinite";return out; } } }
  out.vertices.resize(bytes.size());
  for (size_t at=0;at<bytes.size();at+=stride) {
    if (p==Profile::Bone0) {
      int16_t w[2]{};uint32_t bone=0;std::memcpy(w,bytes.data()+at+weight,4);std::memcpy(&bone,bytes.data()+at+indices,4);
      if (w[0]!=32767 || w[1]!=-1 || bone) { out.reason="model_not_rigid_bone_zero";out.vertices.clear();return out; }
    }
    float v[3]{},t[2]{};std::memcpy(v,bytes.data()+at+position,12);std::memcpy(t,bytes.data()+at+uv,8);
    for (float x:v) { if (!std::isfinite(x) || std::abs(x)>1e7f) { out.reason="model_position_nonfinite";out.vertices.clear();return out; } }
    float mapped[2]{};
    for (unsigned j=0;j<2;++j) {
      mapped[j]=t[0]*c[48+j][0]+t[1]*c[48+j][1]+c[48+j][3];
      if (!std::isfinite(mapped[j]) || std::abs(mapped[j])>1e8f) { out.reason="model_uv_nonfinite";out.vertices.clear();return out; }
    }
    std::memcpy(out.vertices.data()+at,v,12);std::memcpy(out.vertices.data()+at+12,mapped,8);
  }
  out.valid=true;out.reason=p==Profile::Bone0?"original_vs_rigid_bone_zero":"original_vs_model_matrix";return out;
}
inline bool cameraCompatible(const Result& result,const sky_shader::Constants& c,const std::array<float,16>& view,const std::array<float,16>& projection) {
  if (!result.valid) { return false; }
  for (size_t at=0;at<result.vertices.size();at+=32) {
    sky_shader::Vec local{0,0,0,1},world{0,0,0,1},clip{};std::memcpy(local.data(),result.vertices.data()+at,12);
    for (unsigned j=0;j<3;++j) { double v=0;for (unsigned i=0;i<4;++i) { v+=double(local[i])*result.world[i*4+j]; }world[j]=float(v); }
    for (unsigned j=0;j<4;++j) { double v=0;for (unsigned i=0;i<4;++i) { v+=double(world[i])*c[8+j][i]; }clip[j]=float(v); }
    if (!sky_shader::sameProjection(world,clip,view,projection)) { return false; }
  }return true;
}
} // namespace ldb_rt::model_shader
