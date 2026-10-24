#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <ostream>
// Host elapsed scopes include blocking; nested scopes must not be summed.
// These are not GPU duration queries or process CPU utilization counters.
namespace ldb_rt::runtime_profile {
enum class Stage : unsigned { Draw,Metadata,Static,Model,Prepare,SourceGeometry,Skin,Texture,TextureRead,TexturePersist,InfectedBake,Hash,GeometryBuild,Material,MeshCreate,Instance,Retire,OriginalDraw,Readback,Scene,Trace,Present,Count };
inline constexpr const char* names[]={"draw","metadata","static","model","prepare","source_geometry","skin","texture","texture_read","texture_persist","infected_bake","hash","geometry_build","material","mesh_create","instance","retire","original_draw","readback","scene","trace","present"};
struct Counter { uint64_t calls=0,ticks=0,maximum=0; };
struct Window { bool enabled=false,threadTimes=false;uint64_t start=0,frequency=0,first=0,threadKernel=0,threadUser=0;std::array<Counter,unsigned(Stage::Count)> values{}; };
inline Window& window() { static Window w;return w; }
inline uint64_t clock() noexcept { LARGE_INTEGER t{};QueryPerformanceCounter(&t);return uint64_t(t.QuadPart); }
inline uint64_t fileTime(const FILETIME& t) noexcept { return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime; }
inline bool threadTimes(uint64_t& kernel,uint64_t& user) noexcept {
  FILETIME created{},exited{},k{},u{};if(!GetThreadTimes(GetCurrentThread(),&created,&exited,&k,&u)) { return false; }
  kernel=fileTime(k);user=fileTime(u);return true;
}
inline void reset(bool enabled=true) noexcept { auto& w=window();w={};LARGE_INTEGER f{};QueryPerformanceFrequency(&f);w.frequency=uint64_t(f.QuadPart);w.enabled=enabled&&w.frequency;w.start=clock();w.threadTimes=threadTimes(w.threadKernel,w.threadUser); }
class Scope {
  Stage stage;uint64_t begin=0;
public:
  explicit Scope(Stage s) noexcept:stage(s),begin(window().enabled?clock():0) {}
  ~Scope() { if(begin) { const auto delta=clock()-begin;auto& v=window().values[unsigned(stage)];++v.calls;v.ticks+=delta;if(delta>v.maximum) { v.maximum=delta; } } }
  Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};
inline void emit(std::ostream& out,uint64_t frame,const char* segment) {
  auto& w=window();if(!w.enabled||!w.frequency) { return; }const auto now=clock();const auto ms=1000.0/double(w.frequency);FILETIME utc{};GetSystemTimeAsFileTime(&utc);const uint64_t filetime=fileTime(utc);
  uint64_t kernel=0,user=0;const bool haveThreadTimes=w.threadTimes&&threadTimes(kernel,user);const double kernelMs=haveThreadTimes?double(kernel-w.threadKernel)/10000.0:0.0,userMs=haveThreadTimes?double(user-w.threadUser)/10000.0:0.0;
  out<<"{\"event\":\"runtime_timing_window\",\"frame\":"<<frame<<",\"first_after_frame\":"<<w.first<<",\"segment\":\""<<segment
    <<"\",\"wall_ms\":"<<double(now-w.start)*ms<<",\"thread_times_available\":"<<(haveThreadTimes?"true":"false")<<",\"thread_kernel_ms\":"<<kernelMs<<",\"thread_user_ms\":"<<userMs<<",\"thread_cpu_ms\":"<<kernelMs+userMs<<",\"qpc_begin\":"<<w.start<<",\"qpc_end\":"<<now<<",\"qpc_frequency\":"<<w.frequency<<",\"utc_filetime_100ns\":"<<filetime<<",\"nested_scopes\":true,\"scopes\":{";
  for(unsigned i=0;i<unsigned(Stage::Count);++i) { if(i) { out<<','; }const auto& v=w.values[i];out<<'"'<<names[i]<<"\":{\"calls\":"<<v.calls<<",\"elapsed_ms\":"<<double(v.ticks)*ms<<",\"max_ms\":"<<double(v.maximum)*ms<<'}'; }
  out<<"}}\n";w.values={};w.first=frame;w.start=clock();w.threadTimes=threadTimes(w.threadKernel,w.threadUser);
}
}
