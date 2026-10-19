// Frozen captured triangles, no L4D2/Bridge injection. Runtime/device/crash helpers
// are the same ones already exercised by the standalone prerequisite diagnostic.
#define wmain rt_smoke_unused_entry
#include "../../integration/rt/rt_runtime_smoke.cpp"
#undef wmain
#include "geometry.h"
#include <memory>
#include <vector>
#ifndef RT_REPLAY_BUILD
#define RT_REPLAY_BUILD "development"
#endif
namespace replay_gpu {
constexpr const char* identity="ldb-rt-replay+" RT_REPLAY_BUILD;
void initializeRuntime(Runtime& r,Log& log,const fs::path& dll) {
  r.module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);log.result("load_runtime",r.module?0:GetLastError());
  const auto pointer=GetProcAddress(r.module,"remixapi_InitializeLibrary");PFN_remixapi_InitializeLibrary initialize=nullptr;std::memcpy(&initialize,&pointer,sizeof(initialize));
  log.result("find_api",initialize?0:ERROR_PROC_NOT_FOUND);remixapi_InitializeLibraryInfo init{};init.sType=REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;init.version=REMIXAPI_VERSION_MAKE(0,6,4);
  log.result("initialize_api",uint32_t(initialize(&init,&r.api)));
  if(!r.api.Shutdown||!r.api.SetConfigVariable||!r.api.dxvk_CreateD3D9||!r.api.dxvk_RegisterD3D9Device||!r.api.CreateMesh||!r.api.DestroyMesh||!r.api.CreateMaterial||!r.api.DestroyMaterial||!r.api.DrawInstance||!r.api.SetupCamera||!r.api.dxvk_CopyRenderingOutput) { throw std::runtime_error("incomplete API"); }
}
int probe(const fs::path& dll,const fs::path& dir) {
  if(!dll.is_absolute()||!dir.is_absolute()||!fs::create_directory(dir)) { return 2; }
  Log log(dir/"events.jsonl");Runtime r;bool passed=false;std::string failed,error;
  try { initializeRuntime(r,log,dll);passed=true; }catch(const std::exception& e) { failed=log.stage;error=e.what(); }
  r.cleanup(log);std::ofstream out(dir/"status.json");out<<"{\"identity\":"<<jsonString(identity)<<",\"api_probe\":"<<jsonString(passed?"pass":"fail")
    <<",\"gpu_execution\":\"unrun\",\"failed_at\":"<<jsonString(failed)<<",\"error\":"<<jsonString(error)<<"}\n";return passed?0:1;
}
struct Buffers {
  ComPtr<IDirect3DVertexBuffer9> vb;ComPtr<IDirect3DIndexBuffer9> ib;
  Buffers()=default;Buffers(const Buffers&)=delete;Buffers& operator=(const Buffers&)=delete;
};
void upload(Runtime& r,const replay::Part& p,Buffers& b,Log& log) {
  log.hr("create_vb",r.device->CreateVertexBuffer(UINT(p.vertices.size()*32),0,0,D3DPOOL_DEFAULT,&b.vb.value,nullptr));
  log.hr("create_ib",r.device->CreateIndexBuffer(UINT(p.indices.size()*4),0,D3DFMT_INDEX32,D3DPOOL_DEFAULT,&b.ib.value,nullptr));
  void* data=nullptr;log.hr("lock_vb",b.vb.value->Lock(0,0,&data,0));
  if(!data) { throw std::runtime_error("null vertex upload"); }std::memcpy(data,p.vertices.data(),p.vertices.size()*32);log.hr("unlock_vb",b.vb.value->Unlock());
  log.hr("lock_ib",b.ib.value->Lock(0,0,&data,0));if(!data) { throw std::runtime_error("null index upload"); }
  std::memcpy(data,p.indices.data(),p.indices.size()*4);log.hr("unlock_ib",b.ib.value->Unlock());
  for(unsigned kind=0;kind<2;++kind) {
    const auto size=kind?p.indices.size()*4:p.vertices.size()*32;
    const auto* expected=kind?static_cast<const void*>(p.indices.data()):static_cast<const void*>(p.vertices.data());
    log.hr("lock_uploaded_readonly",kind?b.ib.value->Lock(0,0,&data,D3DLOCK_READONLY):b.vb.value->Lock(0,0,&data,D3DLOCK_READONLY));
    const bool equal=data&&!std::memcmp(data,expected,size);
    log.hr("unlock_uploaded_readonly",kind?b.ib.value->Unlock():b.vb.value->Unlock());
    log.result("uploaded_bytes_equal",equal?0:ERROR_INVALID_DATA);
  }
}
void finalColor(Runtime& r,const fs::path& path,Log& log) {
  ComPtr<IDirect3DSurface9> gpu,cpu;
  log.hr("get_backbuffer",r.device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&gpu.value));
  log.hr("create_color_readback",r.device->CreateOffscreenPlainSurface(640,400,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&cpu.value,nullptr));
  log.hr("read_color",r.device->GetRenderTargetData(gpu.value,cpu.value));D3DLOCKED_RECT locked{};
  log.hr("lock_color",cpu.value->LockRect(&locked,nullptr,D3DLOCK_READONLY));
  if(!locked.pBits||locked.Pitch<2560) { cpu.value->UnlockRect();throw std::runtime_error("invalid color pitch"); }
  std::ofstream out(path,std::ios::binary);out<<"P6\n640 400\n255\n";
  for(unsigned y=0;y<400;++y) { const auto* row=static_cast<const uint8_t*>(locked.pBits)+size_t(y)*size_t(locked.Pitch);
    for(unsigned x=0;x<640;++x) { out.put(char(row[x*4+2]));out.put(char(row[x*4+1]));out.put(char(row[x*4])); } }
  out.close();log.hr("unlock_color",cpu.value->UnlockRect());if(!out) { throw std::runtime_error("save color"); }
}
void gbuffer(Runtime& r,const fs::path& dir,unsigned kind,Log& log) {
  ComPtr<IDirect3DSurface9> gpu,cpu;
  log.hr("create_gbuffer_target",r.device->CreateRenderTarget(640,400,D3DFMT_A32B32G32R32F,D3DMULTISAMPLE_NONE,0,FALSE,&gpu.value,nullptr));
  log.hr("clear_gbuffer_target",r.device->ColorFill(gpu.value,nullptr,0));
  log.result("copy_gbuffer",uint32_t(r.api.dxvk_CopyRenderingOutput(gpu.value,remixapi_dxvk_CopyRenderingOutputType(kind))));
  log.hr("create_gbuffer_readback",r.device->CreateOffscreenPlainSurface(640,400,D3DFMT_A32B32G32R32F,D3DPOOL_SYSTEMMEM,&cpu.value,nullptr));
  log.hr("read_gbuffer",r.device->GetRenderTargetData(gpu.value,cpu.value));D3DLOCKED_RECT locked{};
  log.hr("lock_gbuffer",cpu.value->LockRect(&locked,nullptr,D3DLOCK_READONLY));
  if(!locked.pBits||locked.Pitch<640*16) { cpu.value->UnlockRect();throw std::runtime_error("invalid gbuffer pitch"); }
  std::ofstream out(dir/(kind==1?"depth.rgba32f":"normals.rgba32f"),std::ios::binary);
  for(unsigned y=0;y<400;++y) { out.write(static_cast<const char*>(locked.pBits)+size_t(y)*size_t(locked.Pitch),640*16); }
  out.close();log.hr("unlock_gbuffer",cpu.value->UnlockRect());if(!out) { throw std::runtime_error("save gbuffer"); }
}
struct Material {
  remixapi_MaterialInfo info{};remixapi_MaterialInfoOpaqueEXT opaque{};remixapi_MaterialInfoTranslucentEXT glass{};
  explicit Material(const std::string& name,const wchar_t* checker) {
    info.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;info.hash=0x4c444d0001;
    opaque=opaqueTriangleMaterial();opaque.albedoConstant={0.55f,0.55f,0.55f};opaque.roughnessConstant=0.8f;
    info.pNext=&opaque;
    if(name=="metal") { opaque.metallicConstant=1;opaque.roughnessConstant=0.15f; }
    else if(name=="rough") { opaque.roughnessConstant=1; }
    else if(name=="emissive") { info.emissiveIntensity=3;info.emissiveColorConstant={0.8f,0.2f,0.05f}; }
    else if(name=="cutout") { info.albedoTexture=checker;opaque.alphaTestType=4;opaque.alphaReferenceValue=127; }
    else if(name=="alpha") { opaque.opacityConstant=0.4f;opaque.blendType_hasvalue=TRUE;opaque.blendType_value=0; }
    else if(name=="additive") { opaque.opacityConstant=0.4f;opaque.blendType_hasvalue=TRUE;opaque.blendType_value=6;info.emissiveIntensity=1;info.emissiveColorConstant={0.2f,0.5f,0.8f}; }
    else if(name=="glass") { glass.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;glass.refractiveIndex=1.5f;
      glass.transmittanceColor={0.65f,0.85f,0.95f};glass.transmittanceMeasurementDistance=1;glass.thinWallThickness_hasvalue=TRUE;glass.thinWallThickness_value=0.1f;info.pNext=&glass; }
    else if(name!="neutral") { throw std::runtime_error("unknown material"); }
  }
};
struct ApiPart { std::vector<remixapi_HardcodedVertex> vertices;std::vector<uint32_t> indices;remixapi_MeshHandle mesh=nullptr; };
void apiGeometry(const replay::Part& source,ApiPart& target,bool safeUv,uint64_t& repaired) {
  target.vertices.resize(source.indices.size());target.indices.resize(source.indices.size());
  for(size_t at=0;at<source.indices.size();at+=3) {
    const auto& a=source.vertices[source.indices[at]];const auto& b=source.vertices[source.indices[at+1]];const auto& c=source.vertices[source.indices[at+2]];
    const auto n=replay::normal(a,b,c);const bool repair=safeUv&&replay::degenerateUv(a,b,c);if(repair) { ++repaired; }
    const float uv[3][2]={{0,0},{1,0},{0,1}};
    for(unsigned i=0;i<3;++i) { auto& v=target.vertices[at+i];const auto& input=source.vertices[source.indices[at+i]];
      std::memcpy(v.position,input.position,12);std::memcpy(v.normal,n.data(),12);std::memcpy(v.texcoord,repair?uv[i]:input.uv,8);
      v.color=0xffffffff;target.indices[at+i]=uint32_t(at+i); }
  }
}
int run(int argc,wchar_t** argv) {
  if(argc!=7) { std::cerr<<"usage: runner runtime.dll scene.bin NEW-output route material checker.dds\n";return 2; }
  const fs::path dll(argv[1]),input(argv[2]),dir(argv[3]);const std::string route=fs::path(argv[4]).string(),materialName=fs::path(argv[5]).string();
  if(!dll.is_absolute()||!input.is_absolute()||!dir.is_absolute()||!fs::create_directory(dir)) { return 2; }
  crashPath=(dir/"crash.json").wstring();dumpPath=(dir/"crash.dmp").wstring();SetUnhandledExceptionFilter(crashReport);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  Log log(dir/"events.jsonl");Runtime r;bool complete=false;std::string error,failed;
  remixapi_MaterialHandle material=nullptr;std::vector<std::unique_ptr<Buffers>> persistent;
  std::vector<ApiPart> apiParts;ComPtr<IDirect3DVertexDeclaration9> decl;ComPtr<IDirect3DTexture9> texture;
  const bool api=route=="api"||route=="api-safe-uv",raster=route=="raster";
  uint64_t repaired=0;unsigned frames=0;
  try {
    if(!api&&!raster&&route!="persistent"&&route!="retire-present"&&route!="ephemeral") { throw std::runtime_error("unknown route"); }
    std::ifstream stream(input,std::ios::binary);const auto scene=replay::load(stream);
    log.output<<"{\"identity\":"<<jsonString(identity)<<",\"route\":"<<jsonString(route)<<",\"material\":"<<jsonString(materialName)<<",\"parts\":"<<scene.parts.size()<<"}\n";
    initializeRuntime(r,log,dll);
    const std::pair<const char*,const char*> options[]={
      {"rtx.enableRaytracing",raster?"False":"True"},{"rtx.useVertexCapture","False"},{"rtx.useBuffersDirectly","False"},{"rtx.enablePreservePath","False"},
      {"rtx.upscalerType","0"},{"rtx.enableRayReconstruction","False"},{"rtx.enableAlphaBlend","True"},{"rtx.enableAlphaTest","True"},
      {"rtx.sceneScale","0.3937007874"},{"rtx.fallbackLightMode","2"},{"rtx.fallbackLightType","1"},
      {"rtx.fallbackLightRadiance","3000,3000,3000"},{"rtx.fallbackLightRadius","5"},{"rtx.fallbackLightPositionOffset","0,0,0"},
      {"rtx.autoExposure.enabled","True"},{"rtx.autoExposure.evMinValue","-2"},{"rtx.autoExposure.evMaxValue","5"},
      {"rtx.tonemappingMode","1"},{"rtx.localtonemap.exposure","0.75"},{"rtx.bloom.enable","False"},{"rtx.postfx.enable","False"}};
    for(const auto& o:options) { log.result(o.first,uint32_t(r.api.SetConfigVariable(o.first,o.second))); }
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"LdbFrozenReplay";
    if(!RegisterClassW(&wc)) { throw std::runtime_error("register window"); }RECT bounds{0,0,640,400};AdjustWindowRect(&bounds,WS_OVERLAPPEDWINDOW,FALSE);
    r.window=CreateWindowW(wc.lpszClassName,L"LDB GPU replay",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,wc.hInstance,nullptr);
    if(!r.window) { throw std::runtime_error("create window"); }ShowWindow(r.window,SW_SHOW);
    if(api) { log.result("create_api_d3d9",uint32_t(r.api.dxvk_CreateD3D9(FALSE,&r.d3d))); }
    else { const auto createPointer=GetProcAddress(r.module,"Direct3DCreate9Ex");using Create=HRESULT(WINAPI*)(UINT,IDirect3D9Ex**);Create create=nullptr;std::memcpy(&create,&createPointer,sizeof(create));
      log.result("find_legacy_d3d9",create?0:ERROR_PROC_NOT_FOUND);log.hr("create_legacy_d3d9",create(D3D_SDK_VERSION,&r.d3d)); }
    if(!r.d3d) { throw std::runtime_error("null d3d9"); }D3DADAPTER_IDENTIFIER9 adapter{};log.hr("adapter",r.d3d->GetAdapterIdentifier(0,0,&adapter));
    log.output<<"{\"vendor_id\":"<<adapter.VendorId<<",\"device_id\":"<<adapter.DeviceId<<"}\n";
    D3DPRESENT_PARAMETERS pp{};pp.BackBufferWidth=640;pp.BackBufferHeight=400;pp.BackBufferFormat=D3DFMT_A8R8G8B8;pp.BackBufferCount=1;
    pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=r.window;pp.Windowed=TRUE;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;pp.EnableAutoDepthStencil=TRUE;pp.AutoDepthStencilFormat=D3DFMT_D24S8;
    log.hr("create_device",r.d3d->CreateDeviceEx(0,D3DDEVTYPE_HAL,r.window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,nullptr,&r.device));
    log.result("register_device",uint32_t(r.api.dxvk_RegisterD3D9Device(r.device)));r.registered=true;
    if(api) {
      Material m(materialName,argv[6]);log.result("create_material",uint32_t(r.api.CreateMaterial(&m.info,&material)));apiParts.resize(scene.parts.size());
      for(size_t i=0;i<scene.parts.size();++i) { auto& p=apiParts[i];apiGeometry(scene.parts[i],p,route=="api-safe-uv",repaired);
        remixapi_MeshInfoSurfaceTriangles surface{};surface.vertices_values=p.vertices.data();surface.vertices_count=p.vertices.size();surface.indices_values=p.indices.data();surface.indices_count=p.indices.size();surface.material=material;
        remixapi_MeshInfo mesh{};mesh.sType=REMIXAPI_STRUCT_TYPE_MESH_INFO;mesh.hash=0x4c44470000+i;mesh.surfaces_values=&surface;mesh.surfaces_count=1;
        log.result("create_mesh",uint32_t(r.api.CreateMesh(&mesh,&p.mesh))); }
    } else {
      const D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0},{0,12,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
      log.hr("create_declaration",r.device->CreateVertexDeclaration(elements,&decl.value));
      log.hr("create_neutral_texture",r.device->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture.value,nullptr));
      D3DLOCKED_RECT locked{};log.hr("lock_texture",texture.value->LockRect(0,&locked,nullptr,0));if(!locked.pBits) { throw std::runtime_error("null texture"); }
      *static_cast<uint32_t*>(locked.pBits)=0xffbdbdbd;log.hr("unlock_texture",texture.value->UnlockRect(0));
      if(route=="persistent"||raster) { for(const auto& p:scene.parts) { auto b=std::make_unique<Buffers>();upload(r,p,*b,log);persistent.push_back(std::move(b)); } }
    }
    log.output<<"{\"uv_repaired_triangles\":"<<repaired<<",\"api_flat_normals\":"<<(api?"true":"false")<<"}\n"<<std::flush;
    const auto started=std::chrono::steady_clock::now();
    for(unsigned f=1;f<=64;++f) {
      if(std::chrono::steady_clock::now()-started>std::chrono::seconds(120)) { throw std::runtime_error("frame time budget"); }
      MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message);DispatchMessageW(&message); }
      if(!IsWindow(r.window)) { throw std::runtime_error("window closed"); }
      log.hr("clear",r.device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xff000000,1,0));
      std::vector<std::unique_ptr<Buffers>> retired;
      if(api) {
        remixapi_CameraInfo camera{};camera.sType=REMIXAPI_STRUCT_TYPE_CAMERA_INFO;camera.type=REMIXAPI_CAMERA_TYPE_WORLD;
        std::memcpy(camera.view,scene.view.data(),64);std::memcpy(camera.projection,scene.projection.data(),64);log.result("setup_camera",uint32_t(r.api.SetupCamera(&camera)));
        for(size_t i=0;i<scene.parts.size();++i) { remixapi_InstanceInfo instance{};instance.sType=REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;instance.mesh=apiParts[i].mesh;instance.doubleSided=TRUE;
          for(unsigned row=0;row<3;++row) { for(unsigned col=0;col<4;++col) { instance.transform.matrix[row][col]=scene.parts[i].world[col*4+row]; } }
          log.result("draw_instance",uint32_t(r.api.DrawInstance(&instance))); }
      } else {
        log.hr("begin_scene",r.device->BeginScene());log.hr("set_declaration",r.device->SetVertexDeclaration(decl.value));
        log.hr("set_vs",r.device->SetVertexShader(nullptr));log.hr("set_ps",r.device->SetPixelShader(nullptr));
        const std::pair<D3DRENDERSTATETYPE,DWORD> rs[]={{D3DRS_LIGHTING,FALSE},{D3DRS_CULLMODE,D3DCULL_NONE},{D3DRS_ZENABLE,TRUE},{D3DRS_ZWRITEENABLE,TRUE},{D3DRS_ZFUNC,D3DCMP_LESSEQUAL},{D3DRS_ALPHATESTENABLE,FALSE},{D3DRS_ALPHABLENDENABLE,FALSE},{D3DRS_FOGENABLE,FALSE}};
        for(const auto& v:rs) { log.hr("render_state",r.device->SetRenderState(v.first,v.second)); }
        log.hr("set_texture",r.device->SetTexture(0,texture.value));log.hr("select_texture",r.device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));
        log.hr("texture_color",r.device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE));log.hr("alpha_op",r.device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1));
        log.hr("alpha_arg",r.device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE));log.hr("disable_stage1",r.device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE));
        log.hr("sampler_srgb",r.device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,TRUE));D3DMATRIX matrix{};
        std::memcpy(&matrix,scene.view.data(),64);log.hr("set_view",r.device->SetTransform(D3DTS_VIEW,&matrix));std::memcpy(&matrix,scene.projection.data(),64);log.hr("set_projection",r.device->SetTransform(D3DTS_PROJECTION,&matrix));
        for(size_t i=0;i<scene.parts.size();++i) { const auto& p=scene.parts[i];std::unique_ptr<Buffers> temporary;Buffers* b=nullptr;
          if(persistent.empty()) { temporary=std::make_unique<Buffers>();upload(r,p,*temporary,log);b=temporary.get(); }else { b=persistent[i].get(); }
          std::memcpy(&matrix,p.world.data(),64);log.hr("set_world",r.device->SetTransform(D3DTS_WORLD,&matrix));
          log.hr("set_stream",r.device->SetStreamSource(0,b->vb.value,0,32));log.hr("set_indices",r.device->SetIndices(b->ib.value));
          log.hr("draw_indexed",r.device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,UINT(p.vertices.size()),0,UINT(p.indices.size()/3)));
          log.hr("unbind_stream",r.device->SetStreamSource(0,nullptr,0,0));log.hr("unbind_indices",r.device->SetIndices(nullptr));
          if(route=="retire-present") { retired.push_back(std::move(temporary)); }
        }
        log.hr("end_scene",r.device->EndScene());
      }
      log.hr("present",r.device->PresentEx(nullptr,nullptr,nullptr,nullptr,0));++frames;
      if(f==48||f==64) { finalColor(r,dir/(f==48?"early.ppm":"final.ppm"),log); }
      if(f==64&&!raster) { gbuffer(r,dir,1,log);gbuffer(r,dir,2,log); }
      if(f%16==0) { std::cout<<route<<" / "<<materialName<<": "<<f<<"/64"<<std::endl; }
      MsgWaitForMultipleObjectsEx(0,nullptr,16,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
    complete=true;
  } catch(const std::exception& e) { failed=log.stage;error=e.what(); }
  // Save measurements before teardown: an exit hook crash must not hide images.
  { std::ofstream pending(dir/"status.json");pending<<"{\"schema\":1,\"identity\":"<<jsonString(identity)
      <<",\"execution\":"<<jsonString(complete?"complete":"fail")<<",\"image_acceptance\":\"unreviewed\",\"frames\":"<<frames
      <<",\"cleanup\":\"pending\",\"failed_at\":"<<jsonString(failed)<<",\"error\":"<<jsonString(error)<<"}\n"; }
  // Drop all COM children before the pinned Shutdown releases device ownership.
  persistent.clear();if(decl.value) { decl.value->Release();decl.value=nullptr; }if(texture.value) { texture.value->Release();texture.value=nullptr; }
  std::string cleanup="complete";
  try { for(const auto& p:apiParts) { if(p.mesh) { log.result("destroy_mesh",uint32_t(r.api.DestroyMesh(p.mesh))); } }
    if(material) { log.result("destroy_material",uint32_t(r.api.DestroyMaterial(material))); }r.cleanup(log);
  } catch(const std::exception& e) { cleanup="fail";if(error.empty()) { failed=log.stage;error=e.what(); }complete=false; }
  std::ofstream status(dir/"status.json");status<<"{\"schema\":1,\"identity\":"<<jsonString(identity)<<",\"execution\":"<<jsonString(complete?"complete":"fail")
    <<",\"image_acceptance\":\"unreviewed\",\"frames\":"<<frames<<",\"route\":"<<jsonString(route)<<",\"material\":"<<jsonString(materialName)<<",\"cleanup\":"<<jsonString(cleanup)<<",\"failed_at\":"<<jsonString(failed)<<",\"error\":"<<jsonString(error)<<"}\n";
  status.close();return complete&&status?0:1;
}
} // namespace replay_gpu
#ifndef RT_REPLAY_NO_MAIN
int wmain(int argc,wchar_t** argv) { try {
  if(argc==4&&!std::wcscmp(argv[1],L"--probe")) { return replay_gpu::probe(argv[2],argv[3]); }
  return replay_gpu::run(argc,argv);
}catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 2; } }
#endif
