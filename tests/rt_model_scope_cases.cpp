#include "rt_model_scope.h"

#include <future>
#include <iostream>
#include <stdexcept>

using namespace ldb_rt;
void check(bool value,const char* message) { if(!value) { throw std::runtime_error(message); } }
std::vector<model_scope::Matrix3x4> palette(unsigned count,float translation=0) {
  std::vector<model_scope::Matrix3x4> value(count);for(auto& bone:value) { bone[0]=bone[5]=bone[10]=1;bone[3]=translation; }return value;
}
model_scope::Input input(const std::vector<model_scope::Matrix3x4>& bones,const char* path="models/infected/common_male.mdl",uintptr_t renderable=0x1234) {
  model_scope::Input value;value.path=path;value.checksum=0x55667788;value.renderable=renderable;value.entity=17;value.skin=2;
  value.body=3;value.lod=1;value.handle=9;value.bones=bones.data();value.boneCount=uint32_t(bones.size());return value;
}
void identityAndOwnedPalette() {
  auto bones=palette(100,7);model_scope::Identity first,second;check(model_scope::identify(input(bones),first),"identity rejected");
  check(model_scope::identify(input(bones),second)&&first.resource==second.resource&&first.instance==second.instance,"identity unstable");
  auto changed=input(bones);changed.skin=4;model_scope::Identity variant;check(model_scope::identify(changed,variant)&&variant.resource!=first.resource,"resource variant collided");
  model_scope::Scope scope(input(bones));check(scope.valid(),"scope rejected");bones[0][3]=99;const auto surface=model_scope::Scope::takeSurface();
  check(surface.identity&&surface.first&&surface.surface==0&&surface.boneCount==100&&surface.bones[0][3]==7,"palette was not owned");
}
void nestedScopesRestore() {
  const auto outerBones=palette(2,1),innerBones=palette(3,2);model_scope::Scope outer(input(outerBones,"models/a.mdl",1));
  const auto a=model_scope::Scope::takeSurface();check(a.identity&&a.bones[0][3]==1&&a.surface==0,"outer unavailable");
  {
    model_scope::Scope inner(input(innerBones,"models/b.mdl",2));const auto b=model_scope::Scope::takeSurface();
    check(b.identity&&b.bones[0][3]==2&&b.surface==0&&b.scope!=a.scope,"inner unavailable");
  }
  const auto restored=model_scope::Scope::takeSurface();check(restored.identity&&restored.bones[0][3]==1&&restored.surface==1,"outer not restored");
  check(outer.surfaces()==2,"outer surface count changed");
}
void invalidInputsDoNotPublish() {
  auto bones=palette(1);auto value=input(bones);value.boneCount=model_resource::kMaxBones+1;model_scope::Scope tooMany(value);
  check(!tooMany.valid()&&!model_scope::Scope::currentIdentity(),"invalid scope published");
  value=input(bones);value.path="";model_scope::Scope empty(value);check(!empty.valid(),"empty path accepted");
}
void threadIsolation() {
  auto run=[](uintptr_t renderable,float translation) {
    const auto bones=palette(4,translation);model_scope::Scope scope(input(bones,"models/thread.mdl",renderable));
    const auto first=model_scope::Scope::takeSurface(),second=model_scope::Scope::takeSurface();
    check(first.identity&&second.surface==1&&first.bones[0][3]==translation,"thread scope invalid");return std::pair<uint64_t,uint64_t>{first.identity->instance,first.scope};
  };
  auto a=std::async(std::launch::async,run,11,3);auto b=std::async(std::launch::async,run,12,4);const auto av=a.get(),bv=b.get();
  check(av.first!=bv.first&&av.second!=bv.second&&!model_scope::Scope::currentIdentity(),"thread scopes collided");
}
int main() {
  try { identityAndOwnedPalette();nestedScopesRestore();invalidInputsDoNotPublish();threadIsolation();
    std::cout<<"RT model Source scope cases: PASS (4 groups)\n";return 0;
  } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
