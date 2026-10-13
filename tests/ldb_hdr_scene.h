#pragma once
// Real DXVK test pass: HDR values above one plus a depth-range/near-geometry change.
void hdrScene(IDirect3DDevice9* device, uint32_t frame, uint32_t width, uint32_t height) {
  if (halfFloat(0x0001) != std::ldexp(1.0f,-24) || halfFloat(0x7bff) != 65504.0f ||
      halfFloat(0xbc00) != -1.0f || !std::signbit(halfFloat(0x8000)) ||
      !std::isinf(halfFloat(0x7c00)) || !std::isnan(halfFloat(0x7e00)))
    throw std::runtime_error("Binary16 decoder edge cases failed");
  Com<IDirect3DSurface9> original, target;
  hr(device->GetRenderTarget(0, original.put()), "HDR original RT");
  hr(device->CreateRenderTarget(width,height,D3DFMT_A16B16G16R16F,D3DMULTISAMPLE_NONE,0,FALSE,target.put(),nullptr), "HDR CreateRenderTarget");
  hr(device->SetRenderTarget(0,target.get()), "HDR bind RT");
  hr(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,D3DCOLOR_XRGB(16,32,192),1.0f,0), "HDR Clear");
  Com<IDirect3DPixelShader9> shader;
  const DWORD code[] = {0xffff0200,0x02000001,0x800f0800,0xa0e40000,0x0000ffff}; // ps_2_0 mov oC0,c0
  hr(device->CreatePixelShader(code,shader.put()), "HDR pixel shader");
  hr(device->SetPixelShader(shader.get()), "HDR bind shader");
  hr(device->SetFVF(D3DFVF_XYZRHW), "HDR FVF");
  hr(device->SetVertexShader(nullptr), "HDR fixed vertex shader");
  hr(device->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE), "HDR ZENABLE");
  hr(device->SetRenderState(D3DRS_ZWRITEENABLE,TRUE), "HDR ZWRITE");
  hr(device->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESS), "HDR ZFUNC");
  hr(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE), "HDR CULL");
  hr(device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE), "HDR blend");
  hr(device->BeginScene(), "HDR BeginScene");
  struct Vertex { float x,y,z,w; };
  auto triangle = [&](float z,const std::array<float,4>& color) {
    Vertex vertices[]={{float(width)*0.2f,float(height)*0.8f,z,1},
                       {float(width)*0.5f,float(height)*0.2f,z,1},
                       {float(width)*0.8f,float(height)*0.8f,z,1}};
    hr(device->SetPixelShaderConstantF(0,color.data(),1), "HDR constant");
    const HRESULT result=device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,vertices,sizeof(Vertex));
    ldb_capture::draw(device,"HDR DrawPrimitiveUP",frame,result,1);
    hr(result,"HDR Draw");
  };
  triangle(0.25f,{4.0f,0.5f,0.25f,1.0f});
  D3DVIEWPORT9 nearView{0,0,width,height,0,0.1f};
  ldb_capture::viewportBefore(device,&nearView,frame);
  hr(device->SetViewport(&nearView), "HDR near viewport");
  triangle(0.05f,{0.125f,2.0f,0.5f,0.75f});
  D3DVIEWPORT9 fullView{0,0,width,height,0,1};
  ldb_capture::viewportBefore(device,&fullView,frame);
  hr(device->SetViewport(&fullView), "HDR restore viewport");
  hr(device->EndScene(), "HDR EndScene");
  ldb_capture::boundary(device,"SetRenderTarget",frame);
  hr(device->SetRenderTarget(0,original.get()), "HDR restore RT");
  hr(device->SetPixelShader(nullptr), "HDR restore pixel shader");
}
