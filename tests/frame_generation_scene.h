#pragma once
inline Com<IDirect3DSurface9> fgFixtureTarget;
inline void fgFixtureReset() { fgFixtureTarget.reset(); }
void fgFixtureScene(IDirect3DDevice9 *device, uint32_t frame, uint32_t width,
                    uint32_t height) {
  Com<IDirect3DSurface9> back;
  hr(device->GetRenderTarget(0, back.put()), "FG fixture RT");
  if (!fgFixtureTarget.get())
    hr(device->CreateRenderTarget(width, height, D3DFMT_A16B16G16R16F,
                                  D3DMULTISAMPLE_NONE, 0, FALSE,
                                  fgFixtureTarget.put(), nullptr),
       "FG fixture HDR");
  ldb_fg::boundary(device, frame);
  hr(device->SetRenderTarget(0, fgFixtureTarget.get()), "FG fixture bind HDR");
  D3DVIEWPORT9 full{0, 0, width, height, 0, 1};
  ldb_fg::viewport(device, &full, frame);
  hr(device->SetViewport(&full), "FG fixture viewport");
  ldb_fg::boundary(device, frame);
  hr(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                   D3DCOLOR_XRGB(16 + frame, 32, 120), 1, 0),
     "FG fixture clear");
  ldb_fg::clear(device, frame, S_OK);
  Com<IDirect3DVertexShader9> vs;
  const DWORD code[] = {
      0xfffe0200, 0x0010fffe, 0x42415443, 0x1c,       0,          0xfffe0200,
      1,          0x1c,       0,          0,          0x30,       0x40002,
      4,          0,          0,          0x65695663, 0x6f725077, 0x6a,
      0x0200001f, 0x80000000, 0x900f0000, 0x03000014, 0xc00f0000, 0x90e40000,
      0xa0e40004, 0xffff};
  hr(device->CreateVertexShader(code, vs.put()), "FG fixture VS");
  hr(device->SetVertexShader(vs.get()), "FG fixture bind VS");
  Com<IDirect3DVertexDeclaration9> declaration;
  const D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT4,
                                         D3DDECLMETHOD_DEFAULT,
                                         D3DDECLUSAGE_POSITION, 0},
                                        D3DDECL_END()};
  hr(device->CreateVertexDeclaration(elements, declaration.put()),
     "FG fixture declaration");
  hr(device->SetVertexDeclaration(declaration.get()),
     "FG fixture bind declaration");
  Com<IDirect3DPixelShader9> ps;
  const DWORD pixel[] = {0xffff0200, 0x02000001, 0x800f0800, 0xa0e40000,
                         0xffff};
  hr(device->CreatePixelShader(pixel, ps.put()), "FG fixture PS");
  hr(device->SetPixelShader(ps.get()), "FG fixture bind PS");
  struct Vertex {
    float x, y, z, w;
  };
  auto quad = [&](float x0, float y0, float x1, float y1, float z,
                  std::array<float, 4> color) {
    Vertex vertices[] = {
        {x0, y0, z, 1}, {x1, y0, z, 1}, {x0, y1, z, 1}, {x1, y1, z, 1}};
    hr(device->SetPixelShaderConstantF(0, color.data(), 1), "FG fixture color");
    ldb_fg::beforeDraw(device, frame);
    HRESULT result = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices,
                                             sizeof(Vertex));
    ldb_fg::afterDraw(device, frame, result);
    hr(result, "FG fixture Draw");
  };
  auto projection = [&](float shift) {
    ldb_camera::Matrix m{2.f / width, 0, 0, -1 + shift, 0, -2.f / height,
                         0,           1, 0, 0,          1, 0,
                         0,           0, 0, 1};
    hr(device->SetVertexShaderConstantF(4, m.data(), 4),
       "FG fixture projection");
  };
  hr(device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE), "FG fixture z");
  hr(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE), "FG fixture zw");
  hr(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL), "FG fixture zfunc");
  hr(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "FG fixture cull");
  hr(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE), "FG fixture blend");
  hr(device->BeginScene(), "FG fixture begin");
  projection(.02f * frame);
  quad(-.1f * width, .15f * height, 1.1f * width, float(height), .75f,
       {.08f, .3f, .12f, 1});
  quad(.2f * width, .2f * height, .35f * width, .65f * height, .5f,
       {.9f, .05f, .03f, 1});
  float objectShift = frame % 2 ? .025f * width : 0;
  quad(.55f * width + objectShift, .32f * height, .68f * width + objectShift,
       .55f * height, .4f,
       frame % 2 ? std::array<float, 4>{.1f, .9f, .2f, 1}
                 : std::array<float, 4>{.1f, .1f, .9f, 1});
  D3DVIEWPORT9 nearView{0, 0, width, height, 0, .1f};
  ldb_fg::viewport(device, &nearView, frame);
  hr(device->SetViewport(&nearView), "FG fixture near");
  bool mixedNear =
      ldb_fg::readEnv(L"LDB_FG_FIXTURE_NEAR_CASE") == L"no_depth_and_rebind";
  if (frame == 0)
    std::cout << "{\"event\":\"near_fixture_selection\",\"mixed\":"
              << (mixedNear ? "true" : "false") << ",\"env_size\":"
              << ldb_fg::readEnv(L"LDB_FG_FIXTURE_NEAR_CASE").size() << "}\n"
              << std::flush;
  if (mixedNear) {
    // A real near pass may clear before drawing and repeat a target boundary.
    ldb_fg::boundary(device, frame);
    HRESULT clear = device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1, 0);
    ldb_fg::clear(device, frame, clear);
    hr(clear, "FG fixture near clear");
    hr(device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE),
       "FG fixture near no depth writes");
  }
  projection(0);
  quad(.72f * width, .7f * height, .9f * width, .95f * height, .05f,
       {.9f, .9f, .08f, 1});
  if (mixedNear) {
    ldb_fg::boundary(device, frame);
    hr(device->SetRenderTarget(0, fgFixtureTarget.get()),
       "FG fixture same near target rebind");
    // SetRenderTarget resets the D3D9 viewport; re-enter the near range.
    ldb_fg::viewport(device, &nearView, frame);
    hr(device->SetViewport(&nearView), "FG fixture near viewport after rebind");
    quad(.73f * width, .73f * height, .87f * width, .9f * height, .04f,
         {.95f, .95f, .04f, 1});
    // Low-contrast stripes stay below the temporal rejection threshold. A
    // depth-only mask wrongly camera-warps this texture even if flat colour
    // samples happen to match.
    uint32_t stripeStart = uint32_t(width * .74f);
    for (uint32_t i = 0; i < 16; i++)
      quad(float(stripeStart + i * 4), height * .74f,
           float(stripeStart + (i + 1) * 4), height * .89f, .03f,
           {i % 2 ? .98f : .9f, .9f, .08f, 1});
  }
  ldb_fg::viewport(device, &full, frame);
  hr(device->SetViewport(&full), "FG fixture full");
  hr(device->EndScene(), "FG fixture end");
  ldb_fg::boundary(device, frame);
  hr(device->SetRenderTarget(0, back.get()), "FG fixture restore backbuffer");
  hr(device->StretchRect(fgFixtureTarget.get(), nullptr, back.get(), nullptr,
                         D3DTEXF_NONE),
     "FG fixture final color blit");
  hr(device->SetVertexShader(nullptr), "FG fixture fixed HUD");
  hr(device->SetFVF(D3DFVF_XYZRHW), "FG fixture HUD FVF");
  hr(device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE), "FG fixture HUD z");
  hr(device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE), "FG fixture HUD zw");
  hr(device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE),
     "FG fixture HUD blend");
  hr(device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE),
     "FG fixture HUD src");
  hr(device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO),
     "FG fixture HUD dst");
  hr(device->BeginScene(), "FG fixture HUD begin");
  quad(10, 10, 100, 35, 0, {.05f, .8f, .95f, 1});
  hr(device->EndScene(), "FG fixture HUD end");
  hr(device->SetPixelShader(nullptr), "FG fixture reset PS");
}
void fgFixtureCheck(IDirect3DDevice9 *device, uint32_t frame, uint32_t width,
                    uint32_t height, bool generated, uint32_t index) {
  Com<IDirect3DSurface9> back, read;
  hr(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back.put()),
     "FG fixture actual backbuffer");
  hr(device->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8,
                                         D3DPOOL_SYSTEMMEM, read.put(),
                                         nullptr),
     "FG fixture readback");
  hr(device->GetRenderTargetData(back.get(), read.get()),
     "FG fixture read actual present pixels");
  D3DLOCKED_RECT lock{};
  hr(read->LockRect(&lock, nullptr, D3DLOCK_READONLY), "FG fixture lock");
  auto rgb = [&](uint32_t x, uint32_t y) {
    auto p = static_cast<const uint8_t *>(lock.pBits) + size_t(y) * lock.Pitch +
             x * 4;
    return std::array<int, 3>{p[2], p[1], p[0]};
  };
  auto nearColor = rgb(uint32_t(width * .8f), uint32_t(height * .8f));
  auto ui = rgb(20, 20);
  auto dynamic =
      rgb(uint32_t(width * (.61f + .01f * frame)), uint32_t(height * .43f));
  if (ldb_fg::readEnv(L"LDB_FG_FIXTURE_NEAR_CASE") == L"no_depth_and_rebind") {
    uint32_t stripeStart = uint32_t(width * .74f);
    for (uint32_t i = 0; i < 16; i++) {
      auto pixel = rgb(stripeStart + i * 4 + 2, uint32_t(height * .8f));
      if (std::abs(pixel[0] - (i % 2 ? 250 : 230)) > 2 ||
          std::abs(pixel[1] - 230) > 2 || std::abs(pixel[2] - 20) > 2) {
        read->UnlockRect();
        throw std::runtime_error(
            "Near texture was camera-warped despite disabled depth writes");
      }
    }
  }
  uint32_t edge = width;
  for (uint32_t x = 0; x < width; x++) {
    auto c = rgb(x, uint32_t(height * .4f));
    if (c[0] > 180 && c[1] < 60 && c[2] < 30) {
      edge = x;
      break;
    }
  }
  read->UnlockRect();
  if (dynamic[0] > 35 || (frame % 2 ? (dynamic[1] < 220 || dynamic[2] > 65)
                                    : (dynamic[1] > 35 || dynamic[2] < 220)))
    throw std::runtime_error(
        "Changing object was blended or camera-warped instead of preserved");
  float expected = .2f * width +
                   .01f * float(width) * (float(frame) - (generated ? .5f : 0));
  if (std::abs(float(edge) - expected) > 2)
    throw std::runtime_error("Generated image was not the expected midpoint in "
                             "the actual backbuffer");
  if (nearColor[0] < 220 || nearColor[1] < 220 || nearColor[2] > 30)
    throw std::runtime_error(
        "Near weapon pixels were altered by frame generation");
  if (ui[0] > 20 || ui[1] < 195 || ui[2] < 235)
    throw std::runtime_error("UI pixels were altered by frame generation");
  std::cout << "{\"event\":\"fixture_present\",\"source_frame\":" << frame
            << ",\"present_index\":" << index
            << ",\"generated_verified\":" << (generated ? "true" : "false")
            << ",\"bar_edge\":" << edge << ",\"expected_bar_edge\":" << expected
            << ",\"ui_preserved\":true,\"near_preserved\":true,\"dynamic_"
               "preserved\":true}\n"
            << std::flush;
}
