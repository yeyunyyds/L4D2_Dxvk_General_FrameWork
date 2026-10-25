#pragma once

#include "rt_live_mesh.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace ldb_rt::model_batch {

struct Job {
  uint64_t frame=0;
  draw_observe::Snapshot snapshot;
  model_input::Captured captured;
  uint32_t kind=0;
  D3DPRIMITIVETYPE topology=D3DPT_FORCE_DWORD;
  std::array<uint32_t,6> args{};
  DWORD cull=D3DCULL_NONE;
  live_mesh::Material variant=live_mesh::Material::Neutral;
  model_texture::Source material;
};

struct Result {
  uint64_t frame=0,ordinal=0;
  std::string shader,reason;
  live_mesh::Geometry geometry;
  std::array<float,16> world{};
  DWORD cull=D3DCULL_NONE;
  live_mesh::Material variant=live_mesh::Material::Neutral;
  model_texture::Source material;
  bool success=false;
};

struct Statistics {
  uint64_t submitted=0,completed=0,failed=0,workerTicks=0;
  unsigned workers=0,maxActive=0;
};

class Pool {
  std::vector<std::thread> threads;
  std::deque<std::function<void()>> queue;
  std::mutex mutex;
  std::condition_variable ready;
  bool stopping=false;
  std::atomic<uint64_t> submitted{0},completed{0},failed{0},workerTicks{0};
  std::atomic<unsigned> active{0},maxActive{0};

  void run() {
    for(;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock,[&] { return stopping||!queue.empty(); });
        if(stopping&&queue.empty()) { return; }
        task=std::move(queue.front());queue.pop_front();
      }
      const auto now=active.fetch_add(1,std::memory_order_relaxed)+1;
      auto peak=maxActive.load(std::memory_order_relaxed);
      while(now>peak&&!maxActive.compare_exchange_weak(peak,now,std::memory_order_relaxed)) { }
      const auto begin=runtime_profile::clock();task();
      workerTicks.fetch_add(runtime_profile::clock()-begin,std::memory_order_relaxed);
      active.fetch_sub(1,std::memory_order_relaxed);
    }
  }
public:
  explicit Pool(unsigned count=4) {
    count=(std::max)(2u,(std::min)(count,8u));threads.reserve(count);
    for(unsigned i=0;i<count;++i) { threads.emplace_back([this] { run(); }); }
  }
  Pool(const Pool&)=delete;Pool& operator=(const Pool&)=delete;
  ~Pool() { shutdown(); }
  void shutdown() {
    { std::lock_guard<std::mutex> lock(mutex);if(stopping) { return; }stopping=true; }
    ready.notify_all();for(auto& thread:threads) { if(thread.joinable()) { thread.join(); } }threads.clear();
  }
  std::future<Result> submit(Job job) {
    submitted.fetch_add(1,std::memory_order_relaxed);
    auto task=std::make_shared<std::packaged_task<Result()>>([this,job=std::move(job)]() mutable {
      Result result;result.frame=job.frame;result.ordinal=job.snapshot.ordinal;result.shader=job.snapshot.materialInfo.shader.data();
      result.cull=job.cull;result.variant=job.variant;result.material=std::move(job.material);
      auto prepared=model_input::finish(job.snapshot,std::move(job.captured),job.kind,job.args,false,false,false);
      result.reason=prepared.reason;
      if(prepared.ready&&live_mesh::build(prepared,job.kind,job.topology,job.cull,result.geometry)) {
        result.world=prepared.world;result.success=true;completed.fetch_add(1,std::memory_order_relaxed);
      } else { failed.fetch_add(1,std::memory_order_relaxed); }
      return result;
    });
    auto future=task->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex);
      if(stopping) { throw std::runtime_error("model worker pool stopped"); }
      queue.emplace_back([task] { (*task)(); });
    }
    ready.notify_one();return future;
  }
  Statistics statistics() const {
    return {submitted.load(std::memory_order_relaxed),completed.load(std::memory_order_relaxed),failed.load(std::memory_order_relaxed),
      workerTicks.load(std::memory_order_relaxed),unsigned(threads.size()),maxActive.load(std::memory_order_relaxed)};
  }
};

struct Key {
  std::string material,path;
  std::array<uint32_t,16> world{};
  DWORD cull=0;
  unsigned variant=0;
  uint32_t width=0,height=0,format=0,levels=0;
  uint8_t alphaBlend=0,alphaTest=0,alphaRef=0,wrapU=0,wrapV=0,filter=0;
  bool operator<(const Key& other) const {
    return std::tie(material,path,world,cull,variant,width,height,format,levels,alphaBlend,alphaTest,alphaRef,wrapU,wrapV,filter)<
      std::tie(other.material,other.path,other.world,other.cull,other.variant,other.width,other.height,other.format,other.levels,
        other.alphaBlend,other.alphaTest,other.alphaRef,other.wrapU,other.wrapV,other.filter);
  }
};

struct Batch {
  live_mesh::Geometry geometry;
  std::array<float,16> world{};
  DWORD cull=D3DCULL_NONE;
  live_mesh::Material variant=live_mesh::Material::Neutral;
  model_texture::Source material;
  uint64_t sourceDraws=0;
};

struct Assembly {
  std::vector<Batch> batches;
  uint64_t sourceDraws=0,sourceVertices=0,sourceIndices=0;
};

inline Key key(const Result& result) {
  Key out;out.material=result.material.key;out.path=result.material.path.u8string();out.cull=result.cull;out.variant=unsigned(result.variant);
  out.width=result.material.width;out.height=result.material.height;out.format=result.material.format;out.levels=result.material.levels;
  out.alphaBlend=result.material.alphaBlend;out.alphaTest=result.material.alphaTest;out.alphaRef=result.material.alphaRef;
  out.wrapU=result.material.wrapU;out.wrapV=result.material.wrapV;out.filter=result.material.filter;
  static_assert(sizeof(out.world)==sizeof(result.world),"batch transform identity size");
  std::memcpy(out.world.data(),result.world.data(),sizeof(out.world));return out;
}

inline Assembly assemble(std::vector<Result>&& results) {
  constexpr size_t kMaxVertices=262144,kMaxIndices=786432;
  std::map<Key,std::vector<Batch>> groups;Assembly out;
  for(auto& result:results) {
    if(!result.success) { continue; }
    ++out.sourceDraws;out.sourceVertices+=result.geometry.vertices.size();out.sourceIndices+=result.geometry.indices.size();
    auto& chunks=groups[key(result)];
    if(chunks.empty()||result.geometry.vertices.size()>kMaxVertices-chunks.back().geometry.vertices.size()||
       result.geometry.indices.size()>kMaxIndices-chunks.back().geometry.indices.size()) {
      Batch batch;batch.world=result.world;batch.cull=result.cull;batch.variant=result.variant;batch.material=result.material;chunks.push_back(std::move(batch));
    }
    auto& batch=chunks.back();const auto base=uint32_t(batch.geometry.vertices.size());
    batch.geometry.vertices.insert(batch.geometry.vertices.end(),result.geometry.vertices.begin(),result.geometry.vertices.end());
    batch.geometry.indices.reserve(batch.geometry.indices.size()+result.geometry.indices.size());
    for(const auto index:result.geometry.indices) { batch.geometry.indices.push_back(base+index); }
    ++batch.sourceDraws;
  }
  for(auto& group:groups) { for(auto& batch:group.second) { out.batches.push_back(std::move(batch)); } }
  return out;
}

} // namespace ldb_rt::model_batch
