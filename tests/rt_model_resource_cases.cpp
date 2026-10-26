#include "rt_model_frame.h"

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace ldb_rt;
void require(bool value,const char* message) { if(!value) { throw std::runtime_error(message); } }

model_resource::Definition model(uint64_t generation=1,unsigned surfaces=3) {
  model_resource::Definition value;value.id=41;value.generation=generation;value.topology=0x123456789abcdef0ull;
  value.lod=1;value.body=2;value.skin=3;value.maxBones=128;
  for(unsigned n=0;n<surfaces;++n) {
    model_resource::Surface surface;surface.vertexCount=1000+n;surface.indexCount=3000;surface.materialSlot=n;surface.bonesPerVertex=3;
    surface.vertices={100+n,generation};surface.indices={200+n,generation};surface.skinning={300+n,generation};surface.material={400+n,generation};
    value.surfaces.push_back(surface);
  }
  return value;
}
model_frame::Instance instance(uint64_t id=9,unsigned bones=100,unsigned surfaces=3) {
  model_frame::Instance value;value.id=id;value.generation=1;value.model=41;value.modelGeneration=1;value.view=1;
  value.world[0]=value.world[5]=value.world[10]=1;value.bones.resize(bones);
  for(unsigned n=0;n<bones;++n) { value.bones[n][0]=value.bones[n][5]=value.bones[n][10]=1;value.bones[n][3]=float(n); }
  value.visibleSurfaces.resize((surfaces+31)/32);value.visibleSurfaces[0]=(uint32_t(1)<<surfaces)-1;return value;
}

void wireAndGeneration() {
  const auto original=model();const auto bytes=model_resource::encode(original);model_resource::Definition decoded;
  require(model_resource::decodeDefinition(bytes.data(),bytes.size(),decoded)==model_resource::Code::Ok,"definition wire rejected");
  require(decoded.id==original.id&&decoded.surfaces.size()==original.surfaces.size()&&decoded.surfaces[2].indices==original.surfaces[2].indices,"definition wire changed");
  for(size_t n=0;n<bytes.size();++n) { require(model_resource::decodeDefinition(bytes.data(),n,decoded)!=model_resource::Code::Ok,"truncated definition accepted"); }
  model_resource::Registry registry;require(registry.define(decoded)==model_resource::Code::Ok,"definition rejected");
  require(registry.define(decoded)==model_resource::Code::Duplicate,"duplicate definition accepted");
  require(registry.find(41,1)==nullptr,"pending resource exposed");require(registry.markReady(41,1)==model_resource::Code::Ok,"ready rejected");
  require(registry.find(41,1)!=nullptr,"ready resource hidden");require(registry.invalidate(41,1)==model_resource::Code::Ok,"invalidate rejected");
  const model_resource::Invalidation invalidation{41,1};const auto invalidationBytes=model_resource::encode(invalidation);model_resource::Invalidation decodedInvalidation;
  require(model_resource::decodeInvalidation(invalidationBytes.data(),invalidationBytes.size(),decodedInvalidation)==model_resource::Code::Ok&&
    decodedInvalidation.id==41&&decodedInvalidation.generation==1,"invalidation wire changed");
  require(registry.define(model(1))==model_resource::Code::Stale,"generation reuse accepted");
  require(registry.define(model(2))==model_resource::Code::Ok,"next generation rejected");
}

void instanceWireAndValidation() {
  auto value=instance();const auto bytes=model_frame::encode(value);model_frame::Instance decoded;
  require(model_frame::decodeInstance(bytes.data(),bytes.size(),decoded)==model_resource::Code::Ok,"instance wire rejected");
  require(decoded.bones.size()==100&&decoded.visibleSurfaces==value.visibleSurfaces&&decoded.bones[99][3]==99,"instance wire changed");
  const auto definition=model();require(model_frame::validate(decoded,definition)==model_resource::Code::Ok,"valid instance rejected");
  decoded.bones[0][0]=std::numeric_limits<float>::quiet_NaN();require(model_frame::validate(decoded,definition)==model_resource::Code::Invalid,"NaN bone accepted");
  decoded=value;decoded.visibleSurfaces[0]|=uint32_t(1)<<7;require(model_frame::validate(decoded,definition)==model_resource::Code::Invalid,"out-of-range surface accepted");
}

void tripleOwnership() {
  model_resource::Registry registry;require(registry.define(model())==model_resource::Code::Ok,"define");require(registry.markReady(41,1)==model_resource::Code::Ok,"ready");
  model_frame::TripleArena arena;
  for(uint64_t frame=1;frame<=3;++frame) {
    require(arena.begin(frame,1)==model_resource::Code::Ok,"begin first arenas");auto value=instance(frame);
    const auto bytes=model_frame::payloadBytes(value);require(arena.add(std::move(value),registry)==model_resource::Code::Ok,"add first arenas");
    require(arena.commit(1,bytes)==model_resource::Code::Ok,"commit first arenas");require(arena.acquire(frame)!=nullptr,"acquire first arenas");
  }
  require(arena.begin(4,1)==model_resource::Code::FrameBusy,"in-flight arena overwritten");
  require(arena.complete(1)==model_resource::Code::Ok,"complete first arena");require(arena.begin(4,1)==model_resource::Code::Ok,"released arena unavailable");
  auto value=instance(4);const auto bytes=model_frame::payloadBytes(value);require(arena.add(std::move(value),registry)==model_resource::Code::Ok,"add reused arena");
  require(arena.commit(1,bytes)==model_resource::Code::Ok,"commit reused arena");
}

void replayAndNoFallback() {
  model_resource::Registry registry;require(registry.define(model())==model_resource::Code::Ok,"replay define");require(registry.markReady(41,1)==model_resource::Code::Ok,"replay ready");
  model_frame::TripleArena arena;uint64_t previous=0;
  for(uint64_t frame=1;frame<=300;++frame) {
    if(previous) { require(arena.complete(previous)==model_resource::Code::Ok,"replay completion"); }
    require(arena.begin(frame,1)==model_resource::Code::Ok,"replay begin");auto value=instance(1000+frame);
    const auto bytes=model_frame::payloadBytes(value);
    require(arena.add(std::move(value),registry)==model_resource::Code::Ok,"replay add");require(arena.commit(1,bytes)==model_resource::Code::Ok,"replay commit");
    require(arena.acquire(frame)!=nullptr,"replay acquire");previous=frame;
  }
  require(arena.complete(previous)==model_resource::Code::Ok,"replay final completion");
  const auto& resources=registry.statistics();const auto& frames=arena.statistics();
  require(resources.definitions==1&&resources.ready==1,"topology uploaded more than once");
  require(frames.frames==300&&frames.instances==300&&frames.omitted==0,"instance replay incomplete");
  require(frames.cpuPosedVertices==0&&frames.cpuRewrittenIndices==0&&frames.compatibilityFallbacks==0,"legacy work entered resource path");

  model_frame::Commit commit{300,1,frames.instances,frames.paletteBytes,0x9876};const auto commitBytes=model_frame::encode(commit);model_frame::Commit decodedCommit;
  require(model_frame::decodeCommit(commitBytes.data(),commitBytes.size(),decodedCommit)==model_resource::Code::Ok&&
    decodedCommit.frame==300&&decodedCommit.instances==300&&decodedCommit.payloadBytes==frames.paletteBytes,"commit wire changed");

  model_frame::TripleArena scale;uint64_t done=0;
  for(const unsigned count:{1u,8u,32u,64u}) {
    if(done) { require(scale.complete(done)==model_resource::Code::Ok,"scale completion"); }
    const auto frame=done+1;require(scale.begin(frame,1)==model_resource::Code::Ok,"scale begin");size_t bytes=0;
    for(unsigned n=0;n<count;++n) { auto value=instance(10000+frame*100+n);bytes+=model_frame::payloadBytes(value);require(scale.add(std::move(value),registry)==model_resource::Code::Ok,"scale add"); }
    require(scale.commit(count,bytes)==model_resource::Code::Ok,"scale commit");const auto* snapshot=scale.acquire(frame);
    require(snapshot&&snapshot->instances.size()==count&&snapshot->paletteBytes==size_t(count)*100*12*4,"palette scaling mismatch");done=frame;
  }
  require(scale.complete(done)==model_resource::Code::Ok,"scale final completion");
}

void noImplicitFallback() {
  model_resource::Registry registry;require(registry.define(model())==model_resource::Code::Ok,"pending define");
  model_frame::TripleArena arena;require(arena.begin(1,1)==model_resource::Code::Ok,"pending begin");
  require(arena.add(instance(),registry)==model_resource::Code::NotReady,"pending resource accepted");
  require(arena.statistics().omitted==1&&arena.statistics().compatibilityFallbacks==0,"pending resource fell back");
  require(arena.commit(0,0)==model_resource::Code::Ok,"empty diagnostic commit rejected");
}

int main() {
  try {
    wireAndGeneration();instanceWireAndValidation();tripleOwnership();replayAndNoFallback();noImplicitFallback();
    std::cout<<"RT model resource transaction cases: PASS (5 groups)\n";return 0;
  } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
