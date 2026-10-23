#include "rt_world_diagnostics.h"
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
using namespace ldb_rt::world_diagnostics;
void require(bool ok, const char* why) { if (!ok) { throw std::runtime_error(why); } }
int main() {
  try {
    require(!allowPersistentGpuSkin(0,0),"GPU skin warm-up needs a complete density sample");
    require(allowPersistentGpuSkin(kGpuSkinModelDensityLimit,0)&&allowPersistentGpuSkin(1,kGpuSkinSubmissionsPerFrame-1),"low-density GPU skin path rejected");
    require(allowPersistentGpuSkin(kGpuSkinModelDensityLimit+1,kGpuSkinHighDensitySubmissionsPerFrame-1)&&
      !allowPersistentGpuSkin(kGpuSkinModelDensityLimit+1,kGpuSkinHighDensitySubmissionsPerFrame)&&
      !allowPersistentGpuSkin(1,kGpuSkinSubmissionsPerFrame),"adaptive GPU skin density or submission bound exceeded");
    require(performanceGpuSkinLimit(0)==16&&performanceGpuSkinLimit(29)==16&&performanceGpuSkinLimit(30)==32&&
      performanceGpuSkinLimit(60)==64&&performanceGpuSkinLimit(90)==128&&performanceGpuSkinLimit(999)==128,
      "GPU performance ramp phase limits differ");
    std::cout << "RT GPU skin previous-frame density and submission limits: PASS\n";
    CoverageComparison coverage;
    require(coverage.phase()==CoveragePhase::Raster && !coverage.afterCapture(120), "coverage must wait for raster control");
    require(coverage.afterCapture(30) && coverage.phase()==CoveragePhase::BaselineWorld, "baseline remains old coverage");
    require(coverage.afterCapture(120) && coverage.phase()==CoveragePhase::ExpandedWorld && !coverage.afterCapture(120), "expanded coverage only after baseline readback");
    require(coverage.afterCapture(180) && coverage.phase()==CoveragePhase::ExpandedModels &&
      coverage.afterCapture(240) && !coverage.afterCapture(240), "sky then model coverage is bounded");
    require(coverage.models()==ModelStage::NekoPreserve && preserve(coverage.models()) && !includeModel(coverage.models(),false) && includeModel(coverage.models(),true),"Neko preserve control not isolated");
    require(coverage.afterCapture(300) && coverage.models()==ModelStage::NekoRecheck && !preserve(coverage.models()),"Neko preserve-off recheck missing");
    require(coverage.afterCapture(360) && coverage.models()==ModelStage::Combined && includeModel(coverage.models(),false) && includeModel(coverage.models(),true),"combined models missing");
    require(coverage.afterCapture(420) && matte(coverage.models()) && includeModel(coverage.models(),true),"neutral material model stage missing");
    require(coverage.afterCapture(480) && directional(coverage.models()),"directional light stage missing");
    require(coverage.afterCapture(540) && coverage.models()==ModelStage::Recovery && !includeModel(coverage.models(),false) && !includeModel(coverage.models(),true),"recovery retained model conversion");
    require(coverage.afterCapture(600) && !coverage.afterCapture(600) && !coverage.afterCapture(660) && readback(600) && !readback(660),"coverage capture must stop at recovery");
    for (unsigned failure=0;failure<3;++failure) {
      CoverageComparison broken;unsigned at=0;
      require(!broken.afterCapture(30,[&](const char*,const char*) { return at++!=failure; }) && broken.phase()==CoveragePhase::Raster,"partial compatibility config mislabeled as applied");
    }
    CoverageComparison paths;std::map<std::string,std::string> pathValues;
    for (uint64_t frame:{30u,120u,180u,240u,300u,360u,420u,480u,540u,600u}) {
      unsigned writes=0;
      require(paths.afterCapture(frame,[&](const char* k,const char* v) { ++writes;pathValues[k]=v;return true; }),"compatibility configuration ordering");
      require(writes==(frame==600?0u:3u),"final readback mutated compatibility settings");
      if (frame<600) {
        require(pathValues["rtx.enablePreservePath"]==(frame==240?"True":"False"),"preserve OFF/ON/OFF or restoration wrong");
        require(pathValues["rtx.fallbackLightType"]==(frame==480?"0":"1") && pathValues["rtx.fallbackLightRadiance"]==(frame==480?"3,3,3":"3000,3000,3000"),"light comparison or restoration wrong");
      }
    }
    std::cout << "RT model preserve OFF/ON/OFF/material/light/recovery/partial-failure: PASS\n";
    LiveCoverageComparison live;
    const char* stages[]={"off","off","off","api_neko_neutral","api_combined_neutral","api_combined_rough","api_combined_metal","api_combined_alpha","api_combined_emissive","recovery"};
    unsigned stageIndex=0;
    for(uint64_t frame:{30u,120u,180u,240u,300u,360u,420u,480u,540u,600u}) {
      require(std::string(liveName(live.models()))==stages[stageIndex++],"live material stage mislabeled");
      if(frame==240) { require(liveInclude(live.models(),true)&&!liveInclude(live.models(),false),"first API stage must isolate Neko"); }
      if(frame>=300&&frame<=540) { require(liveInclude(live.models(),true)&&liveInclude(live.models(),false),"combined API stage omitted a model family"); }
      if(frame==600) { require(!liveInclude(live.models(),true)&&!liveInclude(live.models(),false),"recovery retains API models"); }
      unsigned writes=0;
      require(live.afterCapture(frame,[&](const char*,const char* value) { ++writes;return std::string(value)=="False"; }),"live compatibility setters failed");
      require(writes==(frame==600?0u:4u),"live compatibility setter count");
    }
    for(unsigned failure=0;failure<4;++failure) {
      LiveCoverageComparison broken;unsigned at=0;
      require(!broken.afterCapture(30,[&](const char*,const char*) { return at++!=failure; })&&broken.phase()==CoveragePhase::Raster,"partial live configuration advanced label");
    }
    std::cout<<"RT live mesh stage ordering/material controls/partial-failure: PASS\n";
    Comparison comparison;
    std::map<std::string, std::string> values;
    auto setter = [&](const char* key, const char* value) { values[key] = value; return true; };
    require(comparison.phase() == Phase::Raster && !readback(31) && readback(180), "raster/control and readback gate");
    require(comparison.afterCapture(30, setter) && values["rtx.fallbackLightRadiance"] == "30,30,30", "baseline follows control readback");
    require(comparison.afterCapture(120, setter) && values["rtx.fallbackLightType"] == "1" && values["rtx.fallbackLightRadiance"] == "3000,3000,3000", "sphere comparison changes radiance only");
    require(comparison.afterCapture(180, setter) && values["rtx.fallbackLightType"] == "0" && values["rtx.fallbackLightDirection"] == "0,0,-1", "distant comparison has explicit Source Z-up direction");
    unsigned calls = 0;
    require(comparison.afterCapture(240, [&](const char*, const char*) { ++calls; return false; }) && calls == 0, "final readback must not change lighting");
    for (unsigned failure = 0; failure < 7; ++failure) {
      Comparison broken; unsigned at = 0;
      require(!broken.afterCapture(30, [&](const char*, const char*) { return at++ != failure; }) && broken.failed() && broken.phase() == Phase::Raster, "partial config failure must not label phase as applied");
      require(!broken.afterCapture(120, setter), "failed comparison must not continue");
    }
    Comparison unordered;
    require(!unordered.afterCapture(120, setter) && unordered.failed(), "missing control must not start comparison");
    Comparison duplicate;
    require(duplicate.afterCapture(30, setter) && !duplicate.afterCapture(30, setter), "duplicate boundary must not reapply a phase");
    Comparison partialSecond;
    require(partialSecond.afterCapture(30, setter), "second-phase failure setup");
    require(!partialSecond.afterCapture(120, [](const char*, const char*) { return false; }) &&
      partialSecond.phase() == Phase::BaselineSphere && partialSecond.failed(), "failed second phase must retain last accepted label");
    std::cout << "RT world lighting comparison/order/partial-failure: PASS; effective GPU options unrun\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
