#define RT_REPLAY_NO_MAIN
#include "runner.cpp"
void demand(bool value,const char* text) { if(!value) { throw std::runtime_error(text); } }
int wmain(int argc,wchar_t** argv) {
  try {
    demand(argc==2,"output folder required");const fs::path dir(argv[1]);demand(fs::create_directory(dir),"new output required");Log log(dir/"events.jsonl");
    replay::Part p;p.vertices={{{0,0,0},{0,0},{0,0,0}},{{1,0,0},{0,0},{0,0,0}},{{0,1,0},{0,0},{0,0,0}}};p.indices={0,1,2};
    replay_gpu::ApiPart output;uint64_t repairs=0;replay_gpu::apiGeometry(p,output,true,repairs);
    demand(repairs==1&&output.indices.size()==3,"safe-UV repair count");
    for(unsigned i=0;i<3;++i) { demand(!std::memcmp(p.vertices[i].position,output.vertices[i].position,12),"safe-UV moved geometry");
      demand(output.vertices[i].normal[2]==1&&output.vertices[i].color==0xffffffff,"flat normal/color"); }
    demand(output.vertices[1].texcoord[0]==1&&output.vertices[2].texcoord[1]==1,"UV repair coordinates");
    replay_gpu::apiGeometry(p,output,false,repairs);demand(repairs==1&&output.vertices[1].texcoord[0]==0,"raw UV control modified");
    for(const auto* name:{"neutral","rough","metal","cutout","alpha","additive","emissive","glass"}) {
      replay_gpu::Material m(name,L"checker.dds");demand(m.info.pNext!=nullptr,"material extension missing");
      if(std::strcmp(name,"glass")) { demand(!m.opaque.useDrawCallAlphaState&&m.opaque.alphaTestType==(!std::strcmp(name,"cutout")?4:7),"API alpha enum wrong"); }
      if(!std::strcmp(name,"alpha")) { demand(m.opaque.blendType_hasvalue&&m.opaque.blendType_value==0&&m.opaque.opacityConstant==0.4f,"alpha material contract"); }
      if(!std::strcmp(name,"additive")) { demand(m.opaque.blendType_value==6,"additive material enum"); }
      if(!std::strcmp(name,"glass")) { demand(m.glass.refractiveIndex==1.5f&&m.glass.thinWallThickness_hasvalue,"glass material contract"); }
    }
    std::cout<<"REPLAY_API_INPUT_PASS: exact positions, raw/safe UV, flat normals and eight material contracts; GPU unrun\n";
    wchar_t system[MAX_PATH]{};demand(GetSystemDirectoryW(system,MAX_PATH)!=0,"system directory");
    Runtime r;r.module=LoadLibraryExW((fs::path(system)/L"d3d9.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);demand(r.module!=nullptr,"system d3d9");
    using Create=HRESULT(WINAPI*)(UINT,IDirect3D9Ex**);Create create=nullptr;const auto pointer=GetProcAddress(r.module,"Direct3DCreate9Ex");std::memcpy(&create,&pointer,sizeof(create));demand(create!=nullptr,"system D3D9Ex factory");log.hr("create_nullref_d3d9",create(D3D_SDK_VERSION,&r.d3d));
    r.window=CreateWindowExW(0,L"STATIC",L"replay-native",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);demand(r.window!=nullptr,"native window");
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=r.window;
    log.hr("create_nullref_device",r.d3d->CreateDeviceEx(0,D3DDEVTYPE_NULLREF,r.window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,nullptr,&r.device));
    { replay_gpu::Buffers buffers;replay_gpu::upload(r,p,buffers,log);log.hr("bind_vb",r.device->SetStreamSource(0,buffers.vb.value,0,32));log.hr("bind_ib",r.device->SetIndices(buffers.ib.value));
      ComPtr<IDirect3DVertexBuffer9> vb;ComPtr<IDirect3DIndexBuffer9> ib;UINT offset=0,stride=0;
      log.hr("read_vb",r.device->GetStreamSource(0,&vb.value,&offset,&stride));log.hr("read_ib",r.device->GetIndices(&ib.value));
      demand(vb.value==buffers.vb.value&&ib.value==buffers.ib.value&&!offset&&stride==32,"native upload/binding");
      log.hr("unbind_vb",r.device->SetStreamSource(0,nullptr,0,0));log.hr("unbind_ib",r.device->SetIndices(nullptr)); }
    r.cleanup(log);std::cout<<"REPLAY_SYSTEM_NULLREF_PASS: production VB/IB upload, readonly equality, bindings, cleanup; Vulkan RT unrun\n";return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
