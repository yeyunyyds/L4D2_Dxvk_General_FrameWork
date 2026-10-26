#include "rt_model_assembly.h"

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace ldb_rt;
void check(bool value,const char* message) { if(!value) { throw std::runtime_error(message); } }
draw_observe::Snapshot snapshot(uint64_t scope,uint64_t resource,uint64_t instance,uint32_t surface,bool first) {
  draw_observe::Snapshot value;value.modelScope=scope;value.modelResource=resource;value.modelInstance=instance;value.modelSurface=surface;
  value.modelBoneCount=3;value.modelEntity=17;value.modelFlags=draw_observe::kModelPresent|(first?draw_observe::kModelFirstSurface:0);
  if(first) { value.modelBones.resize(3);for(auto& bone:value.modelBones) { bone={1,0,0,4,0,1,0,5,0,0,1,6}; } }return value;
}
int main() {
  try {
    model_assembly::Collector<unsigned> collector;auto first=snapshot(1,10,100,0,true);
    check(collector.observe(first)==model_assembly::Code::Ok,"first scope rejected");first.modelBones[0][3]=99;
    auto body=snapshot(1,10,100,2,false),clothes=snapshot(1,10,100,5,false);
    check(collector.observe(body)==model_assembly::Code::Ok&&collector.append(body,20)==model_assembly::Code::Ok,"body rejected");
    check(collector.observe(clothes)==model_assembly::Code::Ok&&collector.append(clothes,50)==model_assembly::Code::Ok,"clothes rejected");
    auto next=snapshot(2,10,101,0,true);check(collector.observe(next)==model_assembly::Code::Ok,"next scope rejected");
    auto completed=collector.take();check(completed&&completed->scope==1&&completed->resource==10&&completed->instance==100&&completed->palette[0][3]==4&&
      completed->surfaces.size()==2&&completed->surfaces[0].first==2&&completed->surfaces[1].second==50,"completed scope changed or borrowed palette");
    auto part=snapshot(2,10,101,3,false);check(collector.observe(part)==model_assembly::Code::Ok&&collector.append(part,30)==model_assembly::Code::Ok,"second scope surface rejected");
    check(collector.append(part,31)==model_assembly::Code::Order,"duplicate surface accepted");
    check(collector.finishFrame()==model_assembly::Code::Ok,"last scope not finalized");completed=collector.take();check(completed&&completed->scope==2&&completed->surfaces.size()==1,"last scope lost");
    model_assembly::Collector<unsigned> invalid;auto orphan=snapshot(3,30,300,4,false);check(invalid.observe(orphan)==model_assembly::Code::Orphan,"orphan continuation accepted");
    auto bad=snapshot(3,30,300,0,true);bad.modelBones[0][0]=std::numeric_limits<float>::quiet_NaN();check(invalid.observe(bad)==model_assembly::Code::Palette,"NaN palette accepted");
    auto empty=snapshot(4,40,400,0,true);check(invalid.observe(empty)==model_assembly::Code::Ok&&invalid.finishFrame()==model_assembly::Code::Empty&&!invalid.take(),"empty scope published");
    std::cout<<"RT model scope assembly cases: PASS (owned palette, sparse surfaces, boundaries, rejection)\n";return 0;
  } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
