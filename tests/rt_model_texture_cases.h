#pragma once
// Real DDS sizes which exceeded the old geometry-only SHA256 bound. Digests
// were computed independently with Python hashlib over the complete DDS.
inline void textureHashCases() {
  struct Case { UINT width,height;D3DFORMAT format;const char* expected; };
  const Case cases[]={
    {1024,1024,D3DFMT_A8R8G8B8,"4b0f1caecc8f94d8b43e16316623813297826893aeeaea5e53cbad7f71ef7b79"},
    {2048,2048,D3DFMT_DXT5,"842781c2bb6a084f5e4f19c479543c67519e624303462e4a28acc99e191a7756"},
    {4096,4096,D3DFMT_DXT5,"910a6afc10dd3b2919d17ad96a4f9f84be8f30e629e891078daffce32501a5b4"},
    {2048,2048,D3DFMT_A8R8G8B8,"43ebea6a3b99f4309a45601af2bf56d5e002980159170ae6ec5674d97fee3d0e"}
  };
  for(const auto& c:cases) {
    const auto layout=model_texture::layout(c.width,c.height,c.format,false);
    std::vector<uint8_t> source(size_t(layout.bytes()),42);
    auto encoded=model_texture::dds(c.width,c.height,layout,source.data(),int(layout.rowBytes));
    require(encoded.size()==148+layout.bytes(),"large DDS missing header or payload");
    require(ldb_rt_file::geometrySha256(encoded.data(),encoded.size()).empty(),"geometry hash safety bound unexpectedly enlarged");
    require(model_texture::ddsSha256(encoded)==c.expected,"full-size texture hash differs from independent digest");
    encoded.back()^=1;require(model_texture::ddsSha256(encoded)!=c.expected,"texture hash omitted final payload byte");
  }
  std::vector<uint8_t> oversized(model_texture::kMaxDdsBytes+1,42);
  require(model_texture::ddsSha256(oversized).empty()&&model_texture::ddsSha256({}).empty(),"texture hash budget not enforced");
  std::cout<<"RT model texture full DDS SHA256 4MiB/16MiB plus header/bounds: PASS\n";
}

inline void systemTextureExport() {
  struct Library { HMODULE module=LoadLibraryExW(L"d3d9.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    ~Library() { if(module) { FreeLibrary(module); } } } library;
  require(library.module!=nullptr,"texture fixture system D3D9 unavailable");
  const auto address=GetProcAddress(library.module,"Direct3DCreate9");
  using Create=IDirect3D9* (WINAPI*)(UINT);Create create=nullptr;
  static_assert(sizeof(create)==sizeof(address),"function address size");std::memcpy(&create,&address,sizeof(create));
  require(create!=nullptr,"texture fixture D3D9 export missing");
  static_draw::Ref<IDirect3D9> factory;factory.value=create(D3D_SDK_VERSION);require(factory.value!=nullptr,"texture fixture D3D9 factory");
  struct Window { HWND handle=CreateWindowExW(0,L"STATIC",L"LDB texture export check",WS_OVERLAPPED,0,0,320,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ~Window() { if(handle) { DestroyWindow(handle); } } } window;
  require(window.handle!=nullptr,"texture fixture window");
  D3DPRESENT_PARAMETERS pp{};pp.BackBufferWidth=320;pp.BackBufferHeight=240;pp.BackBufferFormat=D3DFMT_A8R8G8B8;pp.BackBufferCount=1;
  pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window.handle;pp.Windowed=TRUE;
  static_draw::Ref<IDirect3DDevice9> device;
  require(SUCCEEDED(factory.value->CreateDevice(0,D3DDEVTYPE_NULLREF,window.handle,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device.value)),"texture fixture NULLREF required");
  auto* d=device.value;
  const auto check=[](HRESULT hr) { require(SUCCEEDED(hr),"texture fixture D3D9 operation"); };
  check(d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE));check(d->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_ALWAYS));check(d->SetRenderState(D3DRS_ALPHAREF,0));
  check(d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE));check(d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP));check(d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
  check(d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR));check(d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR));
  wchar_t temp[MAX_PATH]{};const auto length=GetTempPathW(MAX_PATH,temp);require(length&&length<MAX_PATH,"texture fixture temporary directory");
  struct Directory { std::filesystem::path path;
    ~Directory() { std::error_code ignored;std::filesystem::remove_all(path,ignored); } } directory{
      std::filesystem::path(temp)/(L"ldb-texture-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()))};
  require(std::filesystem::create_directory(directory.path),"texture fixture directory already exists");
  model_texture::Cache cache;uint64_t expectedBytes=0,at=181;
  for(UINT dimension:std::array<UINT,2>{{1024,2048}}) {
    static_draw::Ref<IDirect3DTexture9> texture;
    check(d->CreateTexture(dimension,dimension,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture.value,nullptr));
    D3DLOCKED_RECT locked{};check(texture.value->LockRect(0,&locked,nullptr,0));
    for(UINT y=0;y<dimension;++y) { std::memset(static_cast<uint8_t*>(locked.pBits)+size_t(y)*size_t(locked.Pitch),42,size_t(dimension)*4); }
    check(texture.value->UnlockRect(0));check(d->SetTexture(0,texture.value));
    const auto source=cache.acquire(d,directory.path,at,1);
    const char* expected=dimension==1024?"4b0f1caecc8f94d8b43e16316623813297826893aeeaea5e53cbad7f71ef7b79":"43ebea6a3b99f4309a45601af2bf56d5e002980159170ae6ec5674d97fee3d0e";
    require(source.textured()&&source.digest==expected&&ldb_rt_file::fileSha256(source.path)==expected,"actual texture cache export/hash failed");
    expectedBytes+=uint64_t(dimension)*dimension*4+148;
    const auto again=cache.acquire(d,directory.path,at,1);
    require(again.key==source.key&&cache.bytes==expectedBytes&&cache.exported==at-180&&!cache.failed,"cache hit reread texture or changed digest");
    // A new write lock verifies the read-only export did not leave a lock held.
    check(texture.value->LockRect(0,&locked,nullptr,0));check(texture.value->UnlockRect(0));
    check(d->SetTexture(0,nullptr));++at;
  }
  require(cache.held()==2,"texture source ownership lost");
  static_draw::Ref<IDirect3DTexture9> chain;
  check(d->CreateTexture(8,4,0,0,D3DFMT_DXT5,D3DPOOL_MANAGED,&chain.value,nullptr));
  require(chain.value->GetLevelCount()==4,"source mip count lost");
  for(UINT level=0;level<4;++level) {
    D3DSURFACE_DESC desc{};check(chain.value->GetLevelDesc(level,&desc));
    const auto layout=model_texture::layout(desc.Width,desc.Height,desc.Format,true);
    D3DLOCKED_RECT lock{};check(chain.value->LockRect(level,&lock,nullptr,0));
    for(UINT y=0;y<layout.rows;++y) { std::memset(static_cast<uint8_t*>(lock.pBits)+size_t(y)*size_t(lock.Pitch),int(42+level),layout.rowBytes); }
    check(chain.value->UnlockRect(level));
  }
  check(d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,TRUE));check(d->SetTexture(0,chain.value));
  const auto mipped=cache.acquire(d,directory.path,at,1);
  require(mipped.textured()&&mipped.levels==4&&mipped.fullMipLevels==4&&mipped.digest=="80fc995ed673b906c9df5fde6439fd94e2c11c70ae151507e3cd8cd99d3a5bde","source mip chain differs from independent digest");
  require(ldb_rt_file::fileSha256(mipped.path)==mipped.digest&&std::filesystem::file_size(mipped.path)==228,"source mip disk payload differs");
  struct FailedMip {
    IDirect3DTexture9* inner;unsigned locks=0,unlocks=0;
    HRESULT GetLevelDesc(UINT level,D3DSURFACE_DESC* desc) { return inner->GetLevelDesc(level,desc); }
    HRESULT LockRect(UINT level,D3DLOCKED_RECT* rect,const RECT* area,DWORD flags) { ++locks;return level==1?E_FAIL:inner->LockRect(level,rect,area,flags); }
    HRESULT UnlockRect(UINT level) { ++unlocks;return inner->UnlockRect(level); }
  } failed{chain.value};
  D3DSURFACE_DESC base{};check(chain.value->GetLevelDesc(0,&base));
  require(model_texture::mipDds(&failed,base,true,4).empty()&&failed.locks==2&&failed.unlocks==1,"partial mip failure leaked lock or accepted truncated chain");
  require(model_texture::mipBytes(4096,4096,D3DFMT_DXT5,true,13)>16u*1024*1024&&!model_texture::mipBytes(8,4,D3DFMT_DXT5,true,5),"full chain size or overlong count accepted");
  check(d->SetTexture(0,nullptr));cache.clear();require(!cache.held(),"texture references not cleared");
  {
    model_texture::Cache::Limits policy;policy.resident=300;policy.spoolBytes=1024;policy.evidence=212;policy.refs=2;
    model_texture::Cache working(policy);static_draw::Ref<IDirect3DTexture9> a,b;
    check(d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE));
    check(d->CreateTexture(4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&a.value,nullptr));
    check(d->CreateTexture(8,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&b.value,nullptr));
    for(auto* texture:{a.value,b.value}) { D3DLOCKED_RECT lock{};D3DSURFACE_DESC desc{};check(texture->GetLevelDesc(0,&desc));check(texture->LockRect(0,&lock,nullptr,0));for(UINT y=0;y<desc.Height;++y) { std::memset(static_cast<uint8_t*>(lock.pBits)+size_t(y)*size_t(lock.Pitch),73,desc.Width*4); }check(texture->UnlockRect(0)); }
    check(d->SetTexture(0,a.value));auto first=working.acquire(d,directory.path,181,1);
    require(first.textured()&&first.evidenceSaved&&working.residentBytes==212,"working cache first export/evidence");
    check(d->SetTexture(0,b.value));working.completedThrough(180);auto pending=working.acquire(d,directory.path,184,1);
    require(!pending.textured()&&working.held()==1&&!working.evicted,"working cache evicted uncompleted source");
    working.completedThrough(181);auto second=working.acquire(d,directory.path,184,1);
    require(second.textured()&&!second.evidenceSaved&&working.evicted==1&&working.held()==1&&working.residentBytes==276&&working.bytes==488,"working eviction or independent evidence budget");
    require(std::filesystem::is_regular_file(first.path)&&std::filesystem::is_regular_file(second.path),"async runtime backing file deleted");
    check(d->SetTexture(0,a.value));working.completedThrough(184);auto reused=working.acquire(d,directory.path,186,1);
    require(reused.textured()&&reused.digest==first.digest&&reused.evidenceSaved&&working.exported==2&&working.bytes==488&&working.evicted==2,"working cache repeated content charged twice");
    check(d->SetTexture(0,nullptr));working.clear();require(working.residentBytes==0&&!working.held(),"working cache source release");
    std::cout<<"RT texture working-set completed idle eviction/spool dedup/bounded evidence/backing lifetime: PASS\n";
  }
  std::cout<<"RT model source mip chain NULLREF/hash/sRGB/small BC levels/partial failure unlock: PASS\n";
  std::cout<<"RT model texture system NULLREF 1024/2048 cache export/rehash/reuse/unlock/clear: PASS; GPU RT unrun\n";
}
