#pragma once
#include "rt_infected_material.h"
inline void infectedMaterialCases() {
  using namespace infected_material;
  Parameters p{};p.range={.1f,.9f,0,0};p.tint={.8f,.6f,.4f,0};
  const auto color=diffuse({.1f,.2f,.8f,.5f},{.25f,.6f,0,0},{.8f,.4f,.2f,1},{.2f,.3f,.4f,1},{.1f,.5f,.9f,1},p);
  const float expected[]={.575015625f,.33415625f,.214328125f};
  for(unsigned i=0;i<3;++i) { require(std::abs(color[i]-expected[i])<1e-6f,"infected palette/detail/tint dependency differs"); }
  require(color[3]==1,"infected encoded alpha used as surface opacity");
  auto invalid=p;invalid.range[1]=invalid.range[0];require(!valid(invalid),"infected singular range admitted");
  invalid=p;invalid.atlas[0]=NAN;require(!valid(invalid),"infected nonfinite atlas admitted");
  Image bc;bc.width=bc.height=4;bc.format=71;bc.raw.resize(156);bc.raw[148]=0;bc.raw[149]=248; // red 565, selector0
  auto c=bc.pixel(0,0);require(c[0]==1&&c[1]==0&&c[2]==0&&c[3]==1,"BC1 color endpoint decode");
  bc.format=77;bc.raw.resize(164);bc.raw[148]=64;bc.raw[149]=200;bc.raw[156]=224;bc.raw[157]=7; // green 565
  c=bc.pixel(3,3);require(c[0]==0&&c[1]==1&&c[2]==0&&std::abs(c[3]-64.f/255)<1e-6,"BC3 alpha/color decode");
  bc.format=74;bc.raw[148]=0x21;c=bc.pixel(0,0);require(std::abs(c[3]-1.f/15)<1e-6,"BC2 alpha nibble decode");
  require(Image::address(-1,4,1)==3&&Image::address(-1,4,2)==0&&Image::address(4,4,2)==3&&Image::address(9,4,0)==3,"infected sampler addressing");
  Image solid;solid.width=solid.height=1;solid.format=87;solid.raw.resize(152);solid.raw[148]=255;solid.raw[149]=128;solid.raw[150]=64;solid.raw[151]=128;
  solid.srgb=true;c=solid.pixel(0,0);require(std::abs(c[0]-.05126946f)<1e-6&&std::abs(c[1]-.2158605f)<1e-6&&c[2]==1,"infected sRGB RGB-only decode");
  solid.srgb=false;p.detailU={1,0,0,0};p.detailV={0,1,0,0};
  const auto encoded=bake(solid,solid,solid,p,8,4);uint32_t header[37]{};std::memcpy(header,encoded.data(),148);
  require(encoded.size()==148+(32+8+2+1)*4&&header[7]==4&&header[32]==87&&header[27]==0x401008,"infected full generated linear mip chain");
  for(size_t at=151;at<encoded.size();at+=4) { require(encoded[at]==255,"infected baked mip opacity"); }
  require(bake(solid,solid,solid,p,257,4).empty(),"infected bake resolution budget");
  std::cout<<"RT Infected palette/detail/tint/BC1-3/sRGB/wrap/opaque/generated mips: PASS\n";
}
